/**
 * @file test_controller_v4.cpp
 * @brief 在确定性 RTOS 与硬件替身中验证真实控制器的 V4 安全集成。
 * @note 验证代次、任务所有权与输出约束，不模拟真实并发或机械停车距离。
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
static uint16_t output[4]={0};
static int64_t wheelPulses[4]={0};
static float measured[4]={0};
static uint32_t revision=1;
static bool outputsHealthy=true,refreshSensors=true;
static bool refreshImu=true,refreshDigital=true;
namespace bot {
Config configSnapshot(){return cfg;}
bool configSave(const Config& c,String&){cfg=c;return true;}
void motorStop(){for(auto& p:output)p=0;}
void motorOutputs(uint16_t p[4]){std::memcpy(p,output,sizeof(output));}
void motorWrite(int i,float pwm,const Config&){output[i]=uint16_t(std::fabs(pwm));}
void motorSample(float,const Config&,int64_t p[4],float speed[4],float rpm[4]){
  std::memcpy(p,wheelPulses,sizeof(wheelPulses));std::memcpy(speed,measured,sizeof(measured));for(int i=0;i<4;++i)rpm[i]=0;
}
void servoWrite(float,const Config&){}
Sensors sensorsRead(const Config&){sensors.sampledMs=hostMillis;if(refreshSensors){if(refreshImu)sensors.imuMs=hostMillis;if(refreshDigital)sensors.digitalMs=hostMillis;for(auto& ms:sensors.irMs)ms=hostMillis;}return sensors;}
bool hardwareHealthy(){return true;}
bool hardwareOutputsHealthy(){return outputsHealthy;}
uint32_t hardwareBootId(){return 4321;}
}
#include "../src/controller.cpp"
static void tick(uint32_t elapsed=10){hostMillis+=elapsed;try{hostTask(nullptr);}catch(const HostTickComplete&){} }
static void advance(uint32_t elapsed){for(uint32_t i=0;i<elapsed;i+=10)tick();}
static bot::Command command(bot::CommandType type,uint32_t client=11){bot::Command c;c.type=type;c.client=client;c.y=1;c.speed=20;return c;}
static bool zero(){for(auto p:output)if(p)return false;return true;}
static void fresh(bool calibrated=true){
  outputsHealthy=refreshSensors=true;
  refreshImu=refreshDigital=true;
  cfg=bot::Config();cfg.revision=++revision;cfg.arenaEnabled=true;cfg.arenaCalibrated=calibrated;cfg.arenaTaskMs=3000;
  cfg.safetyEnabled=true;cfg.irProtection=false;cfg.stallTimeoutMs=5000;
  sensors=bot::Sensors();sensors.imuOk=sensors.accelOk=sensors.ioOk=true;sensors.acc[2]=1;
  for(int i=0;i<3;++i)sensors.e18[i]=sensors.groundRaw[i]=true;
  for(int i=0;i<12;++i){sensors.irValid[i]=true;sensors.ir[i]=140;}
  for(auto& v:measured)v=0;
  assert(bot::controllerEnqueue(command(bot::CommandType::Stop)));tick();
  assert(bot::controllerEnqueue(command(bot::CommandType::Unlock)));tick();tick(200);
}
static void anchor(){
  auto c=command(bot::CommandType::Pose);c.x=c.y=c.value=0;c.upper=true;
  assert(bot::controllerEnqueue(c));tick();assert(bot::controllerSnapshot().arena.pose.valid);
  assert(bot::controllerSnapshot().arena.pose.layer==bot::arena::Layer::UpperAnchored);
}
int main(){
  using bot::CommandType;
  bot::controllerBegin();fresh(false);
  // 未校准默认无法启动自治；网络层受理不等于控制任务实际启动。
  anchor();assert(bot::controllerEnqueue(command(CommandType::Battle)));tick();
  assert(!bot::controllerBusy()&&zero());assert(std::strcmp(bot::controllerSnapshot().fault,"not_armed")==0);
  // E18 true 是见地，不能沿用旧版“见物即边缘”的解释把车辆锁死。
  fresh();anchor();assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();
  assert(bot::controllerBusy()&&!bot::controllerSnapshot().estop);
  // 人工仍由心跳兜底，自动任务则保持车端所有权，网页退出不结束比赛。
  tick(1100);assert(!bot::controllerBusy()&&zero());assert(std::strcmp(bot::controllerSnapshot().fault,"heartbeat_lost")==0);
  fresh();anchor();assert(bot::controllerEnqueue(command(CommandType::Battle,bot::CloudOwner)));tick();
  assert(bot::controllerSnapshot().owner==bot::AutonomousOwner&&bot::controllerBusy());
  bot::controllerDisconnect(bot::CloudOwner);advance(1100);
  assert(bot::controllerBusy()&&bot::controllerSnapshot().owner==bot::AutonomousOwner);
  assert(std::strcmp(bot::controllerSnapshot().fault,"heartbeat_lost")!=0);
  advance(2000);assert(!bot::controllerBusy()&&zero()); // 自治有限总预算仍有效。
  // 接管旁路在满队列中先失效旧代次；后续手动动作使用新代次正常启动。
  fresh();for(int i=0;i<16;++i)assert(bot::controllerEnqueue(command(CommandType::Drive,11)));
  assert(!bot::controllerEnqueue(command(CommandType::Drive,11)));
  assert(bot::controllerEnqueue(command(CommandType::Takeover,22)));tick();assert(!bot::controllerBusy()&&zero());
  assert(bot::controllerEnqueue(command(CommandType::Drive,22)));tick();assert(bot::controllerSnapshot().owner==22);
  bot::controllerDisconnect(11);tick();assert(bot::controllerBusy());bot::controllerDisconnect(22);tick();assert(!bot::controllerBusy()&&zero());
  // 入队时未到期、消费时已到期的云命令不能重算期限并驱动车辆。
  fresh();auto delayed=command(CommandType::Drive,bot::CloudOwner);delayed.expires=true;delayed.expiresAt=hostMillis+5;
  assert(bot::controllerEnqueue(delayed));tick(10);assert(!bot::controllerBusy()&&zero());assert(std::strcmp(bot::controllerSnapshot().fault,"command_expired")==0);
  delayed.expiresAt=hostMillis;assert(!bot::controllerEnqueue(delayed));
  // 外力移动中不能用人工起点重置坐标；速度归零后才允许锚定。
  fresh();anchor();const auto original=bot::controllerSnapshot().arena.pose;
  for(auto& v:measured) { v=5; }
  auto pose=command(CommandType::Pose);pose.x=30;pose.y=20;pose.upper=true;
  assert(bot::controllerEnqueue(pose));tick();auto snapshot=bot::controllerSnapshot();
  assert(std::fabs(snapshot.arena.pose.xCm-original.xCm)<0.001f);
  assert(std::strcmp(snapshot.fault,"pose_requires_stationary_valid_location")==0);
  for(auto& v:measured) { v=0; }
  tick();tick(200);assert(bot::controllerEnqueue(pose));tick();assert(std::fabs(bot::controllerSnapshot().arena.pose.xCm-30)<0.001f);

  // 轮速为零并不充分：陀螺运动和新脉冲都会重置至少 120 ms 静止窗口。
  fresh();anchor();sensors.gyroDps=4;pose.x=50;assert(bot::controllerEnqueue(pose));tick();
  assert(std::fabs(bot::controllerSnapshot().arena.pose.xCm-50)>1);
  sensors.gyroDps=0;tick();tick(200);++wheelPulses[0];assert(bot::controllerEnqueue(pose));tick();
  assert(std::fabs(bot::controllerSnapshot().arena.pose.xCm-50)>1);
  tick();tick(200);assert(bot::controllerEnqueue(pose));tick();assert(std::fabs(bot::controllerSnapshot().arena.pose.xCm-50)<0.001f);

  // 自治前进中触发缺地，首拍先清旧方向需求，下一拍才允许安全撤离。
  fresh();anchor();auto navigation=command(CommandType::Navigate);navigation.x=50;navigation.y=0;
  assert(bot::controllerEnqueue(navigation));tick();advance(100);
  assert(bot::demand[0]>0&&!zero());sensors.e18[0]=sensors.groundRaw[0]=false;tick();
  assert(bot::controllerSnapshot().arena.mode==bot::arena::Mode::Escape&&zero());
  for(float v:bot::demand)assert(v==0);
  tick();for(float v:bot::demand)assert(v<=0);assert(!bot::controllerSnapshot().estop);
  // 自治中的缺地由模型有限撤离；人工缺地始终撤输出并锁定。
  fresh();anchor();assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();sensors.e18[0]=sensors.groundRaw[0]=false;tick();
  assert(zero()&&bot::controllerSnapshot().estop);assert(std::strcmp(bot::controllerSnapshot().fault,"floor_support_lost")==0);
  // 开发模式只放宽手动摇杆的可选传感器门槛，并限制最终 PWM；缺传感器不能启用自治或标定。
  fresh();cfg.developmentMode=true;cfg.arenaEnabled=false;cfg.safetyEnabled=true;cfg.irProtection=true;
  sensors.imuOk=sensors.accelOk=sensors.ioOk=false;
  for(int i=0;i<12;++i)sensors.irValid[i]=false;
  assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();
  assert(bot::controllerBusy()&&!zero());for(auto p:bot::controllerSnapshot().pwm)assert(p<=180);
  assert(bot::controllerSnapshot().irUnavailable);
  auto stop=command(CommandType::Estop);assert(bot::controllerEnqueue(stop));tick();
  assert(zero()&&bot::controllerSnapshot().estop);
  // 无 FG 的开发手动不经过 PID 累积；trim/死区也不得突破最终 PWM 限幅。
  fresh();cfg.developmentMode=true;cfg.arenaEnabled=false;cfg.safetyEnabled=false;
  cfg.pwmDeadzone=900;cfg.leftTrim=cfg.rightTrim=1.5f;sensors.imuOk=sensors.accelOk=sensors.ioOk=false;
  assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();
  for(int window=0;window<28;++window) {
    assert(bot::controllerEnqueue(command(CommandType::Drive)));for(int i=0;i<50;++i)tick();
    assert(bot::controllerBusy());for(auto p:bot::controllerSnapshot().pwm)assert(p<=180);
  }
  assert(bot::controllerEnqueue(command(CommandType::Drive)));tick(1000);
  assert(!bot::controllerBusy()&&zero());assert(std::strcmp(bot::controllerSnapshot().fault,"development_drive_timeout")==0);
  // 持续按住摇杆在超时后仍重发 Drive，不得自动开启另一轮15秒窗口。
  for(int i=0;i<100;++i) {
    assert(!bot::controllerEnqueue(command(CommandType::Drive)));tick(200);
    assert(!bot::controllerBusy()&&zero());
  }
  auto neutral=command(CommandType::Drive);neutral.x=neutral.y=neutral.speed=0;
  assert(!bot::controllerEnqueue(neutral));tick();assert(zero()); // 自动空闲归零不能解除锁存。
  assert(bot::controllerEnqueue(command(CommandType::Stop,0)));tick();
  assert(!bot::controllerEnqueue(command(CommandType::Drive)));tick();assert(zero()); // 内部断线停车不重新准入。
  assert(bot::controllerEnqueue(command(CommandType::Unlock)));tick();
  assert(!bot::controllerEnqueue(command(CommandType::Drive))); // 普通Unlock也不能解除开发超时。
  assert(bot::controllerEnqueue(command(CommandType::Stop)));tick();
  assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();
  assert(bot::controllerBusy()&&!zero());for(auto p:bot::controllerSnapshot().pwm)assert(p<=180);
  fresh();cfg.developmentMode=true;cfg.arenaEnabled=false;cfg.safetyEnabled=false;
  assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();outputsHealthy=false;tick();
  assert(zero()&&bot::controllerSnapshot().estop);
  assert(std::strcmp(bot::controllerSnapshot().fault,"motor_output_unavailable")==0);
  // 汇总采样时钟刷新不能掩盖各通道缓存陈旧；普通手动按80ms新鲜度门控。
  fresh();cfg.arenaEnabled=false;cfg.safetyEnabled=false;
  assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();refreshImu=false;tick(90);
  assert(zero()&&bot::controllerSnapshot().estop);
  assert(std::strcmp(bot::controllerSnapshot().fault,"imu_sample_stale")==0);
  fresh();cfg.arenaEnabled=false;cfg.safetyEnabled=true;cfg.irProtection=false;
  for(int i=0;i<3;++i)sensors.e18[i]=false;
  assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();refreshDigital=false;tick(90);
  assert(zero()&&bot::controllerSnapshot().estop);
  assert(std::strcmp(bot::controllerSnapshot().fault,"safety_io_unavailable")==0);
  // 开发手动把陈旧缓存视为未知，不执行缓存中的旧危险；新鲜危险仍停车。
  fresh();cfg.developmentMode=true;cfg.arenaEnabled=false;cfg.safetyEnabled=true;cfg.irProtection=false;
  for(int i=0;i<3;++i)sensors.e18[i]=false;
  refreshImu=refreshDigital=false;sensors.imuMs=sensors.digitalMs=hostMillis-100;
  sensors.gray[0]=true;sensors.acc[2]=-1;
  assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();
  assert(bot::controllerBusy()&&!zero());
  refreshImu=refreshDigital=true;tick();assert(zero()&&bot::controllerSnapshot().estop);
  fresh();cfg.developmentMode=true;cfg.arenaEnabled=false;cfg.safetyEnabled=false;
  refreshImu=false;sensors.imuMs=hostMillis-100;sensors.gyroDps=NAN;
  assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();
  assert(zero()&&bot::controllerSnapshot().estop);
  fresh();cfg.developmentMode=true;cfg.arenaEnabled=false;cfg.safetyEnabled=true;cfg.irProtection=true;
  sensors.imuOk=sensors.accelOk=sensors.ioOk=false;
  assert(bot::controllerEnqueue(command(CommandType::Battle)));tick();
  assert(!bot::controllerBusy()&&zero());
  fresh();cfg.developmentMode=true;cfg.arenaEnabled=false;cfg.safetyEnabled=true;cfg.irProtection=true;
  sensors.imuOk=sensors.accelOk=sensors.ioOk=false;
  assert(bot::controllerEnqueue(command(CommandType::OdoCalStart)));tick();
  assert(!bot::controllerBusy()&&zero());
  // 若可选传感器在线并报告真实危险，开发模式仍执行对应保护。
  fresh();cfg.developmentMode=true;cfg.arenaEnabled=false;cfg.safetyEnabled=true;cfg.irProtection=true;
  sensors.ioOk=true;sensors.e18[0]=true;
  assert(bot::controllerEnqueue(command(CommandType::Drive)));tick();
  assert(zero()&&bot::controllerSnapshot().estop);
  // 直线标定不能把三路 FG 的均值到位误认为四轮反馈完整。
  fresh();assert(bot::controllerEnqueue(command(CommandType::OdoCalStart)));tick();
  const auto threeWheelTravel=int64_t(std::round(100*bot::pulsesPerCm(cfg)*4/3));
  for(int i=0;i<3;++i)wheelPulses[i]+=threeWheelTravel;
  for(int i=0;i<20;++i)tick();
  assert(zero()&&bot::controllerSnapshot().estop&&!bot::controllerSnapshot().resultReady);
  assert(std::strcmp(bot::controllerSnapshot().fault,"calibration_fg_incomplete")==0);
  // 左右转向标定使用独立系数、独立结果类型；旧 turn 命令仍沿用通用结果名。
  fresh();cfg.turnFactorLeft=1.2f;cfg.turnFactorRight=1.4f;
  auto leftCal=command(CommandType::TurnCalStart);leftCal.value=360;leftCal.directionalCalibration=true;
  assert(bot::controllerEnqueue(leftCal));tick();
  auto pulseDelta=int64_t(std::round(bot::wheelTurnDistance(360,cfg)*bot::pulsesPerCm(cfg)));
  wheelPulses[0]-=pulseDelta;wheelPulses[2]-=pulseDelta;wheelPulses[1]+=pulseDelta;wheelPulses[3]+=pulseDelta;
  for(int i=0;i<20;++i) tick();
  auto calibration=bot::controllerSnapshot();
  assert(std::strcmp(calibration.resultType,"turn_left_cal")==0&&calibration.calibrationTurnFactor==1.2f);
  fresh();cfg.turnFactorLeft=1.2f;cfg.turnFactorRight=1.4f;
  auto rightCal=command(CommandType::TurnCalStart);rightCal.value=-360;rightCal.directionalCalibration=true;
  assert(bot::controllerEnqueue(rightCal));tick();
  pulseDelta=int64_t(std::round(bot::wheelTurnDistance(-360,cfg)*bot::pulsesPerCm(cfg)));
  wheelPulses[0]-=pulseDelta;wheelPulses[2]-=pulseDelta;wheelPulses[1]+=pulseDelta;wheelPulses[3]+=pulseDelta;
  for(int i=0;i<20;++i) tick();
  calibration=bot::controllerSnapshot();
  assert(std::strcmp(calibration.resultType,"turn_right_cal")==0&&calibration.calibrationTurnFactor==1.4f);
  // 登台观测只记录倾角、FG 行程、支撑变化与时间；期间所有电机 PWM 必须保持零。
  fresh();auto observe=command(CommandType::ClimbObserveStart);
  assert(bot::controllerEnqueue(observe));tick();
  assert(bot::controllerBusy());assert(zero());
  sensors.acc[0]=0.5f;sensors.acc[2]=0.8660254f;sensors.e18[0]=false;++wheelPulses[0];tick(10);
  auto observeEnd=command(CommandType::ClimbObserveEnd);
  assert(bot::controllerEnqueue(observeEnd));tick(10);calibration=bot::controllerSnapshot();
  assert(!bot::controllerBusy()&&zero()&&std::strcmp(calibration.resultType,"climb_observe")==0);
  assert(calibration.calibrationDurationMs>=10&&calibration.calibrationMaxTiltDeg>29&&calibration.calibrationMaxTiltDeg<31);
  assert(calibration.calibrationSupportChanges==1);
  assert(calibration.calibrationSessionId==4321&&calibration.resultConfigRevision==cfg.revision);
  // 结束指令不能把本周期掉线/过期的传感记录发布为成功结果，也不能占住空闲控制器。
  fresh();assert(bot::controllerEnqueue(command(CommandType::ClimbObserveEnd)));tick();
  assert(!bot::controllerBusy()&&zero());
  fresh();assert(bot::controllerEnqueue(command(CommandType::ClimbObserveStart)));tick();
  refreshSensors=false;assert(bot::controllerEnqueue(command(CommandType::ClimbObserveEnd)));tick(100);
  assert(!bot::controllerBusy()&&zero()&&!bot::controllerSnapshot().resultReady);
  fresh();assert(bot::controllerEnqueue(command(CommandType::ClimbObserveStart)));tick();
  assert(bot::controllerEnqueue(command(CommandType::ClimbObserveEnd)));tick(1100);
  assert(!bot::controllerBusy()&&zero()&&!bot::controllerSnapshot().resultReady);
  // 观测自动止于60秒，按时续心跳仍不能延长时限；数据仅回报、不能自动写配置。
  fresh();observe=command(CommandType::ClimbObserveStart);assert(bot::controllerEnqueue(observe));tick();
  for(int window=0;window<120 && bot::controllerBusy();++window) {
    assert(bot::controllerEnqueue(command(CommandType::Heartbeat)));
    for(int i=0;i<50;++i) tick();
  }
  calibration=bot::controllerSnapshot();
  assert(!bot::controllerBusy()&&zero()&&calibration.resultReady&&std::strcmp(calibration.resultType,"climb_observe")==0);
  assert(calibration.calibrationDurationMs==60000);
  // 没有心跳时立即丢弃未完成样本，而不是保存成一次成功观测。
  fresh();observe=command(CommandType::ClimbObserveStart);assert(bot::controllerEnqueue(observe));tick();tick(1100);
  assert(!bot::controllerBusy()&&zero()&&!bot::controllerSnapshot().resultReady);
  assert(std::strcmp(bot::controllerSnapshot().fault,"climb_observe_disconnected")==0);
  std::puts("PASS: controller safety / dev-mode PWM and sensor gate / autonomous sensor gate / left-right calibration / climb observation / takeover / expiry / pose anchor");
}
