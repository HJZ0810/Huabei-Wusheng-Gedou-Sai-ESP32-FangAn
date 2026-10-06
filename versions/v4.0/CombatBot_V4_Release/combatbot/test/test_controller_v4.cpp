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
Sensors sensorsRead(const Config&){sensors.sampledMs=sensors.digitalMs=sensors.imuMs=hostMillis;for(auto& ms:sensors.irMs)ms=hostMillis;return sensors;}
bool hardwareHealthy(){return true;}
}
#include "../src/controller.cpp"
static void tick(uint32_t elapsed=10){hostMillis+=elapsed;try{hostTask(nullptr);}catch(const HostTickComplete&){} }
static void advance(uint32_t elapsed){for(uint32_t i=0;i<elapsed;i+=10)tick();}
static bot::Command command(bot::CommandType type,uint32_t client=11){bot::Command c;c.type=type;c.client=client;c.y=1;c.speed=20;return c;}
static bool zero(){for(auto p:output)if(p)return false;return true;}
static void fresh(bool calibrated=true){
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
  std::puts("PASS: real controller V4 uncalibrated rejection / ground interpretation / autonomous disconnect / manual loss-stop / finite task budget / takeover generation / cloud queue expiry / stationary pose / manual floor loss");
}
