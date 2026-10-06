/**
 * @file    test_arena_model.cpp
 * @brief   对生产自主算法执行确定性场景验证，覆盖定位、分层导航与危险优先级。
 * @note    虚拟测量不证明真实传感精度、碰撞安全、爬台能力或机械停车距离。
 */
#include "arena_model.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace bot::arena;
namespace {
Frame good(uint32_t now=100) {
  Frame f; f.nowMs=now; f.dtS=0.05f;
  f.imuOk=f.imuFresh=f.accelOk=f.digitalOk=f.digitalFresh=true;
  for(int i=0;i<3;++i) f.groundPresent[i]=f.rawGroundPresent[i]=true;
  for(auto& sampled:f.irSampledMs) sampled=now;
  return f;
}
Config armed() { Config c; c.armed=true; c.climbEnabled=true; return c; }
bool near(float a,float b,float tolerance=0.02f) { return std::fabs(a-b)<tolerance; }
void advance(ArenaModel& m,Frame& f,float ds=0,float yaw=0,bool sampleIr=true) {
  f.nowMs+=50; f.deltaCm=ds; f.deltaYawDeg=yaw;
  if(sampleIr) for(auto& sampled:f.irSampledMs) sampled=f.nowMs;
  m.tick(f);
}
void readyClimb(ArenaModel& m,Frame& f) {
  assert(m.setPoseAnchor(154,0,180,Layer::Lower,f.nowMs));
  assert(m.startClimb(f.nowMs)); advance(m,f); advance(m,f);
  assert(m.snapshot().phase==Phase::ClimbContact);
}
void finishClimb(ArenaModel& m,Frame& f) {
  assert(m.snapshot().phase==Phase::ClimbContact);
  f.tiltDeg=18; advance(m,f,4);
  assert(m.snapshot().phase==Phase::ClimbCrest);
  f.tiltDeg=3;
  for(int i=0;i<75 && m.snapshot().phase==Phase::ClimbCrest;++i) advance(m,f,1);
  assert(m.snapshot().phase==Phase::ClimbSettle);
  for(int i=0;i<10;++i) advance(m,f);
}
}
int main() {
  Config c=armed(); assert(configValid(c));
  Config invalid=c; invalid.attackSpeedCmS=std::numeric_limits<float>::infinity();
  assert(!configValid(invalid)); invalid=c; invalid.entryOffsetCm=110; assert(!configValid(invalid));
  invalid=c; invalid.bodyRadiusCm=40; assert(!configValid(invalid));
  assert(near(wrapDegrees(721),1)); assert(near(wrapDegrees(-181),179));
  // 自主默认关闭；无锚定、错误层面与不安全起点不允许隐式启动。
  ArenaModel off; assert(off.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(!off.startBattle(100) && off.snapshot().reason==StopReason::NotArmed);
  ArenaModel m(c); Frame f=good();
  assert(!m.startBattle(100));
  assert(!m.setPoseAnchor(0,0,0,Layer::Lower,100));
  assert(!m.setPoseAnchor(170,0,0,Layer::UpperAnchored,100));
  assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  advance(m,f,10); assert(near(m.snapshot().pose.xCm,10));
  advance(m,f,-5); assert(near(m.snapshot().pose.xCm,5));
  advance(m,f,0,90); advance(m,f,10);
  assert(near(m.snapshot().pose.yCm,10));
  const Pose once=m.snapshot().pose; m.tick(f); assert(near(m.snapshot().pose.yCm,once.yCm));
  const float error=m.snapshot().pose.uncertaintyCm;
  f.impact=true; advance(m,f); assert(m.snapshot().pose.uncertaintyCm>=error+8);
  m.cancel(); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(m.startNavigate(40,0,100)); assert(!m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  // 正常导航在目标处停止；人工取消及模式切换不得留下旧速度需求。
  Decision d=m.tick(f); assert(d.forwardCmS>0 && near(d.yawDegS,0));
  for(int i=0;i<400 && m.snapshot().mode==Mode::Navigate;++i) {
    d=m.snapshot(); advance(m,f,d.forwardCmS*f.dtS,d.yawDegS*f.dtS);
  }
  assert(m.snapshot().reason==StopReason::Complete && m.snapshot().mode==Mode::Idle);
  assert(m.startBattle(f.nowMs)); m.cancel(); assert(m.snapshot().forwardCmS==0);
  // 上层禁止越过安全边界；上下层之间的台边危险带不能作为导航目标。
  f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(!m.startNavigate(110,0,100) && m.snapshot().reason==StopReason::InvalidGoal);
  assert(m.setPoseAnchor(155,0,90,Layer::Lower,100));
  assert(!m.startNavigate(130,0,100));
  // 使用生产可见图绕中心走到对侧；轨迹每一步均避开膨胀台面。
  Config routeConfig=c; routeConfig.driftPerCm=routeConfig.driftPerTurnDeg=0.001f;
  routeConfig.driftPerSecondCm=0.001f; ArenaModel around(routeConfig); f=good();
  assert(around.setPoseAnchor(155,0,90,Layer::Lower,100));
  assert(around.startNavigate(-155,0,100)); d=around.tick(f);
  bool sawNorth=false;
  for(int i=0;i<2500 && around.snapshot().mode==Mode::Navigate;++i) {
    d=around.snapshot(); advance(around,f,d.forwardCmS*f.dtS,d.yawDegS*f.dtS);
    const Pose p=around.snapshot().pose;
    const float obstacle=120+routeConfig.bodyRadiusCm+routeConfig.safetyMarginCm+p.uncertaintyCm;
    assert(std::fabs(p.xCm)>=obstacle-0.05f || std::fabs(p.yCm)>=obstacle-0.05f);
    sawNorth=sawNorth || p.yCm>145;
  }
  if(around.snapshot().reason!=StopReason::Complete)
    std::printf("route stopped: %s %.2f %.2f\n",reasonName(around.snapshot().reason),
      around.snapshot().pose.xCm,around.snapshot().pose.yCm);
  assert(around.snapshot().reason==StopReason::Complete && sawNorth);
  // 原始缺地先于稳定去抖；前缺地后退、后缺地前进、双侧危险停止。
  m=ArenaModel(c); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(m.startBattle(100)); f.rawGroundPresent[0]=false; d=m.tick(f);
  assert(d.mode==Mode::Escape && d.forwardCmS<0);
  f.rawGroundPresent[2]=false; advance(m,f); assert(m.snapshot().reason==StopReason::EdgeBlocked);
  m.cancel(); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(m.startBattle(100)); f.rawGroundPresent[2]=false; d=m.tick(f); assert(d.forwardCmS>0);
  f.rawGroundPresent[2]=true;
  for(int i=0;i<6;++i) advance(m,f);
  assert(m.snapshot().mode==Mode::Battle);
  m.cancel(); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(m.startBattle(100)); f.rawGroundPresent[0]=false; m.tick(f);
  for(int i=0;i<45;++i) advance(m,f);
  assert(m.snapshot().reason==StopReason::EscapeTimeout);
  // 台面中央亮色不认作边缘；必要输入失效立即停止，不借无效测距进攻。
  m.cancel(); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(m.startBattle(100)); for(bool& b:f.grayBright) b=true; d=m.tick(f);
  assert(d.mode==Mode::Battle && d.forwardCmS==0 && d.yawDegS!=0);
  f.digitalFresh=false; advance(m,f); assert(m.snapshot().reason==StopReason::SensorUnavailable);
  m.cancel(); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(m.startBattle(100)); f.irValid[0]=true; f.irCm[0]=28;
  m.tick(f); advance(m,f); advance(m,f); assert(m.snapshot().phase==Phase::Push);
  for(int i=0;i<31;++i) advance(m,f);
  assert(m.snapshot().phase==Phase::Retreat && m.snapshot().forwardCmS<=0);
  // 重复缓存不能凑够三次确认；目标时效使用采样时刻而非消费时刻。
  m=ArenaModel(c); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(m.startBattle(100)); f.irValid[0]=true; f.irCm[0]=28; m.tick(f);
  for(int i=0;i<3;++i) advance(m,f,0,0,false);
  assert(!m.snapshot().target.valid && m.snapshot().phase==Phase::Search);
  f.irSampledMs[0]=260; advance(m,f,0,0,false);
  assert(!m.snapshot().target.valid);
  f.irSampledMs[0]=310; advance(m,f,0,0,false);
  assert(m.snapshot().target.valid && m.snapshot().target.observedMs==310);
  for(int i=0;i<7;++i) advance(m,f,0,0,false);
  assert(!m.snapshot().target.valid && m.snapshot().phase==Phase::Retreat);
  // 分时采样的其它空通道不打断候选；候选自身的新负测量撤销未确认序列。
  m=ArenaModel(c); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(m.startBattle(100)); f.irValid[0]=true; f.irCm[0]=28; m.tick(f);
  f.irSampledMs[1]=150; advance(m,f,0,0,false);
  f.irSampledMs[0]=200; advance(m,f,0,0,false);
  assert(!m.snapshot().target.valid);
  f.irValid[0]=false; f.irSampledMs[0]=250; advance(m,f,0,0,false);
  f.irValid[0]=true;
  for(int i=0;i<2;++i) { f.irSampledMs[0]=f.nowMs+50; advance(m,f,0,0,false); }
  assert(!m.snapshot().target.valid);
  f.irSampledMs[0]=f.nowMs+50; advance(m,f,0,0,false);
  assert(m.snapshot().target.valid);
  // 入口允许标志不能覆盖未来、过期和倒序采样；同一毫秒不能重复确认。
  m=ArenaModel(c); f=good(500); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,500));
  assert(m.startBattle(500)); f.irValid[0]=true; f.irCm[0]=28;
  f.irSampledMs[0]=600; m.tick(f); assert(!m.snapshot().target.valid);
  f.irSampledMs[0]=100; advance(m,f,0,0,false); assert(!m.snapshot().target.valid);
  f.irSampledMs[0]=590; advance(m,f,0,0,false);
  f.irSampledMs[0]=580; advance(m,f,0,0,false);
  f.irSampledMs[0]=690; advance(m,f,0,0,false);
  assert(!m.snapshot().target.valid);
  f.irSampledMs[0]=740; advance(m,f,0,0,false);
  assert(m.snapshot().target.valid && m.snapshot().target.observedMs==740);
  // millis 回绕后新样本继续推进，回绕前的旧缓存不能借数值更大而重放。
  m=ArenaModel(c); f=good(0xfffffff0u);
  assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,f.nowMs)); assert(m.startBattle(f.nowMs));
  f.irValid[0]=true; f.irCm[0]=28; m.tick(f); advance(m,f); advance(m,f);
  assert(m.snapshot().target.valid && m.snapshot().target.observedMs==f.nowMs);
  const uint32_t confirmed=f.nowMs; f.irSampledMs[0]=0xfffffff0u;
  advance(m,f,0,0,false); assert(m.snapshot().target.observedMs==confirmed);
  // 同一采样毫秒的其它通道和长期断续的观测不能拼成连续三次证据。
  m=ArenaModel(c); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100));
  assert(m.startBattle(100)); f.irValid[0]=true; f.irCm[0]=60; m.tick(f);
  f.irValid[1]=true; f.irCm[1]=60; advance(m,f,0,0,false);
  assert(!m.snapshot().target.valid);
  for(int i=0;i<8;++i) advance(m,f,0,0,false);
  f.irSampledMs[0]=f.nowMs+50; advance(m,f,0,0,false);
  f.irSampledMs[0]=f.nowMs+50; advance(m,f,0,0,false);
  assert(!m.snapshot().target.valid);
  f.irSampledMs[0]=f.nowMs+50; advance(m,f,0,0,false);
  assert(m.snapshot().target.valid);
  // 贴近已知外墙的回波只参与地图排除，不当作敌人。
  Config wallConfig=c; wallConfig.climbEnabled=false; m=ArenaModel(wallConfig);
  f=good(); assert(m.setPoseAnchor(155,0,0,Layer::Lower,100));
  assert(m.startBattle(100)); f.irValid[0]=true; f.irCm[0]=20;
  for(int i=0;i<5;++i) advance(m,f);
  assert(!m.snapshot().target.valid);
  // 登台成功要求入口对正、有效倾角上升、进展、越顶及支撑停稳事件链。
  ArenaModel climbing(c); f=good(); readyClimb(climbing,f);
  f.tiltDeg=18; advance(climbing,f,4);
  assert(climbing.snapshot().phase==Phase::ClimbCrest);
  f.tiltDeg=3;
  for(int i=0;i<67 && climbing.snapshot().phase!=Phase::ClimbSettle;++i) advance(climbing,f,1);
  assert(climbing.snapshot().phase==Phase::ClimbSettle);
  for(int i=0;i<10;++i) advance(climbing,f);
  d=climbing.snapshot(); assert(d.mode==Mode::Idle && d.climbEstimated);
  assert(d.pose.layer==Layer::UpperEstimated && d.pose.uncertaintyCm>c.anchorUncertaintyCm+c.climbAddedUncertaintyCm);
  assert(climbing.startBattle(f.nowMs)); climbing.cancel();
  // 一键格斗从低层自动入场，登台后继续寻敌；保持原任务总期限。
  Config missionConfig=c; missionConfig.taskLimitMs=6000;
  ArenaModel mission(missionConfig); f=good();
  assert(mission.setPoseAnchor(154,0,180,Layer::Lower,100)); assert(mission.startBattle(100));
  advance(mission,f); advance(mission,f); finishClimb(mission,f);
  assert(mission.snapshot().mode==Mode::Battle && mission.snapshot().pose.layer==Layer::UpperEstimated);
  assert(!mission.snapshot().hasGoal);
  for(int i=0;i<55 && mission.snapshot().mode==Mode::Battle;++i) advance(mission,f);
  assert(mission.snapshot().reason==StopReason::TaskTimeout);
  // 跨层点选目标也自动登台，完成后继续原导航并保留世界目标坐标。
  ArenaModel cross(c); f=good();
  assert(cross.setPoseAnchor(154,0,180,Layer::Lower,100)); assert(cross.startNavigate(0,0,100));
  assert(cross.snapshot().hasGoal && near(cross.snapshot().goalX,0));
  advance(cross,f); advance(cross,f); finishClimb(cross,f);
  assert(cross.snapshot().mode==Mode::Navigate);
  for(int i=0;i<800 && cross.snapshot().mode==Mode::Navigate;++i) {
    d=cross.snapshot(); advance(cross,f,d.forwardCmS*f.dtS,d.yawDegS*f.dtS);
  }
  assert(cross.snapshot().reason==StopReason::Complete && cross.snapshot().hasGoal);
  // 登台增大的实际误差使原目标变得不安全时重新拒绝，不复用旧准入结果。
  cross=ArenaModel(c); f=good();
  assert(cross.setPoseAnchor(154,0,180,Layer::Lower,100)); assert(cross.startNavigate(85.8f,0,100));
  advance(cross,f); advance(cross,f); finishClimb(cross,f);
  assert(cross.snapshot().reason==StopReason::InvalidGoal && cross.snapshot().mode==Mode::Halted);
  // 只有FG行程、没有倾角链，不能报已上台；保持未知过渡层等人工校正。
  climbing=ArenaModel(c); f=good(); readyClimb(climbing,f);
  for(int i=0;i<81 && climbing.snapshot().mode==Mode::Climb;++i) advance(climbing,f,1);
  assert(!climbing.snapshot().climbEstimated && climbing.snapshot().mode==Mode::Halted);
  assert(climbing.snapshot().pose.layer==Layer::TransitionUnknown);
  // 冲击与饱和不算倾角上升证据；登台超时与支撑风险优先。
  climbing=ArenaModel(c); f=good(); readyClimb(climbing,f);
  f.tiltDeg=18; f.impact=true; advance(climbing,f,4);
  assert(climbing.snapshot().phase!=Phase::ClimbCrest);
  climbing=ArenaModel(c); f=good(); readyClimb(climbing,f);
  for(int i=0;i<103;++i) advance(climbing,f);
  assert(climbing.snapshot().reason==StopReason::ClimbTimeout);
  climbing=ArenaModel(c); f=good(); readyClimb(climbing,f);
  f.rawGroundPresent[0]=false; advance(climbing,f);
  assert(climbing.snapshot().reason==StopReason::EdgeBlocked);
  assert(climbing.snapshot().pose.layer==Layer::TransitionUnknown);
  // 相同逻辑处理 millis 回绕；数据NaN、周期过期与有限任务期限均拒绝运动。
  Config timed=c; timed.taskLimitMs=1000; ArenaModel timer(timed); f=good(0xfffffff0u);
  assert(timer.setPoseAnchor(0,0,0,Layer::UpperAnchored,f.nowMs)); assert(timer.startBattle(f.nowMs)); timer.tick(f);
  for(int i=0;i<21;++i) advance(timer,f);
  assert(timer.snapshot().reason==StopReason::TaskTimeout);
  m.cancel(); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100)); assert(m.startBattle(100));
  m.tick(f); f.nowMs+=300; d=m.tick(f); assert(d.reason==StopReason::SensorUnavailable);
  m.cancel(); f=good(); assert(m.setPoseAnchor(0,0,0,Layer::UpperAnchored,100)); assert(m.startBattle(100));
  f.deltaCm=std::numeric_limits<float>::quiet_NaN(); d=m.tick(f);
  assert(d.reason==StopReason::InvalidFrame && d.forwardCmS==0 && !d.pose.valid);
  std::puts("arena model: configuration, odometry, layer routing, edge priority, battle budgets and climb evidence passed");
}
