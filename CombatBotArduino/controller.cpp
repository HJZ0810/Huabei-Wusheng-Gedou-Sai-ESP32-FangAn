/**
 * @file    controller.cpp
 * @brief   四驱底盘运动状态机、闭环控制与软件停止协调。
 *
 * ============================================================================
 * 模块职责
 *   统一采集轮脉冲和传感器，完成位置/航向/轮速控制、标定及遥测发布。
 *   运动任务拥有模式、PID 和电机输出；网络侧通过队列与原子门控投递请求。
 *
 * 运行约定
 *   轮序：左前、右前、左后、右后；距离 cm，速度 cm/s，偏航 °，角速度 °/s。
 *   控制任务以 10 ms 为目标周期；同步总线、存储和调度可能使周期超时。
 *
 * 设计边界
 *   软件停止撤去 PWM，无法替代硬件急停或主动制动。队列受理不等于动作完成。
 *   编码器与陀螺仪提供估计量；容差判据不承诺装机后的实际距离与转角精度。
 * ============================================================================
 */
#include "controller.h"
#include "config.h"
#include "hardware.h"
#include "pid.h"
#include "kinematics.h"
#include "safety.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <atomic>
#include <cstring>

namespace bot {
namespace {
// 开发模式仍限制最终 PWM 输出，不依赖速度 PID 或死区补偿间接限速。
constexpr float DevelopmentPwmLimit=180.0f;
constexpr uint32_t DevelopmentDriveLimitMs=15000;
// ============================================================================
// 控制任务私有状态与跨任务门控
// ============================================================================
// 除原子门控、命令队列和发布快照外，本命名空间的运动状态由 run() 独占。
enum class Mode:uint8_t { Idle,Drive,Move,Turn,OdoCal,TurnCal,ImuCal,ClimbObserve,Autonomous };
/** @brief 命令携带投递代次；停止后不再执行旧代次的排队命令。 */
struct Queued { Command command; uint32_t generation; };
QueueHandle_t queue=nullptr; ///< 网络侧生产、控制任务消费的有界命令队列。
// generation：停止代次；owner：占用运动的客户端 ID，0 表示无占用者。
// heartbeat：当前占用者最近一次有效心跳的 millis() 时间。所有权不承担身份认证。
std::atomic<uint32_t> generation{0}, owner{0}, heartbeat{0};
std::atomic<unsigned> pendingStop{0}; ///< 位0：停止；位1：急停锁定；位2：显式Stop允许开发驾驶重新准入。
// locked 拒绝运动，busy 表示动作或保存门控，continuous 允许同一占用者更新 Drive。
std::atomic<bool> locked{false}, busy{false}, continuous{false};
std::atomic<bool> climbObserveActive{false};
// 开发驾驶超时后必须先显式停车；连续摇杆包、空闲零输入和内部断线停车不能重开窗口。
std::atomic<bool> developmentDriveExpired{false};
// 0=可申请，1=控制任务正在消费命令，2=配置操作；只做短路径仲裁，不自旋等待。
std::atomic<unsigned> admission{0};
// 所有权释放与停止调用不是同一时刻；配置窗口必须等控制任务执行过 motorStop。
// 此标志只确认停止流程已运行；输出硬件健康独立门控，不能据此证明物理 PWM 归零。
std::atomic<bool> outputsStopped{true};
portMUX_TYPE snapshotMux=portMUX_INITIALIZER_UNLOCKED;
Telemetry published, t; ///< published 为受临界区保护的发布副本，t 为控制任务工作副本。
Mode mode=Mode::Idle;
Pid speedLoop[4], positionLoop[4], headingLoop;
int64_t lastPulses[4]={0}, startPulses[4]={0}; ///< 上周期计数与本动作起始计数，轮序：左前、右前、左后、右后。
uint32_t stallSince[4]={0}, actionStarted=0, actionTimeout=0, settleSince=0, quietSince=0;
// demand/actionSpeed 为 cm/s；driveX/Y 为归一化摇杆；target 随动作为 cm 或 °。
// 起点独立保存，避免累计里程和多圈偏航的历史值改变当前动作目标。
float demand[4]={0}, driveX=0, driveY=0, actionSpeed=0, target=0, startYaw=0, startOdo=0;
float yawDemand=0; ///< 受独立角加减速度约束的目标角速度，单位 °/s。
// actionWheelTarget 为单侧转向行程 cm；零偏与补偿的单位为 °/s。
// startTurnFactor 随结果发布，供外部实测标定沿用动作开始时的几何比例。
float startTurnFactor=1, actionWheelTarget=0, adaptiveBias=0, configuredBias=0;
float quietGyro[100]={0};
size_t quietGyroCount=0, quietGyroIndex=0;
double calSum=0, calSumSq=0;
uint32_t calSamples=0;
bool pendingBiasSave=false;
float newBias=0;
arena::ArenaModel arenaModel;
arena::Frame arenaFrame;
uint32_t arenaRevision=0;
uint32_t poseQuietStarted=0;
bool poseQuiet=false;
arena::Mode arenaDriveMode=arena::Mode::Idle;
bool directionalTurnCal=false;
uint32_t climbObserveStarted=0, climbObserveSupportChanges=0;
float climbObserveMaxTilt=0;
bool climbObserveGround[7]={false};

/** @brief 将本周期参数映射为无网络依赖的自主配置；保存只在空闲窗口进行。 */
void configureArena(const Config& c) {
  if(arenaRevision==c.revision) return;
  arena::Config a;
  a.armed=c.arenaEnabled && c.arenaCalibrated; a.climbEnabled=c.arenaClimbEnabled;
  a.outerSizeCm=c.arenaOuterCm; a.platformSizeCm=c.arenaPlatformCm;
  a.bodyRadiusCm=0.5f*std::sqrt(c.arenaBodyLengthCm*c.arenaBodyLengthCm+c.arenaBodyWidthCm*c.arenaBodyWidthCm);
  a.safetyMarginCm=c.arenaMarginCm; a.navigationSpeedCmS=c.arenaNavSpeed;
  a.searchSpeedCmS=c.arenaSearchSpeed; a.attackSpeedCmS=c.arenaPushSpeed;
  a.climbSpeedCmS=c.arenaClimbSpeed; a.taskLimitMs=c.arenaTaskMs; a.climbLimitMs=c.arenaClimbMs;
  a.maxYawDegS=std::min(60.0f,c.maxYawRate);
  a.sensorFrontCm=a.sensorRearCm=c.arenaBodyLengthCm/2+5;
  a.grayFrontCm=a.grayRearCm=c.arenaBodyLengthCm/2+1.5f;
  a.grayLeftCm=a.grayRightCm=std::max(1.0f,c.arenaBodyWidthCm/2-1);
  const float heading=arena::wrapDegrees(c.arenaEntryHeading);
  if(std::fabs(heading)<45) { a.entrySide=arena::EntrySide::West; a.entryOffsetCm=c.arenaEntryY; }
  else if(heading>=45 && heading<135) { a.entrySide=arena::EntrySide::South; a.entryOffsetCm=c.arenaEntryX; }
  else if(heading<=-45 && heading>-135) { a.entrySide=arena::EntrySide::North; a.entryOffsetCm=c.arenaEntryX; }
  else { a.entrySide=arena::EntrySide::East; a.entryOffsetCm=c.arenaEntryY; }
  arenaModel.configure(a); arenaRevision=c.revision;
}

// ============================================================================
// 状态转换、统一停止与结果发布
// ============================================================================
void text(char* dest,size_t n,const char* value) { snprintf(dest,n,"%s",value); }
bool precision() { return mode==Mode::Move || mode==Mode::Turn || mode==Mode::OdoCal || mode==Mode::TurnCal; }
bool moving() { return mode!=Mode::Idle && mode!=Mode::ImuCal && mode!=Mode::ClimbObserve; }
/** @brief 动作切换时清除 PID、速度需求和到位保持状态，避免旧积分影响新动作。 */
void resetLoops() {
  headingLoop.reset();
  yawDemand=0;
  for(int i=0;i<4;++i) { speedLoop[i].reset(); positionLoop[i].reset(); demand[i]=0; stallSince[i]=0; }
  settleSince=0;
}
/**
 * @brief   在控制任务中请求四轮零输出，释放占用者并回到空闲状态。
 * @param reason 可选故障文本；为空时保留现有文本。
 * @param latch 为 true 时保持急停锁定。
 * @note 输出 API 失败时仅能尽力撤驱动并关闭硬件准入；成功归零也不代表主动制动。
 *       实际余转与停车距离由硬件和负载决定。
 *       此函数不改变命令代次；需使旧命令失效的调用路径另行增加 generation。
 */
void stopMotion(const char* reason=nullptr,bool latch=false) {
  const auto arenaState=arenaModel.snapshot();
  if(arenaState.mode!=arena::Mode::Idle && arenaState.mode!=arena::Mode::Halted) arenaModel.cancel();
  t.arena=arenaModel.snapshot();
  arenaDriveMode=arena::Mode::Idle;
  motorStop(); outputsStopped.store(true); resetLoops(); mode=Mode::Idle;
  owner.store(0); busy.store(false); continuous.store(false);
  climbObserveActive.store(false);
  t.owner=0; t.imuCalibrating=false;
  if(latch) locked.store(true);
  t.estop=locked.load(); text(t.state,sizeof(t.state),t.estop?"locked":"idle");
  if(reason) text(t.fault,sizeof(t.fault),reason);
}
/** @brief 危险或故障路径：增加停止代次、撤驱动，并锁定后续运动。 */
void fault(const char* reason) {
  generation.fetch_add(1); stopMotion(reason,true);
}
/**
 * @brief   保存动作起点到当前的估计位移或转角，然后停止并发布完成结果。
 * @note 误差为 actual - target。完成仅表示满足软件到位判据，不代表外部实测精度。
 *       标定计数取四轮脉冲增量绝对值均值，原地转向时不会左右抵消。
 */
void result(const Config& c) {
  if(mode==Mode::OdoCal) {
    const float mean=absolutePulseTravel(t.pulses,startPulses);
    for(int i=0;i<4;++i) {
      const double delta=double(t.pulses[i]-startPulses[i]);
      // 直线均值到位不能证明四路 FG 都有反馈；断一路时拒绝形成可落库结果。
      if(delta*target<=0 || std::fabs(delta)<mean*0.5f) { fault("calibration_fg_incomplete"); return; }
    }
  }
  const bool angular=mode==Mode::Turn || mode==Mode::TurnCal;
  const char* type=mode==Mode::OdoCal?"odo_cal":mode==Mode::TurnCal?
    (directionalTurnCal?(target>0?"turn_left_cal":"turn_right_cal"):"turn_cal"):angular?"turn":"move";
  text(t.resultType,sizeof(t.resultType),type);
  t.target=target; t.actual=angular?t.yaw-startYaw:t.odo-startOdo;
  t.error=t.actual-t.target; t.resultReady=true; ++t.resultId;
  t.resultConfigRevision=c.revision; t.calibrationSessionId=hardwareBootId();
  t.calibrationPulses=absolutePulseTravel(t.pulses,startPulses);
  t.calibrationTurnFactor=startTurnFactor;
  t.progress=1;
  stopMotion(); text(t.state,sizeof(t.state),"done");
}
/** @brief 结束只读登台观测并发布传感记录；该结果不触发配置写入或高度结论。 */
void finishClimbObservation(const Config& c,uint32_t now) {
  t.resultReady=true; ++t.resultId; t.resultConfigRevision=c.revision; t.calibrationSessionId=hardwareBootId();
  text(t.resultType,sizeof(t.resultType),"climb_observe");
  t.calibrationDurationMs=std::min<uint32_t>(now-climbObserveStarted,60000);
  t.calibrationSupportChanges=climbObserveSupportChanges;
  t.calibrationMaxTiltDeg=climbObserveMaxTilt;
  t.calibrationPulses=absolutePulseTravel(t.pulses,startPulses);
  t.calibrationTurnFactor=1;
  t.target=float(t.calibrationDurationMs); t.actual=t.calibrationPulses/pulsesPerCm(c);
  t.error=t.calibrationMaxTiltDeg; t.progress=1;
  stopMotion(); text(t.state,sizeof(t.state),"climb_observe_done");
}
/** @brief 观测期间只采样，不产生轮速需求；传感器掉线或心跳超时则丢弃记录。 */
void updateClimbObservation(const Config& c,uint32_t now) {
  const Sensors& s=t.sensors;
  if(!hardwareOutputsHealthy() || !s.imuOk || timedOut(now,s.imuMs,80) || !s.accelOk ||
      !std::isfinite(s.acc[0]) || !std::isfinite(s.acc[1]) || !std::isfinite(s.acc[2]) ||
      !s.ioOk || timedOut(now,s.digitalMs,80)) {
    stopMotion("climb_observe_sensor_unavailable"); return;
  }
  if(timedOut(now,heartbeat.load(),c.heartbeatMs)) {
    stopMotion("climb_observe_disconnected"); return;
  }
  const float magnitude=std::sqrt(s.acc[0]*s.acc[0]+s.acc[1]*s.acc[1]+s.acc[2]*s.acc[2]);
  if(magnitude>0.3f && std::isfinite(magnitude))
    climbObserveMaxTilt=std::max(climbObserveMaxTilt,std::acos(clampf(s.acc[2]/magnitude,-1,1))*180/pi);
  bool current[7]; for(int i=0;i<4;++i) current[i]=s.gray[i];
  for(int i=0;i<3;++i) current[i+4]=s.e18[i];
  for(int i=0;i<7;++i) { if(current[i]!=climbObserveGround[i]) ++climbObserveSupportChanges; climbObserveGround[i]=current[i]; }
  if(uint32_t(now-climbObserveStarted)>=60000) finishClimbObservation(c,now);
}
/**
 * @brief   检查当前运动是否具备基本硬件与传感条件。
 * @details PWM 初始化和采集时间门控始终生效；开发手动可忽略缺失传感器，
 *          已在线且报告真实危险的输入仍按相应保护开关停车。自主及标定不放宽。
 * @note sampledMs 是采集调用时间，各缓存通道并非同一时刻完成采样。
 */
const char* sensorFault(const Sensors& s,const Config& c,uint32_t now,bool allowDevManual=false) {
  const bool relaxed=allowDevManual && c.developmentMode;
  const bool imuFresh=s.imuOk && !timedOut(now,s.imuMs,80);
  const bool digitalFresh=s.ioOk && !timedOut(now,s.digitalMs,80);
  if(!hardwareOutputsHealthy()) return "motor_output_unavailable";
  if(s.imuOk && !std::isfinite(s.gyroDps)) return "imu_or_motor_unavailable";
  if(!imuFresh && !relaxed) return s.imuOk?"imu_sample_stale":"imu_or_motor_unavailable";
  if(timedOut(now,s.sampledMs,250)) return "sensor_timeout";
  if(c.arenaEnabled) {
    const bool floorUnavailable=!digitalFresh;
    if(floorUnavailable && !relaxed) return "floor_sensor_unavailable";
    // 自主边缘处理由模型选择有限撤离方向；人工驾驶则立即撤去输出。
    if(mode!=Mode::Autonomous && !floorUnavailable) for(int i=0;i<3;++i)
      if(!s.e18[i] || !s.groundRaw[i]) return "floor_support_lost";
  }
  if(c.safetyEnabled) {
    if(c.edgeProtection) {
      if(!digitalFresh) { if(!relaxed) return "safety_io_unavailable"; }
      else if(!c.arenaEnabled && sensorEdge(s)) return "edge_detected";
    }
    if(c.tiltProtection) {
      if(!s.accelOk || (s.imuOk && !imuFresh) || !std::isfinite(s.acc[0]) || !std::isfinite(s.acc[1]) || !std::isfinite(s.acc[2])) {
        if(!relaxed) return "tilt_sensor_unavailable";
      } else if(sensorTilt(s)) return "robot_tilted";
    }
  }
  return nullptr;
}
bool sensorsPermit(const Sensors& s,const Config& c,uint32_t now,bool allowDevManual=false) {
  const char* reason=sensorFault(s,c,now,allowDevManual);
  if(reason) { fault(reason); return false; }
  return true;
}

// ============================================================================
// 命令消费：状态机准入与动作初始化
// ============================================================================
/**
 * @brief   由控制任务消费已通过投递门控的命令。
 * @details 先处理解锁和舵机，再按当前模式检查动作准入；Drive 允许当前占用者
 *          更新目标，其余运动需从空闲开始。新动作保存计数、里程和偏航起点。
 * @note 解锁要求当前为空闲且相关传感条件恢复；解锁本身不启动电机。
 */
void accept(const Command& cmd,const Config& c,uint32_t now) {
  if(cmd.expires && static_cast<int32_t>(now-cmd.expiresAt)>=0) {
    if(cmd.client==owner.load()) stopMotion("command_expired");
    return;
  }
  if(cmd.type==CommandType::Unlock) {
    if(mode==Mode::Idle && !sensorFault(t.sensors,c,now,c.developmentMode)) {
      locked.store(false); t.estop=false; text(t.fault,sizeof(t.fault),""); text(t.state,sizeof(t.state),"idle");
    }
    return;
  }
  if(locked.load()) return;
  if(cmd.type==CommandType::ClimbObserveEnd) {
    if(mode==Mode::ClimbObserve && cmd.client==owner.load()) {
      // 手动结束也必须先检查新鲜度与心跳，不能绕过本周期观测校验。
      updateClimbObservation(c,now);
      if(mode==Mode::ClimbObserve) finishClimbObservation(c,now);
    } else if(mode==Mode::Idle) stopMotion("climb_observe_not_active");
    return;
  }
  if(cmd.type==CommandType::ClimbObserveStart) {
    if(mode!=Mode::Idle || !sensorsPermit(t.sensors,c,now) || !t.sensors.accelOk || !t.sensors.ioOk ||
       timedOut(now,t.sensors.imuMs,80) || timedOut(now,t.sensors.digitalMs,80)) {
      if(mode==Mode::Idle) stopMotion("climb_observe_sensor_unavailable");
      return;
    }
    stopMotion(); mode=Mode::ClimbObserve; busy.store(true); owner.store(cmd.client); t.owner=cmd.client;
    climbObserveActive.store(true); climbObserveStarted=now; climbObserveSupportChanges=0; climbObserveMaxTilt=0;
    std::memcpy(startPulses,t.pulses,sizeof(startPulses));
    for(int i=0;i<4;++i) climbObserveGround[i]=t.sensors.gray[i];
    for(int i=0;i<3;++i) climbObserveGround[i+4]=t.sensors.e18[i];
    t.resultReady=false; t.progress=0; text(t.fault,sizeof(t.fault),"");
    text(t.state,sizeof(t.state),"climb_observe"); return;
  }
  if(cmd.type==CommandType::Servo) { servoWrite(cmd.value,c); return; }
  if(mode!=Mode::Idle && !(mode==Mode::Drive && cmd.type==CommandType::Drive && cmd.client==t.owner)) return;
  if(cmd.type==CommandType::Pose) {
    bool still=poseQuiet && uint32_t(now-poseQuietStarted)>=120;
    for(float speed:t.wheelSpeed) still=still && std::fabs(speed)<=0.8f;
    const bool ok=still && arenaModel.setPoseAnchor(cmd.x,cmd.y,cmd.value,
      cmd.upper?arena::Layer::UpperAnchored:arena::Layer::Lower,now);
    owner.store(0); busy.store(false); continuous.store(false); t.arena=arenaModel.snapshot();
    text(t.fault,sizeof(t.fault),ok?"":"pose_requires_stationary_valid_location"); return;
  }
  if(cmd.type==CommandType::ImuCal) {
    if(!t.sensors.imuOk) { text(t.fault,sizeof(t.fault),"imu_unavailable"); busy.store(false); owner.store(0); return; }
    stopMotion(); mode=Mode::ImuCal; busy.store(true); owner.store(cmd.client); t.owner=cmd.client;
    t.resultReady=false; t.imuCalibrating=true; text(t.state,sizeof(t.state),"imu_calibrating");
    std::memcpy(startPulses,t.pulses,sizeof(startPulses)); actionStarted=now; calSum=calSumSq=0; calSamples=0; return;
  }
  if(!sensorsPermit(t.sensors,c,now,cmd.type==CommandType::Drive && c.developmentMode)) return;
  if(cmd.type==CommandType::Navigate || cmd.type==CommandType::Battle || cmd.type==CommandType::Climb) {
    const bool ok=cmd.type==CommandType::Navigate?arenaModel.startNavigate(cmd.x,cmd.y,now):
      cmd.type==CommandType::Battle?arenaModel.startBattle(now):arenaModel.startClimb(now);
    if(!ok) { stopMotion(arena::reasonName(arenaModel.snapshot().reason)); return; }
    resetLoops(); mode=Mode::Autonomous; owner.store(AutonomousOwner); busy.store(true); continuous.store(false);
    t.arena=arenaModel.snapshot();
    t.owner=AutonomousOwner; actionSpeed=c.maxSpeed; t.resultReady=false; t.progress=0;
    text(t.fault,sizeof(t.fault),""); text(t.state,sizeof(t.state),"autonomous"); return;
  }
  if(mode==Mode::Drive && cmd.type==CommandType::Drive) {
    driveX=clampf(cmd.x,-1,1); driveY=clampf(cmd.y,-1,1); actionSpeed=clampf(cmd.speed,0,c.maxSpeed); return;
  }
  // 新动作使用独立起点和清空后的回路状态，心跳期限从接收时间开始计算。
  resetLoops(); owner.store(cmd.client); t.owner=cmd.client; busy.store(true);
  heartbeat.store(cmd.receivedMs); actionStarted=now; t.resultReady=false; t.progress=0;
  text(t.fault,sizeof(t.fault),"");
  std::memcpy(startPulses,t.pulses,sizeof(startPulses)); startYaw=t.yaw; startOdo=t.odo;
  startTurnFactor=directionalTurnFactor(cmd.value,c);
  actionSpeed=clampf(cmd.speed,1,c.maxSpeed);
  switch(cmd.type) {
    case CommandType::Drive: mode=Mode::Drive; continuous.store(true); driveX=clampf(cmd.x,-1,1); driveY=clampf(cmd.y,-1,1); actionSpeed=clampf(cmd.speed,0,c.maxSpeed); text(t.state,sizeof(t.state),"drive"); return;
    case CommandType::Move: mode=Mode::Move; target=cmd.value; text(t.state,sizeof(t.state),"move"); break;
    case CommandType::Turn: mode=Mode::Turn; target=cmd.value; text(t.state,sizeof(t.state),"turn"); break;
    case CommandType::OdoCalStart: mode=Mode::OdoCal; target=cmd.value==0?100:cmd.value; text(t.state,sizeof(t.state),"odo_cal"); break;
    case CommandType::TurnCalStart: mode=Mode::TurnCal; target=cmd.value==0?360:cmd.value; startTurnFactor=directionalTurnFactor(target,c); directionalTurnCal=cmd.directionalCalibration; text(t.state,sizeof(t.state),"turn_cal"); break;
    default: stopMotion(); return;
  }
  continuous.store(false);
  actionWheelTarget=(mode==Mode::Turn || mode==Mode::TurnCal)?wheelTurnDistance(target,c):target;
  // 根据行程/请求速度留出保守时间余量，并限制到 5～600 s；这不是完成时间预测。
  actionTimeout=uint32_t(clampf(5000+std::fabs(actionWheelTarget)/std::max(actionSpeed,5.0f)*5000,5000,600000));
}

// ============================================================================
// 静止零偏标定：采样、稳定性检查与延迟保存
// ============================================================================
/**
 * @brief   在零电机输出下采集陀螺零偏，满足时长和稳定性条件后申请保存。
 * @details 至少采样 3 s 且收集 250 个样本；轮计数不能改变，估计轮速需小于
 *          0.5 cm/s，角速度相对原零偏的差需小于 8 °/s。完成时检查方差不大于
 *          0.25 (°/s)²，以排除明显振动。保存由主控制循环在空闲阶段执行。
 * @note 外部缓慢匀速旋转可能具有很小方差；无绝对参考时无法与零偏区分。
 *       标定前需人为确认机体静止，温漂、地面晃动和振动仍会影响结果。
 */
void updateCalibration(const Config& c,uint32_t now) {
  motorStop();
  if(!hardwareOutputsHealthy() || !t.sensors.imuOk || timedOut(now,t.sensors.imuMs,80) || !std::isfinite(t.sensors.gyroDps)) {
    stopMotion("imu_cal_sample_unavailable"); return;
  }
  bool still=t.sensors.imuOk && std::fabs(t.sensors.gyroDps-c.gyroBias)<8;
  for(int i=0;i<4;++i) still=still && t.pulses[i]==startPulses[i] && std::fabs(t.wheelSpeed[i])<0.5f;
  if(!still) { stopMotion("imu_cal_requires_stationary"); return; }
  calSum+=t.sensors.gyroDps; calSumSq+=double(t.sensors.gyroDps)*t.sensors.gyroDps; ++calSamples;
  t.progress=clampf(float(now-actionStarted)/3000,0,1);
  if(uint32_t(now-actionStarted)<3000 || calSamples<250) return;
  const double mean=calSum/calSamples, variance=std::max(0.0,calSumSq/calSamples-mean*mean);
  if(variance>0.25) { stopMotion("imu_cal_vibration"); return; }
  newBias=float(mean); pendingBiasSave=true; adaptiveBias=0;
  stopMotion(); t.resultReady=true; ++t.resultId; text(t.resultType,sizeof(t.resultType),"imu_cal");
  t.resultConfigRevision=c.revision; t.calibrationSessionId=hardwareBootId();
  t.target=0; t.actual=newBias; t.error=0; t.progress=1; text(t.state,sizeof(t.state),"imu_cal_saving");
}

// ============================================================================
// 运动闭环：安全检查 → 目标规划 → 到位判断 → 速度与 PWM
// ============================================================================
/**
 * @brief   按当前动作模式执行一次控制周期。
 * @param c 本周期配置快照，计算期间保持同一组参数。
 * @param dt 本周期控制计算时间，单位 s。
 * @param now 当前 millis() 时间。
 * @details 位置或航向回路生成轮速需求；速度回路再修正 PWM。加减速限制作用
 *          于轮速目标，障碍、失联和故障停止直接撤去驱动，不等待目标斜坡。
 */
void updateMotion(const Config& c,float dt,uint32_t now) {
  // 1. 先检查运动条件、占用者心跳与动作期限，再计算任何输出需求。
  if(!sensorsPermit(t.sensors,c,now,mode==Mode::Drive && c.developmentMode)) return;
  if(mode!=Mode::Autonomous && timedOut(now,heartbeat.load(),c.heartbeatMs)) { stopMotion("heartbeat_lost"); generation.fetch_add(1); return; }
  if(mode==Mode::Drive && c.developmentMode && uint32_t(now-actionStarted)>=DevelopmentDriveLimitMs) {
    developmentDriveExpired.store(true);
    stopMotion("development_drive_timeout"); generation.fetch_add(1); return;
  }
  if(precision() && timedOut(now,actionStarted,actionTimeout)) { fault("action_timeout"); return; }
  // 2. 按动作模式生成四轮速度需求，统一使用 cm/s。
  //    电池电压有效且低于门限时，限速降至配置最大速度的一半。
  float desired[4]={0};
  float limit=c.maxSpeed*(t.lowBattery?0.5f:1);
  const float requested=std::min(limit,actionSpeed);
  float translation=0, rotation=0;
  bool arrived=false;
  if(mode==Mode::Autonomous) {
    const auto a=t.arena;
    if(a.mode==arena::Mode::Escape && arenaDriveMode!=arena::Mode::Escape) {
      // 边缘危险先撤旧驱动；下一拍才提交撤离方向，仍受驱动层换向等待约束。
      arenaDriveMode=a.mode; motorStop(); resetLoops(); return;
    }
    arenaDriveMode=a.mode;
    if(a.mode==arena::Mode::Halted || a.mode==arena::Mode::Idle) {
      stopMotion(arena::reasonName(a.reason)); generation.fetch_add(1); return;
    }
    translation=clampf(a.forwardCmS,-requested,requested);
    yawDemand=rampVelocity(yawDemand,clampf(a.yawDegS,-c.maxYawRate,c.maxYawRate),c.turnAcceleration,c.turnDeceleration,dt);
    rotation=wheelTurnDistance(yawDemand,c);
    desired[0]=desired[2]=translation-rotation; desired[1]=desired[3]=translation+rotation;
  } else if(mode==Mode::Drive) {
    // x 正值表示摇杆向右，y 正值表示前进；估计偏航以逆时针为正。
    // 因而右转需求取负旋转量；左右轮混合后同比缩放，保留操纵比例。
    translation=driveY*requested;
    const float cmPerDegree=std::max(0.001f,std::fabs(wheelTurnDistance(1,c)));
    const float rate=-driveX*std::min(c.maxYawRate,requested/cmPerDegree);
    yawDemand=rampVelocity(yawDemand,rate,c.turnAcceleration,c.turnDeceleration,dt);
    rotation=wheelTurnDistance(yawDemand,c);
    const float left=translation-rotation, right=translation+rotation;
    const float peak=std::max(std::fabs(left),std::fabs(right));
    const float scale=peak>requested && peak>0?requested/peak:1;
    desired[0]=desired[2]=left*scale; desired[1]=desired[3]=right*scale;
  } else if(mode==Mode::Move || mode==Mode::OdoCal) {
    // 平移：四轮均值里程控制距离，航向 PID 抑制相对起始方向的偏差。
    // 停车包络 sqrt(2 × 减速度 × 剩余距离) 限制位置 PID 的输出幅值。
    const float distance=t.odo-startOdo, remaining=target-distance;
    const float planned=stoppingVelocity(remaining,c.deceleration,requested);
    const float straight=positionLoop[0].step(remaining,dt,c.positionPid,-std::fabs(planned),std::fabs(planned));
    const float yawCorrection=headingLoop.step(startYaw-t.yaw,dt,c.headingPid,-c.maxYawRate,c.maxYawRate);
    yawDemand=rampVelocity(yawDemand,yawCorrection,c.turnAcceleration,c.turnDeceleration,dt);
    const float correction=wheelTurnDistance(yawDemand,c);
    desired[0]=desired[2]=straight-correction; desired[1]=desired[3]=straight+correction;
    translation=straight; rotation=correction;
    arrived=std::fabs(remaining)<=0.5f;
    t.progress=target==0?1:clampf(std::fabs(distance/target),0,1);
  } else if(mode==Mode::TurnCal) {
    // 转向标定逐轮追踪预定行程，刻意不加入偏航纠偏。
    // 外部实测转角才能反映有效轮距/侧滑误差，供 turnFactor 更新使用。
    float greatestError=0;
    for(int i=0;i<4;++i) {
      const float side=(i%2)?1:-1;
      const float distance=float(t.pulses[i]-startPulses[i])/pulsesPerCm(c);
      const float remaining=side*actionWheelTarget-distance;
      greatestError=std::max(greatestError,std::fabs(remaining));
      const float planned=stoppingVelocity(remaining,c.deceleration,requested);
      desired[i]=positionLoop[i].step(remaining,dt,c.positionPid,-std::fabs(planned),std::fabs(planned));
    }
    // 几何标定也受角加减速限制，但不使用偏航反馈修正其轮行程目标。
    float wheelPeak=0; for(float v:desired) wheelPeak=std::max(wheelPeak,std::fabs(v));
    const float cmPerDegree=std::max(0.001f,std::fabs(wheelTurnDistance(1,c)));
    yawDemand=rampVelocity(yawDemand,std::min(c.maxYawRate,wheelPeak/cmPerDegree),c.turnAcceleration,c.turnDeceleration,dt);
    const float wheelCap=std::fabs(wheelTurnDistance(yawDemand,c));
    if(wheelPeak>0) for(float& v:desired) v*=std::min(1.0f,wheelCap/wheelPeak);
    arrived=greatestError<=0.5f; rotation=actionWheelTarget;
    const float distance=absolutePulseTravel(t.pulses,startPulses)/pulsesPerCm(c);
    t.progress=actionWheelTarget==0?1:clampf(distance/std::fabs(actionWheelTarget),0,1);
  } else if(mode==Mode::Turn) {
    // 普通转向以累计偏航闭环为主；几何比例把角速度限制换算为轮速限制。
    const float remaining=target-(t.yaw-startYaw);
    const float degreesPerCm=1/std::max(0.001f,std::fabs(wheelTurnDistance(1,c)));
    const float maxRate=std::min(c.maxYawRate,requested*degreesPerCm);
    const float planned=stoppingVelocity(remaining,c.turnDeceleration,maxRate);
    const float rate=headingLoop.step(remaining,dt,c.headingPid,-std::fabs(planned),std::fabs(planned));
    yawDemand=rampVelocity(yawDemand,rate,c.turnAcceleration,c.turnDeceleration,dt);
    const float yawWheel=wheelTurnDistance(yawDemand,c);
    // 普通转向以累计偏航为唯一角度终点，轮速回路追踪对称目标。
    // 轮行程位置闭环保留给几何标定，避免滑移后的轮位置误差与航向纠偏争夺输出。
    desired[0]=desired[2]=-yawWheel; desired[1]=desired[3]=yawWheel;
    arrived=std::fabs(remaining)<=1.5f; rotation=remaining;
    t.progress=target==0?1:clampf(std::fabs((t.yaw-startYaw)/target),0,1);
  }
  // 3. 用当前运动方向筛选障碍；停止后使已经排队的旧代次命令失效。
  t.irSpeedScale=1; t.irLimited=false; t.irUnavailable=false;
  if(c.safetyEnabled && c.irProtection && (mode!=Mode::Autonomous || t.arena.mode==arena::Mode::Navigate)) {
    Config irConfig=c;
    if(mode==Mode::Drive && c.developmentMode) irConfig.irFailSafeStop=false;
    if(mode==Mode::Autonomous) irConfig.irFailSafeStop=false; // 远场超量程不等于必需地面反馈失效。
    DirectionSafety safety=directionSafety(t.sensors,irConfig,translation,rotation);
    // 换向或减速期间，新指令方向不等于当前运动方向。
    // 同时检查旧目标斜坡与测得轮速；近障碍不得因操作者反打摇杆而被漏掉。
    // 单相 FG 的测量方向仍由已施加的驱动方向推定，不能识别外力推动。
    const float* residuals[2]={demand,t.wheelSpeed};
    for(const float* speeds:residuals) {
      const float left=(speeds[0]+speeds[2])*0.5f;
      const float right=(speeds[1]+speeds[3])*0.5f;
      const DirectionSafety residual=directionSafety(t.sensors,irConfig,(left+right)*0.5f,(right-left)*0.5f);
      safety.speedScale=std::min(safety.speedScale,residual.speedScale);
      safety.unavailable=safety.unavailable || residual.unavailable;
    }
    t.irSpeedScale=safety.speedScale; t.irLimited=safety.speedScale<1; t.irUnavailable=safety.unavailable;
  }
  if(t.irSpeedScale<=0) {
    // 在本控制任务调用中撤去输出并终止动作；新的显式命令仍可重新申请运动。
    stopMotion(t.irUnavailable && c.irFailSafeStop?"ir_measurement_unavailable":"direction_obstructed"); generation.fetch_add(1); return;
  }
  // 4. 进入容差后将目标轮速归零，至少保持 150 ms 且估计轮速低于 2 cm/s。
  //    距离容差 0.5 cm，普通转角容差 1.5°；两者均是软件估计判据。
  if(arrived) {
    for(float& v:desired) v=0;
    if(!settleSince) settleSince=now;
    float fastest=0; for(float v:t.wheelSpeed) fastest=std::max(fastest,std::fabs(v));
    if(uint32_t(now-settleSince)>=150 && fastest<2) { result(c); return; }
  } else settleSince=0;
  // 5. 同比限速后应用每轮加减速斜坡；速度 PID 在比例 PWM 基础上纠偏。
  //    换向先降至零速，实际方向切换还受 motorWrite 的余转等待条件约束。
  float peak=0; for(float v:desired) peak=std::max(peak,std::fabs(v));
  const float scale=(peak>limit?limit/peak:1)*t.irSpeedScale;
  // 红外约束以此次请求为基准；斜坡后的强制上界避免旧的高速需求穿越新限速。
  const float safeLimit=std::min(limit,requested)*t.irSpeedScale;
  float requests[4]={0};
  for(int i=0;i<4;++i) {
    if(pendingStop.load() || locked.load()) { motorStop(); return; }
    demand[i]=rampVelocity(demand[i],desired[i]*scale,c.acceleration,c.deceleration,dt);
    demand[i]=clampf(demand[i],-safeLimit,safeLimit);
    if(mode==Mode::Drive && c.developmentMode) {
      // 开发手动使用前馈开环，避免未接 FG 的零读数被 PID 当作持续速度误差。
      // 该比例仅为低占空比输出请求，不承诺实际车速；死区、trim 后再限制最终 PWM。
      speedLoop[i].reset();
      const float trim=i%2?c.rightTrim:c.leftTrim;
      const float magnitude=std::fabs(demand[i])<0.05f?0:
        std::max(c.pwmDeadzone,1023*std::fabs(demand[i])/c.maxSpeed)*trim;
      requests[i]=(demand[i]<0?-1:1)*std::min(magnitude,DevelopmentPwmLimit);
    } else requests[i]=wheelSpeedPwm(speedLoop[i],demand[i],t.wheelSpeed[i],dt,c,i%2?c.rightTrim:c.leftTrim);
    motorWrite(i,requests[i],c);
  }
  motorOutputs(t.pwm);
  // 换向余转等待期间驱动层可以拒绝非零请求；此时不积累等待造成的轮速误差。
  for(int i=0;i<4;++i) if(std::fabs(requests[i])>=0.5f && t.pwm[i]==0) speedLoop[i].reset();
  // 6. 较高 PWM 持续输出却无新脉冲时，按配置持续时间判为堵转并锁定。
  //    该判据也可能由 FG 接线故障触发，不能单独证明机械堵转。
  for(int i=0;i<4;++i) {
    if(t.pwm[i]>std::max(300.0f,c.pwmDeadzone+80)) {
      if(!stallSince[i]) stallSince[i]=now;
      if(t.pulses[i]!=lastPulses[i]) stallSince[i]=now;
      if(timedOut(now,stallSince[i],c.stallTimeoutMs)) { fault("motor_stalled"); return; }
    } else stallSince[i]=0;
  }
}

// ============================================================================
// 唯一运动任务：采样、融合、消费命令与发布快照
// ============================================================================
/**
 * @brief   以 10 ms 为目标周期维护底盘控制。
 * @details 同步 I²C、配置互斥锁、NVS 保存和任务调度可能造成周期超时。
 *          dt 由实际时间差计算，再限制到 1～100 ms 保护数值计算；限幅不等于
 *          恢复了丢失的采样，也不构成严格实时保证。
 */
void run(void*) {
  // 唯一任务持有时基；静态存储也让确定性测试逐周期恢复任务时保留真实 dt。
  static TickType_t wake=xTaskGetTickCount(); static uint32_t previous=millis();
  for(;;) {
    const uint32_t now=millis(); const uint32_t elapsedMs=uint32_t(now-previous);
    const float dt=clampf(float(elapsedMs)*0.001f,0.001f,0.1f); previous=now;
    // 1. 获取配置快照并处理待停止标志；电机与传感器均在本任务中采样。
    const Config c=configSnapshot();
    configureArena(c);
    const unsigned stopped=pendingStop.exchange(0);
    if(stopped) {
      stopMotion(stopped&2?"emergency_stop":nullptr,(stopped&2)!=0);
      if(stopped&4) developmentDriveExpired.store(false);
    }
    motorSample(dt,c,t.pulses,t.wheelSpeed,t.rpm);
    t.sensors=sensorsRead(c);
    // 2. 用脉冲差更新带符号里程，再计算左右轮差对应的偏航增量。
    //    单相 FG 方向来自已施加的电机方向，外力推动等情形会造成估计误差。
    float delta[4]; for(int i=0;i<4;++i) delta[i]=float(t.pulses[i]-lastPulses[i])/pulsesPerCm(c);
    t.odo+=wheelMean(delta); t.speed=wheelMean(t.wheelSpeed);
    const float wheelYaw=odoYawDegrees((delta[0]+delta[2])/2,(delta[1]+delta[3])/2,c);
    if(c.gyroBias!=configuredBias) { configuredBias=c.gyroBias; adaptiveBias=0; }
    // 3. 空闲且轮计数、PWM 均静止，且加速度模长接近 1 g 时，才允许微调零偏。
    bool quiet=true; for(float v:delta) quiet=quiet && v==0;
    uint16_t actualPwm[4]; motorOutputs(actualPwm);
    bool zeroOutput=true; for(uint16_t v:actualPwm) zeroOutput=zeroOutput && v==0;
    const bool anchorStill=quiet && zeroOutput && t.sensors.imuOk && std::fabs(t.sensors.gyroDps-c.gyroBias)<2 && std::fabs(t.speed)<=0.8f;
    if(!anchorStill) poseQuiet=false;
    else if(!poseQuiet) { poseQuiet=true; poseQuietStarted=now; }
    const float accMagnitude=std::sqrt(t.sensors.acc[0]*t.sensors.acc[0]+t.sensors.acc[1]*t.sensors.acc[1]+t.sensors.acc[2]*t.sensors.acc[2]);
    const bool stationary=quiet && mode==Mode::Idle && zeroOutput && t.sensors.imuOk && t.sensors.accelOk && std::fabs(accMagnitude-1)<0.08f;
    if(!stationary) { quietSince=now; quietGyroCount=quietGyroIndex=0; }
    else {
      quietGyro[quietGyroIndex++]=t.sensors.gyroDps;
      quietGyroIndex%=100; quietGyroCount=std::min(size_t(100),quietGyroCount+1);
      if(quietGyroCount==100 && uint32_t(now-quietSince)>1000) {
        double sum=0,sumSq=0; for(float v:quietGyro) { sum+=v; sumSq+=double(v)*v; }
        const float mean=float(sum/100), variance=float(std::max(0.0,sumSq/100-double(mean)*mean));
        // 无外部航向参考时，缓慢匀速旋转与陀螺零偏不可区分。
        // 因此仅在小方差窗口内微调，并将补偿限定在持久化零偏附近 ±0.1 °/s。
        if(variance<0.01f && std::fabs(mean-c.gyroBias)<0.1f)
          adaptiveBias=clampf(adaptiveBias+0.002f*(mean-c.gyroBias-adaptiveBias),-0.1f,0.1f);
      }
    }
    // 空闲且无轮脉冲时冻结偏航积分，减少静置漂移；
    // 此约定也意味着无轮活动的外部转动不会被完整追踪。
    const float previousYaw=t.yaw;
    if(moving() || !quiet) t.yaw+=t.sensors.imuOk?fusedYawIncrement(t.sensors.gyroDps-adaptiveBias,dt,wheelYaw,c):wheelYaw;
    arenaFrame.nowMs=now; arenaFrame.dtS=elapsedMs?float(elapsedMs)*0.001f:0.001f;
    arenaFrame.deltaCm=wheelMean(delta); arenaFrame.deltaYawDeg=t.yaw-previousYaw;
    arenaFrame.imuOk=t.sensors.imuOk; arenaFrame.imuFresh=!timedOut(now,t.sensors.imuMs,80);
    arenaFrame.accelOk=t.sensors.accelOk; arenaFrame.accelSaturated=t.sensors.accelSaturated; arenaFrame.impact=t.sensors.impact;
    arenaFrame.tiltDeg=accMagnitude>0.3f?std::acos(clampf(t.sensors.acc[2]/accMagnitude,-1,1))*180/pi:0;
    arenaFrame.digitalOk=t.sensors.ioOk; arenaFrame.digitalFresh=!timedOut(now,t.sensors.digitalMs,80);
    for(int i=0;i<3;++i) { arenaFrame.groundPresent[i]=t.sensors.e18[i]; arenaFrame.rawGroundPresent[i]=t.sensors.groundRaw[i]; }
    for(int i=0;i<4;++i) arenaFrame.grayBright[i]=t.sensors.gray[i];
    arenaFrame.irCount=c.irCount;
    for(int i=0;i<12;++i) {
      arenaFrame.irCm[i]=t.sensors.ir[i]; arenaFrame.irSampledMs[i]=t.sensors.irMs[i];
      arenaFrame.irValid[i]=t.sensors.irValid[i] && !timedOut(now,t.sensors.irMs[i],250);
    }
    t.arena=arenaModel.tick(arenaFrame);
    t.lowBattery=t.sensors.batteryValid && t.sensors.batteryV<c.batteryLowV;
    // 4. 每周期最多消费 16 条命令；丢弃停止前的旧代次命令。
    Queued q;
    unsigned available=0;
    if(admission.compare_exchange_strong(available,1)) {
      for(int n=0;n<16 && xQueueReceive(queue,&q,0)==pdTRUE;++n) {
        if(q.generation==generation.load() && !pendingStop.load()) accept(q.command,c,now);
      }
      admission.store(0);
    }
    if(pendingStop.load()) motorStop();
    else if(mode==Mode::ImuCal) updateCalibration(c,now);
    else if(mode==Mode::ClimbObserve) updateClimbObservation(c,now);
    else if(moving()) updateMotion(c,dt,now);
    else { motorStop(); outputsStopped.store(true); }
    if(pendingBiasSave && mode==Mode::Idle && controllerTryBeginConfig()) {
      // 5. 本分支在空闲、零电机输出时保存零偏，期间门控普通动作。
      //    NVS 写入本身仍可阻塞本控制任务，不承诺固定保存耗时。
      Config saved=configSnapshot(); saved.gyroBias=newBias; String error;
      const bool ok=configSave(saved,error); pendingBiasSave=false; controllerEndConfig();
      if(ok) text(t.state,sizeof(t.state),"done");
      else { t.resultReady=false; text(t.fault,sizeof(t.fault),"imu_bias_save_failed"); }
    }
    // 6. 发布完整遥测副本；网络侧只复制 published，不读取正在更新的 t。
    std::memcpy(lastPulses,t.pulses,sizeof(lastPulses));
    motorOutputs(t.pwm); t.estop=locked.load(); t.owner=owner.load();
    portENTER_CRITICAL(&snapshotMux); published=t; portEXIT_CRITICAL(&snapshotMux);
    vTaskDelayUntil(&wake,pdMS_TO_TICKS(10));
  }
}
}

// ============================================================================
// 跨任务公共入口：初始化、排他投递与软件停止
// ============================================================================
void controllerBegin() {
  motorStop(); queue=xQueueCreate(16,sizeof(Queued));
  if(!queue) { locked.store(true); text(published.fault,sizeof(published.fault),"command_queue_failed"); return; }
  if(xTaskCreatePinnedToCore(run,"motion",24576,nullptr,3,nullptr,1)!=pdPASS) {
    locked.store(true); text(published.fault,sizeof(published.fault),"control_task_failed");
  }
}
bool controllerEnqueue(const Command& source) {
  // 停止绕过队列容量：先增加代次，旧命令即失效；再登记控制任务待处理标志。
  // 返回 true 只表示请求已登记，PWM 由控制任务响应后撤去。
  if(source.type==CommandType::Stop || source.type==CommandType::Estop || source.type==CommandType::Takeover) {
    if(source.type==CommandType::Estop) locked.store(true);
    // client=0 的内部断线停车只撤输出；只有客户端显式 Stop 才重新允许开发驾驶。
    const unsigned rearm=source.type==CommandType::Stop && source.client?4:0;
    generation.fetch_add(1); pendingStop.fetch_or((source.type==CommandType::Estop?3:1)|rearm);
    owner.store(0); busy.store(false); continuous.store(false); return true;
  }
  if(!queue) return false;
  if(admission.load()==2) return false;
  Command cmd=source; cmd.receivedMs=millis();
  if(cmd.expires && static_cast<int32_t>(cmd.receivedMs-cmd.expiresAt)>=0) return false;
  // 心跳不排队，且只刷新当前占用者的失联计时。
  if(cmd.type==CommandType::Heartbeat) {
    if(cmd.client && cmd.client==owner.load()) { heartbeat.store(cmd.receivedMs); return true; }
    return false;
  }
  if(!std::isfinite(cmd.x) || !std::isfinite(cmd.y) || !std::isfinite(cmd.value) || !std::isfinite(cmd.speed)) return false;
  if(cmd.type==CommandType::Drive && developmentDriveExpired.load()) return false;
  if(cmd.type==CommandType::Unlock) { const Queued q{cmd,generation.load()}; return xQueueSend(queue,&q,0)==pdTRUE; }
  if(locked.load()) return false;
  if(cmd.type==CommandType::Servo) {
    if(owner.load() && owner.load()!=cmd.client) return false;
    const Queued q{cmd,generation.load()}; return xQueueSend(queue,&q,0)==pdTRUE;
  }
  if(!cmd.client) return false;
  if(busy.load() && owner.load()==0) return false; // 空闲保存期间的门控，禁止新动作抢占。
  // 原子申请运动所有权；已有占用时只允许同一客户端更新持续驾驶。
  uint32_t expected=0;
  bool claimed=owner.compare_exchange_strong(expected,cmd.client);
  // 配置方先占窗口再检查 owner；投递方先申请 owner 再复查窗口。
  // 两条路径互相确认，关闭“检查空闲后，动作恰好入队”的跨任务窗口。
  if(claimed && admission.load()==2) {
    uint32_t mine=cmd.client; owner.compare_exchange_strong(mine,0); return false;
  }
  const bool driveRefresh=!claimed && expected==cmd.client && continuous.load() && cmd.type==CommandType::Drive;
  const bool observationFinish=!claimed && expected==cmd.client && climbObserveActive.load() && cmd.type==CommandType::ClimbObserveEnd;
  if(!claimed && !driveRefresh && !observationFinish) return false;
  if(claimed) { outputsStopped.store(false); busy.store(true); continuous.store(cmd.type==CommandType::Drive); heartbeat.store(cmd.receivedMs); }
  const Queued q{cmd,generation.load()};
  if(xQueueSend(queue,&q,0)==pdTRUE) {
    if(driveRefresh) heartbeat.store(cmd.receivedMs);
    return true;
  }
  // 入队失败时撤回本次新申请，避免无命令却长期保持占用。
  if(claimed) { owner.store(0); busy.store(false); continuous.store(false); }
  return false;
}
void controllerDisconnect(uint32_t client) {
  if(client && client==owner.load()) { Command c; c.type=CommandType::Stop; controllerEnqueue(c); }
}
void controllerEmergencyStop() { Command c; c.type=CommandType::Estop; controllerEnqueue(c); }
Telemetry controllerSnapshot() {
  portENTER_CRITICAL(&snapshotMux); Telemetry copy=published; portEXIT_CRITICAL(&snapshotMux);
  return copy;
}
bool controllerBusy() { return busy.load() || pendingStop.load() || admission.load()==2; }
bool controllerTryBeginConfig() {
  if(!queue) return false;
  unsigned available=0;
  if(!admission.compare_exchange_strong(available,2)) return false;
  if(owner.load() || busy.load() || pendingStop.load() || !outputsStopped.load()) {admission.store(0);return false;}
  // 空闲窗口开始后，先前排队的舵机/解锁请求不再使用写入前的配置执行。
  generation.fetch_add(1);
  return true;
}
void controllerEndConfig() {
  unsigned configuring=2;
  admission.compare_exchange_strong(configuring,0);
}
}
