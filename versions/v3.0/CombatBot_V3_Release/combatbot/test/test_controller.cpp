/**
 * @file test_controller.cpp
 * @brief 在确定性替身环境中执行真实 controller.cpp，验证控制与停止行为。
 *
 * ============================================================================
 * 测试范围
 *   注入虚拟时间、轮脉冲和传感器状态，使用队列替身运行生产控制实现。
 *   场景验证软件状态转换和结果，不验证真实并发、硬件电气安全或调度时延。
 * ============================================================================
 */
#include "Arduino.h"
#include "controller.h"
#include "config.h"
#include "hardware.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>
uint32_t hostMillis=100;
void (*hostTask)(void*)=nullptr;
static bot::Config cfg;
static bot::Sensors sensors;
static bool healthy=true;
static uint16_t output[4]={0};
static bool rejectMotorRequest=false; ///< 模拟真实 motorWrite 的换向等待。
static int64_t wheelPulses[4]={0};
static float wheelMeasured[4]={0};
static unsigned saved=0;
static bool advanceDuringSensorRead=false; ///< 注入采样耗时与期间的异步心跳。
// ============================================================================
// 硬件与配置替身：由场景直接注入测量值，电机写入只记录幅值
// ============================================================================
namespace bot {
Config configSnapshot() { return cfg; }
bool configSave(const Config& c,String&) { cfg=c; ++saved; return true; }
void motorStop() { for(auto& p:output) p=0; }
void motorOutputs(uint16_t p[4]) { std::memcpy(p,output,sizeof(output)); }
void motorWrite(int i,float pwm,const Config&) { output[i]=rejectMotorRequest?0:uint16_t(std::fabs(pwm)); }
void motorSample(float,const Config&,int64_t p[4],float speed[4],float rpm[4]) {
  std::memcpy(p,wheelPulses,sizeof(wheelPulses)); std::memcpy(speed,wheelMeasured,sizeof(wheelMeasured));
  for(int i=0;i<4;++i) rpm[i]=0;
}
void servoWrite(float,const Config&) {}
Sensors sensorsRead(const Config&) {
  if(advanceDuringSensorRead) {
    hostMillis+=5;
    Command hb; hb.type=CommandType::Heartbeat; hb.client=11;
    controllerEnqueue(hb);
  }
  sensors.sampledMs=hostMillis; return sensors;
}
bool hardwareHealthy() { return healthy; }
}
// 直接包含生产实现；测试没有另写一份控制状态机。
#include "../src/controller.cpp"
/**
 * @brief 推进虚拟 millis()，执行真实任务循环直到本次快照发布。
 * @note 调度替身在 vTaskDelayUntil 处抛出完成信号；不是线程调度器，
 *       不模拟真实并发、抢占、传感器等待或控制周期的最坏时延。
 */
