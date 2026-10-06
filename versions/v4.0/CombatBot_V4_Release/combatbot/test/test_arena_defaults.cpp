/**
 * @file    test_arena_defaults.cpp
 * @brief   使用集成配置默认值与10ms周期验证派生入口、登台稳态及模式继续。
 * @note    轮行程、姿态与支撑为确定性注入；通过不代表实车爬台能力。
 */
#include "types.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace bot::arena;
namespace {
Config mapped(const bot::Config& c) {
  Config a; a.armed=true; a.climbEnabled=true;
  a.outerSizeCm=c.arenaOuterCm; a.platformSizeCm=c.arenaPlatformCm;
  a.bodyRadiusCm=0.5f*std::sqrt(c.arenaBodyLengthCm*c.arenaBodyLengthCm+c.arenaBodyWidthCm*c.arenaBodyWidthCm);
  a.safetyMarginCm=c.arenaMarginCm; a.navigationSpeedCmS=c.arenaNavSpeed;
  a.searchSpeedCmS=c.arenaSearchSpeed; a.attackSpeedCmS=c.arenaPushSpeed;
  a.climbSpeedCmS=c.arenaClimbSpeed; a.taskLimitMs=c.arenaTaskMs; a.climbLimitMs=c.arenaClimbMs;
  a.entrySide=EntrySide::South; a.entryOffsetCm=c.arenaEntryX;
  a.sensorFrontCm=a.sensorRearCm=c.arenaBodyLengthCm/2+5;
  a.grayFrontCm=a.grayRearCm=c.arenaBodyLengthCm/2+1.5f;
  a.grayLeftCm=a.grayRightCm=c.arenaBodyWidthCm/2-1;
  return a;
}
Frame good() {
  Frame f; f.nowMs=100; f.dtS=0.01f;
  f.imuOk=f.imuFresh=f.accelOk=f.digitalOk=f.digitalFresh=true;
  for(int i=0;i<3;++i) f.groundPresent[i]=f.rawGroundPresent[i]=true;
  return f;
}
Decision tick(ArenaModel& m,Frame& f,float cm=0,float degrees=0) {
  f.nowMs+=10; f.deltaCm=cm; f.deltaYawDeg=degrees; return m.tick(f);
}
void toSettle(ArenaModel& m,Frame& f,const Config& c) {
  tick(m,f); tick(m,f); assert(m.snapshot().phase==Phase::ClimbContact);
  float travel=0;
  for(int i=0;i<600 && m.snapshot().phase!=Phase::ClimbSettle;++i) {
    travel+=c.climbSpeedCmS*f.dtS;
    f.tiltDeg=travel<3?0:travel<25?18:3;
    tick(m,f,c.climbSpeedCmS*f.dtS);
    assert(m.snapshot().mode==Mode::Climb);
  }
  assert(m.snapshot().phase==Phase::ClimbSettle);
}
}
int main() {
  const bot::Config device;
  assert(!device.arenaEnabled && !device.arenaCalibrated && !device.arenaClimbEnabled);
  assert(device.arenaTaskMs==180000 && device.arenaClimbMs==6000);
  Config c=mapped(device); assert(configValid(c));
  const float span=c.platformSizeCm/2+c.bodyRadiusCm+c.safetyMarginCm+c.anchorUncertaintyCm+8;
  assert(span>155 && span<156);
  ArenaModel m(c); Frame f=good();
  assert(m.setPoseAnchor(0,-span,90,Layer::Lower,f.nowMs));
  assert(m.startBattle(f.nowMs)); toSettle(m,f,c);
  const float y=m.snapshot().pose.yCm;
  assert(y>-84 && y<-80 && m.snapshot().pose.layer==Layer::TransitionUnknown);
  // 10ms内0.05cm仍是5cm/s，不能借每拍小位移进入“停稳”。
  for(int i=0;i<60;++i) {
    const Decision d=tick(m,f,0.05f);
    assert(d.phase==Phase::ClimbSettle && !d.climbEstimated);
  }
  // 平移已停但每拍0.1°是10°/s，同样必须重新开始稳态窗口。
  for(int i=0;i<30;++i) {
    const Decision d=tick(m,f,0,0.1f);
    assert(d.phase==Phase::ClimbSettle && !d.climbEstimated);
  }
  for(int i=0;i<46;++i) tick(m,f);
  Decision d=m.snapshot();
  assert(d.mode==Mode::Battle && d.pose.layer==Layer::UpperEstimated && d.climbEstimated);
  const float half=c.platformSizeCm/2-c.bodyRadiusCm-c.safetyMarginCm-d.pose.uncertaintyCm;
  assert(std::fabs(d.pose.yCm)<=half && d.pose.uncertaintyCm>10);
  // 同一生产默认值用于跨层目标；质量降级后继续核验，不被普通边缘预测全拦。
  ArenaModel nav(c); f=good(); assert(nav.setPoseAnchor(0,-span,90,Layer::Lower,f.nowMs));
  assert(nav.startNavigate(0,0,f.nowMs)); toSettle(nav,f,c);
  for(int i=0;i<46;++i) tick(nav,f);
  assert(nav.snapshot().mode==Mode::Navigate);
  for(int i=0;i<1500 && nav.snapshot().mode==Mode::Navigate;++i) {
    const Decision before=nav.snapshot();
    tick(nav,f,before.forwardCmS*f.dtS,before.yawDegS*f.dtS);
  }
  assert(nav.snapshot().reason==StopReason::Complete && nav.snapshot().pose.layer==Layer::UpperEstimated);
  // 充分姿态和行程也不能覆盖原始缺地；登台应停止等待校正。
  ArenaModel lost(c); f=good(); assert(lost.setPoseAnchor(0,-span,90,Layer::Lower,f.nowMs));
  assert(lost.startBattle(f.nowMs)); tick(lost,f); tick(lost,f);
  f.rawGroundPresent[0]=false; d=tick(lost,f);
  assert(d.mode==Mode::Halted && d.reason==StopReason::EdgeBlocked && d.pose.layer==Layer::TransitionUnknown);
  std::printf("integrated defaults: radius=%.3fcm, South entry=(0,%.3f), climbMs=%u, upper error=%.3fcm; 10ms settle and full mission passed\n",
    c.bodyRadiusCm,-span,unsigned(c.climbLimitMs),m.snapshot().pose.uncertaintyCm);
}