static void tick(uint32_t elapsed=10) {
  hostMillis+=elapsed;
  try { hostTask(nullptr); } catch(const HostTickComplete&) {}
}
static bot::Command command(bot::CommandType type,uint32_t client=11,float value=0) {
  bot::Command c; c.type=type; c.client=client; c.y=1; c.speed=40; c.value=value; return c;
}
static bool motorsZero() { for(auto p:output) if(p) return false; return true; }
/** @brief 恢复测试输入并执行停止、解锁；不重新创建控制任务。 */
static void fresh() {
  cfg=bot::Config(); healthy=true; rejectMotorRequest=false; advanceDuringSensorRead=false; sensors=bot::Sensors(); sensors.imuOk=sensors.accelOk=sensors.ioOk=true; sensors.acc[2]=1;
  for(auto& p:wheelPulses) p=0;
  for(auto& v:wheelMeasured) v=0;
  assert(bot::controllerEnqueue(command(bot::CommandType::Stop))); tick();
  assert(bot::controllerEnqueue(command(bot::CommandType::Unlock))); tick();
  // 场景共用真实控制模块的启动与队列状态，通过 Stop/Unlock 恢复可运行条件。
}
int main() {
  sensors.imuOk=sensors.accelOk=sensors.ioOk=true; sensors.acc[2]=1;
  bot::controllerBegin(); tick();
  assert(!bot::controllerBusy() && motorsZero());
  assert(bot::controllerSnapshot().owner==0);
  // ============================================================================
  // 场景一：运动所有权与有效心跳
  // ============================================================================
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  assert(bot::controllerSnapshot().owner==11);
  assert(!bot::controllerEnqueue(command(bot::CommandType::Drive,22)));
  assert(!bot::controllerEnqueue(command(bot::CommandType::Heartbeat,22)));
  assert(bot::controllerEnqueue(command(bot::CommandType::Heartbeat))); tick(700);
  assert(bot::controllerBusy());
  assert(!bot::controllerEnqueue(command(bot::CommandType::Heartbeat,22))); tick(400);
  assert(!bot::controllerBusy() && motorsZero());
  assert(std::strcmp(bot::controllerSnapshot().fault,"heartbeat_lost")==0);
  // 同一占用者的驾驶更新也不替代独立心跳；持续发 Drive 仍应在心跳过期后停止。
  fresh(); assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  tick(600); assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick(500);
  assert(!bot::controllerBusy() && motorsZero());
  // ============================================================================
  // 场景二：普通停止代次、后续命令与断线停止
  // ============================================================================
  // 任意客户端可申请停止；停止前已排队的旧代次运动命令失效。
  fresh(); assert(bot::controllerEnqueue(command(bot::CommandType::Drive)));
  assert(bot::controllerEnqueue(command(bot::CommandType::Stop,22))); tick();
  assert(!bot::controllerBusy() && motorsZero());
  // 停止后提交的新动作携带新代次，允许重新申请运动；普通 Stop 不保持锁定。
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive)));
  assert(bot::controllerEnqueue(command(bot::CommandType::Stop,22)));
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive,22))); tick();
  assert(bot::controllerBusy() && bot::controllerSnapshot().owner==22);
  bot::controllerDisconnect(11); tick(); assert(bot::controllerBusy());
  bot::controllerDisconnect(22); tick(); assert(!bot::controllerBusy() && motorsZero());
  // ============================================================================
  // 场景三：急停锁定及恢复条件
  // ============================================================================
  // 急停锁定不能被普通 Stop 清除，必须显式解锁后才能再次运动。
  fresh(); assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  bot::controllerEmergencyStop();
  assert(!bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  assert(bot::controllerSnapshot().estop && motorsZero());
  assert(bot::controllerEnqueue(command(bot::CommandType::Stop))); tick();
  assert(!bot::controllerEnqueue(command(bot::CommandType::Drive)));
  assert(bot::controllerEnqueue(command(bot::CommandType::Unlock))); tick();
  assert(!bot::controllerSnapshot().estop);
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  // 硬件条件丢失或边缘危险会锁定运动；条件未恢复时 Unlock 也不能清除锁定。
  healthy=false; tick(); assert(bot::controllerSnapshot().estop && motorsZero());
  assert(bot::controllerEnqueue(command(bot::CommandType::Unlock))); tick(); assert(bot::controllerSnapshot().estop);
  fresh(); cfg.safetyEnabled=true; sensors.gray[0]=true;
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  assert(bot::controllerSnapshot().estop && motorsZero());
  // ============================================================================
  // 场景四：倒车障碍与无 FG 脉冲的堵转判据
  // ============================================================================
  // 倒车检查后向通道；本场景注入后方近障碍，验证输出撤去及故障文本。
  fresh(); cfg.safetyEnabled=true; cfg.irFailSafeStop=false; sensors.irValid[3]=true; sensors.ir[3]=15;
  auto reverse=command(bot::CommandType::Drive); reverse.y=-1;
  assert(bot::controllerEnqueue(reverse)); tick(); assert(!bot::controllerBusy() && motorsZero());
  assert(std::strcmp(bot::controllerSnapshot().fault,"direction_obstructed")==0);
  // 心跳持续有效时，高 PWM 长时间无新 FG 脉冲仍应触发堵转锁定。
  fresh(); assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  for(int i=0;i<2000 && !bot::controllerSnapshot().estop;++i) {
    assert(bot::controllerEnqueue(command(bot::CommandType::Heartbeat))); tick();
  }
  assert(bot::controllerSnapshot().estop && motorsZero());
  assert(std::strcmp(bot::controllerSnapshot().fault,"motor_stalled")==0);
  // ============================================================================
  // 场景五：静止零偏标定、一次保存与运动拒绝
  // ============================================================================
  // 标定跨多个控制周期收集样本；完成后仅保存一次，整个场景输出应为零。
  // 主机保存替身立即返回，本场景不验证真实 Flash 写入耗时。
  fresh(); sensors.gyroDps=0.5f; saved=0;
  assert(bot::controllerEnqueue(command(bot::CommandType::ImuCal))); tick();
  for(int i=0;i<305;++i) { tick(); assert(motorsZero()); }
  assert(saved==1 && std::fabs(cfg.gyroBias-0.5f)<0.001f);
  assert(bot::controllerSnapshot().resultReady && std::strcmp(bot::controllerSnapshot().resultType,"imu_cal")==0);
  // 标定期间注入任一轮脉冲变化，应中止且不调用保存。
  fresh(); saved=0; assert(bot::controllerEnqueue(command(bot::CommandType::ImuCal))); tick();
  ++wheelPulses[0]; tick(); assert(!bot::controllerBusy() && saved==0);
  assert(std::strcmp(bot::controllerSnapshot().fault,"imu_cal_requires_stationary")==0);
  // ============================================================================
  // 场景六：普通转向保留偏航纠偏，转向标定使用轮行程终点
  // ============================================================================
  // 普通转向即使已达到理论轮行程，也应继续修正剩余偏航误差。
  fresh(); assert(bot::controllerEnqueue(command(bot::CommandType::Turn,11,360))); tick();
  const auto turnPulses=int64_t(std::round(bot::wheelTurnDistance(360,cfg)*bot::pulsesPerCm(cfg)));
  wheelPulses[0]-=turnPulses; wheelPulses[2]-=turnPulses;
  wheelPulses[1]+=turnPulses; wheelPulses[3]+=turnPulses; tick();
  // 注入脉冲突变会形成偏航跳变，航向微分项在一个样本内产生减速需求；
  // 再运行一个周期后，应恢复对剩余偏航误差的修正。
  tick();
  assert(bot::controllerBusy() && !motorsZero());
  assert(bot::controllerSnapshot().yaw-bot::startYaw<360-1.5f);
  // 转向标定按轮目标到位，不用融合偏航作完成门控，以保留外部实测标定依据。
  fresh(); cfg.turnFactor=1.3f;
  assert(bot::controllerEnqueue(command(bot::CommandType::TurnCalStart))); tick();
  const auto calibrationPulses=int64_t(std::round(bot::wheelTurnDistance(360,cfg)*bot::pulsesPerCm(cfg)));
  wheelPulses[0]-=calibrationPulses; wheelPulses[2]-=calibrationPulses;
  wheelPulses[1]+=calibrationPulses; wheelPulses[3]+=calibrationPulses;
  for(int i=0;i<20;++i) tick();
  auto report=bot::controllerSnapshot();
  assert(!bot::controllerBusy() && report.resultReady && motorsZero());
  assert(std::strcmp(report.resultType,"turn_cal")==0 && report.target==360);
  assert(std::fabs(report.calibrationPulses-float(calibrationPulses))<0.001f);
  assert(std::fabs(report.calibrationTurnFactor-1.3f)<0.001f);
  assert(std::fabs(report.actual)<360-1.5f); // 融合偏航尚未到目标，但轮行程已经满足标定完成条件。
  // ============================================================================
  // 场景七：默认 100 cm 里程标定及结果计数
  // ============================================================================
  // 默认动作目标为 100 cm；结果应保留估计位移和四轮绝对脉冲均值。
  fresh(); assert(bot::controllerEnqueue(command(bot::CommandType::OdoCalStart))); tick();
  const auto odometerPulses=int64_t(std::round(100*bot::pulsesPerCm(cfg)));
  for(auto& p:wheelPulses) p+=odometerPulses;
  for(int i=0;i<20;++i) tick();
  report=bot::controllerSnapshot();
  assert(!bot::controllerBusy() && report.resultReady && std::strcmp(report.resultType,"odo_cal")==0);
  assert(report.target==100 && std::fabs(report.actual-100)<0.5f);
  assert(std::fabs(report.calibrationPulses-float(odometerPulses))<0.001f);
  // ============================================================================
  // 场景八：独立保护开关与一致的解锁准入
  // ============================================================================
  fresh(); cfg.safetyEnabled=true; cfg.edgeProtection=false; cfg.irProtection=false; cfg.tiltProtection=false;
  sensors.ioOk=sensors.accelOk=false; sensors.gray[0]=true;
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  assert(bot::controllerBusy() && !bot::controllerSnapshot().estop);
  cfg.edgeProtection=true; tick();
  assert(bot::controllerSnapshot().estop && motorsZero());
  assert(std::strcmp(bot::controllerSnapshot().fault,"safety_io_unavailable")==0);
  assert(bot::controllerEnqueue(command(bot::CommandType::Unlock))); tick(); assert(bot::controllerSnapshot().estop);
  cfg.edgeProtection=false;
  assert(bot::controllerEnqueue(command(bot::CommandType::Unlock))); tick(); assert(!bot::controllerSnapshot().estop);
  cfg.tiltProtection=true;
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  assert(bot::controllerSnapshot().estop && motorsZero());
  assert(std::strcmp(bot::controllerSnapshot().fault,"tilt_sensor_unavailable")==0);
  // ============================================================================
  // 场景九：方向渐进限速、当前需求立即限幅与无效测距策略
  // ============================================================================
  fresh(); cfg.safetyEnabled=true; cfg.acceleration=500; cfg.deceleration=1;
  for(int i=0;i<6;++i) { sensors.irValid[i]=true; sensors.ir[i]=100; }
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  for(int i=0;i<10;++i) tick();
  assert(std::fabs(bot::demand[0]-40)<0.01f);
  sensors.ir[0]=42.5f; tick();
  report=bot::controllerSnapshot();
  assert(report.irLimited && !report.irUnavailable && std::fabs(report.irSpeedScale-0.5f)<0.001f);
  assert(std::fabs(bot::demand[0]-20)<0.01f); // 不能沿低减速度继续输出旧的40 cm/s需求。
  sensors.ir[0]=25; tick();
  assert(!bot::controllerBusy() && motorsZero());
  assert(std::strcmp(bot::controllerSnapshot().fault,"direction_obstructed")==0);
  // 正前方危险时仍允许明确倒车脱离；不因前向通道拒绝后向动作。
  reverse=command(bot::CommandType::Drive); reverse.y=-1;
  assert(bot::controllerEnqueue(reverse)); tick(); assert(bot::controllerBusy());
  fresh(); cfg.safetyEnabled=true; cfg.irFailSafeStop=true;
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  assert(!bot::controllerBusy() && motorsZero() && bot::controllerSnapshot().irUnavailable);
  assert(std::strcmp(bot::controllerSnapshot().fault,"ir_measurement_unavailable")==0);
  cfg.irFailSafeStop=false;
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  assert(bot::controllerBusy() && bot::controllerSnapshot().irUnavailable);
  // 换向时旧前向斜坡仍可能非零：不能只检查新倒车方向而忽略前方近障碍。
  fresh(); cfg.safetyEnabled=true; cfg.acceleration=500; cfg.deceleration=1;
  for(int i=0;i<6;++i) { sensors.irValid[i]=true; sensors.ir[i]=100; }
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  for(int i=0;i<10;++i) tick();
  assert(std::fabs(bot::demand[0]-40)<0.01f);
  sensors.ir[0]=25;
  reverse=command(bot::CommandType::Drive); reverse.y=-1;
  assert(bot::controllerEnqueue(reverse)); tick();
  assert(!bot::controllerBusy() && motorsZero());
  assert(std::strcmp(bot::controllerSnapshot().fault,"direction_obstructed")==0);
  // 停车之后新的倒车请求仍可离开前方障碍，不把后方运动永久封锁。
  assert(bot::controllerEnqueue(reverse)); tick(); assert(bot::controllerBusy());
  // ============================================================================
  // 场景十：角加减速、低压速度上限与可配置堵转时间
  // ============================================================================
  fresh(); cfg.turnAcceleration=10; cfg.turnDeceleration=20;
  assert(bot::controllerEnqueue(command(bot::CommandType::Turn,11,90))); tick();
  assert(std::fabs(bot::yawDemand-0.1f)<0.001f);
  fresh(); cfg.turnAcceleration=100; cfg.turnDeceleration=20;
  assert(bot::controllerEnqueue(command(bot::CommandType::Turn,11,90))); tick();
  assert(std::fabs(bot::yawDemand-1)<0.001f);
  // 驾驶中释放旋转键时按独立角减速度回到零。
  fresh(); cfg.turnAcceleration=100; cfg.turnDeceleration=20;
  auto spin=command(bot::CommandType::Drive); spin.x=1; spin.y=0;
  assert(bot::controllerEnqueue(spin)); tick();
  assert(std::fabs(bot::yawDemand+1)<0.001f);
  spin.x=0; assert(bot::controllerEnqueue(spin)); tick();
  assert(std::fabs(bot::yawDemand+0.8f)<0.001f);
  fresh(); cfg.acceleration=500; sensors.batteryValid=true; sensors.batteryV=20;
  auto fast=command(bot::CommandType::Drive); fast.speed=100;
  assert(bot::controllerEnqueue(fast)); tick();
  for(int i=0;i<20;++i) tick();
  assert(bot::controllerSnapshot().lowBattery);
  for(float value:bot::demand) assert(std::fabs(value)<=50.001f);
  fresh(); cfg.acceleration=500; cfg.stallTimeoutMs=300;
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  for(int i=0;i<20;++i) { assert(bot::controllerEnqueue(command(bot::CommandType::Heartbeat))); tick(); }
  assert(!bot::controllerSnapshot().estop);
  for(int i=0;i<20 && !bot::controllerSnapshot().estop;++i) { assert(bot::controllerEnqueue(command(bot::CommandType::Heartbeat))); tick(); }
  assert(bot::controllerSnapshot().estop && motorsZero());
  fresh(); cfg.acceleration=500; cfg.stallTimeoutMs=1000;
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  for(int i=0;i<40;++i) { assert(bot::controllerEnqueue(command(bot::CommandType::Heartbeat))); tick(); }
  assert(!bot::controllerSnapshot().estop);
  // ============================================================================
  // 场景十一：驱动层拒绝PWM时不积分，所有运动与标定均受心跳和急停保护
  // ============================================================================
  fresh(); rejectMotorRequest=true; cfg.acceleration=500; cfg.speedPid=bot::Gains{0,10,0};
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  for(int i=0;i<50;++i) { assert(bot::controllerEnqueue(command(bot::CommandType::Heartbeat))); tick(); assert(motorsZero()); }
  for(auto& loop:bot::speedLoop) assert(loop.step(0,0.01f,cfg.speedPid,-1023,1023)==0);
  rejectMotorRequest=false; tick(); assert(!motorsZero());
  const bot::CommandType guarded[]={bot::CommandType::Move,bot::CommandType::Turn,bot::CommandType::OdoCalStart,bot::CommandType::TurnCalStart};
  for(auto type:guarded) {
    fresh(); cfg.stallTimeoutMs=5000;
    assert(bot::controllerEnqueue(command(type,11,100))); tick(); tick(1001);
    assert(!bot::controllerBusy() && motorsZero());
    assert(std::strcmp(bot::controllerSnapshot().fault,"heartbeat_lost")==0);
    bot::controllerEmergencyStop(); tick();
    assert(!bot::controllerEnqueue(command(type,11,100)));
  }
  // 场景十二：配置窗口与运动准入共享；停止/急停仍能绕过门控。
  fresh();
  assert(bot::controllerTryBeginConfig()); assert(bot::controllerBusy());
  assert(!bot::controllerTryBeginConfig());
  assert(!bot::controllerEnqueue(command(bot::CommandType::Drive)));
  assert(!bot::controllerEnqueue(command(bot::CommandType::Servo)));
  assert(!bot::controllerEnqueue(command(bot::CommandType::Unlock)));
  assert(bot::controllerEnqueue(command(bot::CommandType::Stop))); tick();
  assert(!bot::controllerEnqueue(command(bot::CommandType::Drive)) && motorsZero());
  bot::controllerEndConfig();
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  assert(!bot::controllerTryBeginConfig());
  // 模拟任务已取走Stop标志但尚未执行motorStop的狭窄窗口：不能提前保存。
  assert(bot::controllerEnqueue(command(bot::CommandType::Stop)));
  const unsigned awaitingOutput=bot::pendingStop.exchange(0);
  assert(awaitingOutput && !bot::controllerTryBeginConfig());
  bot::pendingStop.fetch_or(awaitingOutput); tick(); assert(motorsZero());
  assert(bot::controllerTryBeginConfig()); bot::controllerEndConfig();
  fresh(); assert(bot::controllerEnqueue(command(bot::CommandType::Servo)));
  assert(bot::controllerTryBeginConfig()); tick(); assert(!bot::controllerSnapshot().owner);
  bot::controllerEndConfig();
  // 场景十三：采样耗时使新反馈/心跳比周期起点晚5ms，仍应保持运动。
  fresh(); advanceDuringSensorRead=true;
  assert(bot::controllerEnqueue(command(bot::CommandType::Drive))); tick();
  assert(bot::controllerBusy() && !motorsZero());
  tick(); assert(bot::controllerBusy() && !bot::controllerSnapshot().estop);
  assert(bot::controllerSnapshot().fault[0]=='\0');
  advanceDuringSensorRead=false;
  std::puts("controller safety tests passed");
}
