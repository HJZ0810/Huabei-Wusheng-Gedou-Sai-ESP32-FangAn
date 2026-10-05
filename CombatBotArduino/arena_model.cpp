/**
 * @file    arena_model.cpp
 * @brief   固定容量的分层导航、估计定位、格斗与登台状态机。
 *
 * 算法约定：导航用可见图绕开膨胀后的中央台面；安全检查先于任何运动需求。
 *           登台依赖入口、进展、倾角变化与支撑稳定事件链，结果仍是估计层面。
 * 设计边界：不借冲击或无效读数证明登台；不将未知测距解释为敌人或道路畅通。
 */
#include "arena_model.h"
#include <cmath>
#include <algorithm>

namespace bot { namespace arena {
namespace {
const float Pi=3.14159265358979323846f;
bool finite(float x) { return std::isfinite(x); }
float limit(float x,float a,float b) { return std::max(a,std::min(x,b)); }
float distance(float x,float y) { return std::sqrt(x*x+y*y); }
bool elapsed(uint32_t now,uint32_t start,uint32_t duration) {
  const uint32_t d=now-start; return d<0x80000000u && d>=duration;
}
bool upper(Layer l) { return l==Layer::UpperAnchored || l==Layer::UpperEstimated; }
bool inside(float x,float y,float half) { return std::fabs(x)<=half && std::fabs(y)<=half; }
float angle(float x,float y) { return std::atan2(y,x)*180/Pi; }
/** @brief 判断线段是否穿越矩形内部；沿膨胀边界的切线路径允许通过。 */
bool blocked(float ax,float ay,float bx,float by,float half) {
  half-=0.02f;
  float lo=0,hi=1;
  const float a[2]={ax,ay}, d[2]={bx-ax,by-ay};
  for(int i=0;i<2;++i) {
    if(std::fabs(d[i])<0.00001f) { if(std::fabs(a[i])>=half) return false; }
    else {
      float p=(-half-a[i])/d[i],q=(half-a[i])/d[i];
      if(p>q) std::swap(p,q);
      lo=std::max(lo,p); hi=std::min(hi,q);
      if(lo>=hi) return false;
    }
  }
  return lo<hi && hi>0 && lo<1;
}
/** @brief 六节点可见图：起点、终点与台面四角；无堆内存和搜索循环等待。 */
bool route(float sx,float sy,float gx,float gy,float obstacle,float outer,float& nx,float& ny) {
  const float corner=obstacle+5;
  if(corner>=outer || !inside(sx,sy,outer) || !inside(gx,gy,outer) ||
      inside(sx,sy,obstacle-0.02f) || inside(gx,gy,obstacle-0.02f)) return false;
  const float x[6]={sx,gx,corner,-corner,-corner,corner};
  const float y[6]={sy,gy,corner,corner,-corner,-corner};
  float cost[6]={0,1e9f,1e9f,1e9f,1e9f,1e9f};
  int previous[6]={-1,-1,-1,-1,-1,-1}; bool used[6]={false};
  for(int pass=0;pass<6;++pass) {
    int u=-1; for(int i=0;i<6;++i) if(!used[i] && (u<0 || cost[i]<cost[u])) u=i;
    if(u<0 || cost[u]>=1e9f) break;
    used[u]=true;
    for(int v=0;v<6;++v) if(!used[v] && !blocked(x[u],y[u],x[v],y[v],obstacle)) {
      const float candidate=cost[u]+distance(x[v]-x[u],y[v]-y[u]);
      if(candidate<cost[v]) { cost[v]=candidate; previous[v]=u; }
    }
  }
  if(previous[1]<0) return false;
  int next=1;
  for(int count=0;previous[next]!=0 && count<6;++count) {
    next=previous[next]; if(next<0) return false;
  }
  if(previous[next]!=0) return false;
  nx=x[next]; ny=y[next]; return true;
}
}

float wrapDegrees(float x) {
  if(!finite(x)) return 0;
  x=std::fmod(x+180,360); if(x<0) x+=360; return x-180;
}
bool configValid(const Config& c) {
  const float values[]={c.outerSizeCm,c.platformSizeCm,c.bodyRadiusCm,c.safetyMarginCm,
    c.maxUncertaintyCm,c.anchorUncertaintyCm,c.anchorHeadingUncertaintyDeg,c.driftPerCm,
    c.driftPerTurnDeg,c.driftPerSecondCm,c.searchSpeedCmS,c.navigationSpeedCmS,
    c.attackSpeedCmS,c.maxYawDegS,c.headingToleranceDeg,c.goalToleranceCm,
    c.targetMaxCm,c.targetMinCm,c.targetAgreementCm,c.escapeSpeedCmS,c.escapeMaxCm,
    c.sensorFrontCm,c.sensorRearCm,c.grayFrontCm,c.grayRearCm,c.grayLeftCm,c.grayRightCm,
    c.entryOffsetCm,c.entryToleranceCm,c.climbSpeedCmS,c.climbMaxCm,c.climbRiseDeg,
    c.climbLevelDeg,c.maxTiltDeg,c.climbAddedUncertaintyCm,c.predictionSeconds};
  for(float v:values) if(!finite(v)) return false;
  const float clearance=c.bodyRadiusCm+c.safetyMarginCm+c.anchorUncertaintyCm;
  if(c.outerSizeCm<200 || c.outerSizeCm>1000 || c.platformSizeCm<100 ||
    c.platformSizeCm>=c.outerSizeCm || c.bodyRadiusCm<5 || c.bodyRadiusCm>60 ||
    c.safetyMarginCm<1 || c.safetyMarginCm>30 ||
    c.outerSizeCm/2-clearance<=c.platformSizeCm/2+clearance+1) return false;
  if(c.anchorUncertaintyCm<0.5f || c.maxUncertaintyCm<c.anchorUncertaintyCm ||
    c.maxUncertaintyCm>100 || c.anchorHeadingUncertaintyDeg<0 ||
    c.anchorHeadingUncertaintyDeg>20 || c.driftPerCm<0 || c.driftPerCm>0.2f ||
    c.driftPerTurnDeg<0 || c.driftPerTurnDeg>0.2f || c.driftPerSecondCm<0 ||
    c.driftPerSecondCm>1) return false;
  const float speeds[]={c.searchSpeedCmS,c.navigationSpeedCmS,c.attackSpeedCmS,
    c.escapeSpeedCmS,c.climbSpeedCmS};
  for(float v:speeds) if(v<=0 || v>80) return false;
  if(c.maxYawDegS<5 || c.maxYawDegS>180 || c.headingToleranceDeg<1 ||
    c.headingToleranceDeg>30 || c.goalToleranceCm<1 || c.goalToleranceCm>15 ||
    c.targetMinCm<20 || c.targetMaxCm>150 || c.targetMaxCm<=c.targetMinCm ||
    c.targetAgreementCm<1 || c.targetAgreementCm>60 || c.escapeMaxCm<2 ||
    c.escapeMaxCm>40 || c.predictionSeconds<0.1f || c.predictionSeconds>2) return false;
  const float mounts[]={c.sensorFrontCm,c.sensorRearCm,c.grayFrontCm,c.grayRearCm,
    c.grayLeftCm,c.grayRightCm};
  for(float v:mounts) if(v<1 || v>60) return false;
  if(c.entryToleranceCm<2 || c.entryToleranceCm>30 ||
    std::fabs(c.entryOffsetCm)+clearance+c.entryToleranceCm>=c.platformSizeCm/2 ||
    c.climbMaxCm<20 || c.climbMaxCm>150 || c.climbLevelDeg<1 ||
    c.climbRiseDeg<=c.climbLevelDeg || c.climbRiseDeg>=c.maxTiltDeg ||
    c.maxTiltDeg>60 || c.climbAddedUncertaintyCm<3 || c.climbAddedUncertaintyCm>30 ||
    unsigned(c.entrySide)>unsigned(EntrySide::South)) return false;
  return c.taskLimitMs>=1000 && c.taskLimitMs<=600000 && c.targetHoldMs>=50 &&
    c.targetHoldMs<=1000 && c.pushLimitMs>=100 && c.pushLimitMs<=5000 &&
    c.escapeLimitMs>=300 && c.escapeLimitMs<=5000 && c.groundClearMs>=100 &&
    c.groundClearMs<=1000 && c.climbLimitMs>=1000 && c.climbLimitMs<=15000 &&
    c.climbSettleMs>=250 && c.climbSettleMs<=2000;
}
ArenaModel::ArenaModel(const Config& c):config_(c) {
  if(!configValid(c)) halt(StopReason::InvalidConfig);
}
bool ArenaModel::configure(const Config& c) {
  if(!configValid(c)) { halt(StopReason::InvalidConfig); return false; }
  config_=c; cancel(); return true;
}
void ArenaModel::halt(StopReason reason) {
  if(result_.mode==Mode::Climb && result_.phase>=Phase::ClimbContact &&
      result_.phase<=Phase::ClimbSettle) result_.pose.layer=Layer::TransitionUnknown;
  result_.mode=Mode::Halted; result_.reason=reason;
  result_.forwardCmS=result_.yawDegS=0;
}
void ArenaModel::cancel(StopReason reason) {
  if(result_.mode==Mode::Climb && result_.phase>=Phase::ClimbContact &&
      result_.phase<=Phase::ClimbSettle) result_.pose.layer=Layer::TransitionUnknown;
  result_.mode=Mode::Idle; result_.phase=Phase::None; result_.reason=reason;
  result_.forwardCmS=result_.yawDegS=0; result_.target=Target(); candidate_=Target();
  result_.hasGoal=false;
  resumeMode_=postClimbMode_=Mode::Idle; clearTiming_=false; consistentTargetFrames_=0;
}
bool ArenaModel::setPoseAnchor(float x,float y,float heading,Layer layer,uint32_t now) {
  if(result_.mode!=Mode::Idle && result_.mode!=Mode::Halted) return false;
  if(!configValid(config_) || !finite(x) || !finite(y) || !finite(heading) ||
      (layer!=Layer::Lower && layer!=Layer::UpperAnchored)) return false;
  const Pose old=result_.pose;
  Pose p; p.xCm=x; p.yCm=y; p.headingDeg=wrapDegrees(heading);
  p.uncertaintyCm=config_.anchorUncertaintyCm;
  p.headingUncertaintyDeg=config_.anchorHeadingUncertaintyDeg;
  p.layer=layer; p.valid=true; p.updatedMs=p.anchoredMs=now; result_.pose=p;
  if(!poseSafe()) { result_.pose=old; return false; }
  cancel(StopReason::None); frameSeen_=false; disturbanceSeen_=false;
  // 人工锚定建立新的空间参考；首份缓存至多计一次，其后仍须独立新采样。
  for(bool& seen:irSeen_) seen=false;
  return true;
}
bool ArenaModel::start(Mode mode,uint32_t now) {
  if(!configValid(config_)) { halt(StopReason::InvalidConfig); return false; }
  if(!config_.armed) { halt(StopReason::NotArmed); return false; }
  if(!result_.pose.valid || !poseSafe()) { halt(StopReason::PoseRequired); return false; }
  if(result_.mode!=Mode::Idle && result_.mode!=Mode::Halted) cancel();
  result_.mode=mode; result_.reason=StopReason::None; result_.target=Target();
  result_.climbEstimated=false; taskStartedMs_=phaseStartedMs_=now;
  result_.forwardCmS=result_.yawDegS=0; candidate_=Target(); consistentTargetFrames_=0;
  return true;
}
bool ArenaModel::startNavigate(float x,float y,uint32_t now) {
  if(!finite(x) || !finite(y)) { halt(StopReason::InvalidGoal); return false; }
  const Pose p=result_.pose;
  const float upperGoalHalf=config_.platformSizeCm/2-config_.bodyRadiusCm-config_.safetyMarginCm-
    p.uncertaintyCm-config_.climbAddedUncertaintyCm;
  if(p.layer==Layer::Lower && inside(x,y,upperGoalHalf)) {
    if(!startClimb(now)) return false;
    postClimbMode_=Mode::Navigate; goalX_=x; goalY_=y;
    result_.hasGoal=true; result_.goalX=x; result_.goalY=y; return true;
  }
  result_.pose.xCm=x; result_.pose.yCm=y; const bool safe=poseSafe(); result_.pose=p;
  if(!safe) { halt(StopReason::InvalidGoal); return false; }
  if(!start(Mode::Navigate,now)) return false;
  goalX_=x; goalY_=y; result_.hasGoal=true; result_.goalX=x; result_.goalY=y;
  setPhase(Phase::Travel,now); return true;
}
bool ArenaModel::startBattle(uint32_t now) {
  if(result_.pose.layer==Layer::Lower && config_.climbEnabled) {
    if(!startClimb(now)) return false;
    postClimbMode_=Mode::Battle; return true;
  }
  if(!start(Mode::Battle,now)) return false;
  result_.hasGoal=false;
  setPhase(Phase::Search,now); return true;
}
bool ArenaModel::startClimb(uint32_t now) {
  if(!config_.climbEnabled) { halt(StopReason::ClimbUnsupported); return false; }
  if(result_.pose.layer!=Layer::Lower) { halt(StopReason::ClimbUnsupported); return false; }
  if(!start(Mode::Climb,now)) return false;
  postClimbMode_=Mode::Idle;
  const float span=config_.platformSizeCm/2+config_.bodyRadiusCm+
    config_.safetyMarginCm+result_.pose.uncertaintyCm+8;
  climbEntryX_=climbEntryY_=config_.entryOffsetCm;
  switch(config_.entrySide) {
    case EntrySide::East: climbEntryX_=span; climbHeading_=180; break;
    case EntrySide::North: climbEntryY_=span; climbHeading_=-90; break;
    case EntrySide::West: climbEntryX_=-span; climbHeading_=0; break;
    case EntrySide::South: climbEntryY_=-span; climbHeading_=90; break;
  }
  result_.hasGoal=true; result_.goalX=climbEntryX_; result_.goalY=climbEntryY_;
  setPhase(Phase::ClimbApproach,now); climbRiseSeen_=false; return true;
}
void ArenaModel::setPhase(Phase phase,uint32_t now) {
  result_.phase=phase; phaseStartedMs_=now; clearTiming_=false;
}
void ArenaModel::integrate(const Frame& f) {
  Pose& p=result_.pose;
  if(!p.valid) return;
  const float mid=(p.headingDeg+f.deltaYawDeg*0.5f)*Pi/180;
  p.xCm+=f.deltaCm*std::cos(mid); p.yCm+=f.deltaCm*std::sin(mid);
  p.headingDeg=wrapDegrees(p.headingDeg+f.deltaYawDeg);
  p.uncertaintyCm+=std::fabs(f.deltaCm)*config_.driftPerCm+
    std::fabs(f.deltaYawDeg)*config_.driftPerTurnDeg+f.dtS*config_.driftPerSecondCm;
  p.headingUncertaintyDeg+=std::fabs(f.deltaYawDeg)*0.005f+f.dtS*0.02f;
  if((f.impact || f.accelSaturated) && !disturbanceSeen_) {
    p.uncertaintyCm+=8; p.headingUncertaintyDeg+=3;
  }
  disturbanceSeen_=f.impact || f.accelSaturated;
  p.updatedMs=f.nowMs; travelCm_+=std::fabs(f.deltaCm);
}
bool ArenaModel::poseSafe(bool transition) const {
  const Pose& p=result_.pose;
  if(!p.valid || p.uncertaintyCm>config_.maxUncertaintyCm ||
      p.headingUncertaintyDeg>30) return false;
  const float margin=config_.bodyRadiusCm+config_.safetyMarginCm+p.uncertaintyCm;
  if(!inside(p.xCm,p.yCm,config_.outerSizeCm/2-margin)) return false;
  if(transition && (p.layer==Layer::Lower || p.layer==Layer::TransitionUnknown)) return true;
  if(p.layer==Layer::Lower) return !inside(p.xCm,p.yCm,config_.platformSizeCm/2+margin-0.02f);
  return upper(p.layer) && inside(p.xCm,p.yCm,config_.platformSizeCm/2-margin);
}
bool ArenaModel::navigationDemand(float x,float y,bool lowerRoute) {
  const Pose& p=result_.pose;
  const float goalDistance=distance(x-p.xCm,y-p.yCm);
  if(goalDistance<=config_.goalToleranceCm) return true;
  float nx=x,ny=y;
  if(lowerRoute) {
    const float margin=config_.bodyRadiusCm+config_.safetyMarginCm+p.uncertaintyCm;
    if(!route(p.xCm,p.yCm,x,y,config_.platformSizeCm/2+margin,
      config_.outerSizeCm/2-margin,nx,ny)) { halt(StopReason::InvalidGoal); return false; }
  }
  const float error=wrapDegrees(angle(nx-p.xCm,ny-p.yCm)-p.headingDeg);
  result_.yawDegS=limit(error*2,-config_.maxYawDegS,config_.maxYawDegS);
  if(std::fabs(error)<config_.headingToleranceDeg)
    result_.forwardCmS=std::min(config_.navigationSpeedCmS,goalDistance*1.2f);
  return false;
}
bool ArenaModel::edgeDemand(const Frame& f) {
  bool front=!f.groundPresent[0] || !f.groundPresent[1] ||
    !f.rawGroundPresent[0] || !f.rawGroundPresent[1];
  bool rear=!f.groundPresent[2] || !f.rawGroundPresent[2];
  // 白边仅在已知高台、预测探头确实接近边界时成为风险；中心图案不在此列。
  if(upper(result_.pose.layer)) {
    const float theta=result_.pose.headingDeg*Pi/180;
    for(int i=0;i<4;++i) if(f.grayBright[i]) {
      const float forward=i<2?config_.grayFrontCm:-config_.grayRearCm;
      const float left=i%2==0?config_.grayLeftCm:-config_.grayRightCm;
      const float x=result_.pose.xCm+forward*std::cos(theta)-left*std::sin(theta);
      const float y=result_.pose.yCm+forward*std::sin(theta)+left*std::cos(theta);
      if(std::max(std::fabs(x),std::fabs(y))>=config_.platformSizeCm/2-
          config_.safetyMarginCm-result_.pose.uncertaintyCm) {
        if(i<2) front=true; else rear=true;
      }
    }
  }
  if(front && rear) { halt(StopReason::EdgeBlocked); return true; }
  if(front || rear) {
    if(result_.mode!=Mode::Escape) {
      resumeMode_=result_.mode; resumePhase_=result_.phase;
      if(resumeMode_==Mode::Battle) {
        resumePhase_=Phase::Search; result_.target=Target(); candidate_=Target();
        consistentTargetFrames_=0; // 避边打断本次进攻，恢复后重新形成目标证据。
      }
      if(resumeMode_==Mode::Climb && resumePhase_>=Phase::ClimbContact) {
        result_.pose.layer=Layer::TransitionUnknown;
        halt(StopReason::EdgeBlocked); return true;
      }
      result_.mode=Mode::Escape; setPhase(Phase::Retreat,f.nowMs);
      escapeStartTravel_=travelCm_;
    }
    clearTiming_=false;
    if(elapsed(f.nowMs,phaseStartedMs_,config_.escapeLimitMs) ||
        travelCm_-escapeStartTravel_>=config_.escapeMaxCm) { halt(StopReason::EscapeTimeout); return true; }
    result_.forwardCmS=front?-config_.escapeSpeedCmS:config_.escapeSpeedCmS;
    return true;
  }
  if(result_.mode==Mode::Escape) {
    if(!clearTiming_) { clearTiming_=true; clearStartedMs_=f.nowMs; }
    if(elapsed(f.nowMs,phaseStartedMs_,config_.escapeLimitMs)) { halt(StopReason::EscapeTimeout); return true; }
    if(elapsed(f.nowMs,clearStartedMs_,config_.groundClearMs)) {
      result_.mode=resumeMode_; setPhase(resumePhase_,f.nowMs);
    }
    return true; // 恢复周期仍零输出，下一周期再规划。
  }
  return false;
}
void ArenaModel::battle(const Frame& f) {
  Target observed; observed.distanceCm=config_.targetMaxCm+1;
  unsigned observedChannel=0;
  bool candidateChannelUpdated=false;
  if(candidate_.valid && elapsed(f.nowMs,candidate_.observedMs,config_.targetHoldMs)) {
    candidate_=Target(); consistentTargetFrames_=0;
  }
  for(unsigned i=0;i<f.irCount;++i) {
    // 仅消费真正推进的采样时间戳；无符号半周期判据兼容 millis 回绕。
    // 未来、过期、重复及倒序测量都不能累计证据或延长目标有效期。
    const uint32_t age=f.nowMs-f.irSampledMs[i];
    const uint32_t step=f.irSampledMs[i]-consumedIrMs_[i];
    if(age>=0x80000000u || age>250 || age>=config_.targetHoldMs ||
        (irSeen_[i] && (step==0 || step>=0x80000000u))) continue;
    irSeen_[i]=true; consumedIrMs_[i]=f.irSampledMs[i];
    if(candidate_.valid && i==candidateChannel_) candidateChannelUpdated=true;
    if(!f.irValid[i] || !finite(f.irCm[i]) || f.irCm[i]<config_.targetMinCm ||
        f.irCm[i]>config_.targetMaxCm || f.irCm[i]>=observed.distanceCm) continue;
    // 与已知外墙一致的回波不作为进攻候选；仅提供保守地图排除，不能识别物体。
    const float bearing=wrapDegrees(-360.0f*i/f.irCount);
    const float world=(result_.pose.headingDeg+bearing)*Pi/180;
    const float dx=std::cos(world),dy=std::sin(world),half=config_.outerSizeCm/2;
    const float ox=result_.pose.xCm+config_.bodyRadiusCm*dx;
    const float oy=result_.pose.yCm+config_.bodyRadiusCm*dy;
    const float wallX=std::fabs(dx)>0.0001f?((dx>0?half:-half)-ox)/dx:1e9f;
    const float wallY=std::fabs(dy)>0.0001f?((dy>0?half:-half)-oy)/dy:1e9f;
    const float wall=std::min(wallX,wallY);
    if(wall>0 && std::fabs(f.irCm[i]-wall)<result_.pose.uncertaintyCm+10) continue;
    // 通道顺时针排列；控制输出逆时针为正。
    observed.valid=true; observed.distanceCm=f.irCm[i];
    observed.bearingDeg=bearing; observed.observedMs=f.irSampledMs[i]; observedChannel=i;
  }
  // 三次一致性来自独立采样，而非三次控制 tick。其它通道的空回波不会
  // 打断分时采样中的候选；候选自身的负测量或证据过期则重新开始确认。
  const uint32_t observationStep=observed.observedMs-candidate_.observedMs;
  if(observed.valid && (!candidate_.valid ||
      (observationStep>0 && observationStep<0x80000000u))) {
    if(candidate_.valid && std::fabs(wrapDegrees(observed.bearingDeg-candidate_.bearingDeg))<=65 &&
        std::fabs(observed.distanceCm-candidate_.distanceCm)<=config_.targetAgreementCm)
      ++consistentTargetFrames_;
    else consistentTargetFrames_=1;
    candidate_=observed; candidateChannel_=uint8_t(observedChannel);
    if(consistentTargetFrames_>=3) result_.target=observed;
  } else if(candidateChannelUpdated) {
    candidate_=Target(); consistentTargetFrames_=0;
  }
  if(result_.target.valid && elapsed(f.nowMs,result_.target.observedMs,config_.targetHoldMs))
    result_.target=Target();
  if(result_.phase==Phase::Retreat) {
    result_.target=Target(); candidate_=Target(); consistentTargetFrames_=0;
    if(elapsed(f.nowMs,phaseStartedMs_,400)) setPhase(Phase::Search,f.nowMs);
    else result_.forwardCmS=-config_.escapeSpeedCmS;
    return;
  }
  if(result_.phase==Phase::Push) {
    if(!result_.target.valid || elapsed(f.nowMs,phaseStartedMs_,config_.pushLimitMs)) {
      result_.target=Target(); setPhase(Phase::Retreat,f.nowMs); return;
    }
    result_.forwardCmS=config_.attackSpeedCmS; return;
  }
  if(!result_.target.valid) {
    setPhase(Phase::Search,f.nowMs); result_.yawDegS=config_.maxYawDegS*0.45f; return;
  }
  const float bearing=result_.target.bearingDeg;
  result_.yawDegS=limit(bearing*2,-config_.maxYawDegS,config_.maxYawDegS);
  if(std::fabs(bearing)>config_.headingToleranceDeg) { setPhase(Phase::Align,f.nowMs); return; }
  if(result_.target.distanceCm<=config_.targetMinCm+10) {
    setPhase(Phase::Push,f.nowMs); result_.forwardCmS=config_.attackSpeedCmS;
  } else { setPhase(Phase::Approach,f.nowMs); result_.forwardCmS=config_.searchSpeedCmS; }
}
void ArenaModel::climb(const Frame& f) {
  if(result_.phase==Phase::ClimbApproach) {
    if(navigationDemand(climbEntryX_,climbEntryY_,true)) setPhase(Phase::ClimbAlign,f.nowMs);
    return;
  }
  if(result_.phase==Phase::ClimbAlign) {
    const float error=wrapDegrees(climbHeading_-result_.pose.headingDeg);
    result_.yawDegS=limit(error*2,-config_.maxYawDegS,config_.maxYawDegS);
    if(std::fabs(error)<config_.headingToleranceDeg && f.tiltDeg<=config_.climbLevelDeg &&
        !f.impact && !f.accelSaturated) {
      climbStartedMs_=f.nowMs; climbStartTravel_=travelCm_;
      climbRiseSeen_=false; setPhase(Phase::ClimbContact,f.nowMs);
      result_.yawDegS=0;
    }
    return;
  }
  const float travel=travelCm_-climbStartTravel_;
  if(elapsed(f.nowMs,climbStartedMs_,config_.climbLimitMs)) { halt(StopReason::ClimbTimeout); return; }
  if(travel>=config_.climbMaxCm) { halt(StopReason::ClimbDistanceLimit); return; }
  const bool eastWest=config_.entrySide==EntrySide::East || config_.entrySide==EntrySide::West;
  const float lateral=eastWest?result_.pose.yCm:result_.pose.xCm;
  if(std::fabs(lateral-config_.entryOffsetCm)>config_.entryToleranceCm ||
      std::fabs(wrapDegrees(result_.pose.headingDeg-climbHeading_))>20) {
    halt(StopReason::ClimbEvidenceInsufficient); return;
  }
  if(result_.phase==Phase::ClimbContact) {
    result_.forwardCmS=config_.climbSpeedCmS;
    if(!f.impact && !f.accelSaturated && f.tiltDeg>=config_.climbRiseDeg && travel>=3) {
      climbRiseSeen_=true; climbRiseTravel_=travel;
      result_.pose.layer=Layer::TransitionUnknown; setPhase(Phase::ClimbCrest,f.nowMs);
    }
    return;
  }
  if(result_.phase==Phase::ClimbCrest) {
    result_.forwardCmS=config_.climbSpeedCmS;
    const float half=config_.platformSizeCm/2-config_.bodyRadiusCm-config_.safetyMarginCm-
      result_.pose.uncertaintyCm-config_.climbAddedUncertaintyCm;
    if(climbRiseSeen_ && travel-climbRiseTravel_>=8 && !f.impact && !f.accelSaturated &&
        f.tiltDeg<=config_.climbLevelDeg && inside(result_.pose.xCm,result_.pose.yCm,half)) {
      setPhase(Phase::ClimbSettle,f.nowMs); result_.forwardCmS=0;
    }
    return;
  }
  if(result_.phase==Phase::ClimbSettle) {
    if(f.impact || f.accelSaturated || f.tiltDeg>config_.climbLevelDeg ||
        std::fabs(f.deltaCm)/f.dtS>0.8f || std::fabs(f.deltaYawDeg)/f.dtS>2) {
      phaseStartedMs_=f.nowMs; return;
    }
    if(elapsed(f.nowMs,phaseStartedMs_,config_.climbSettleMs)) {
      result_.pose.layer=Layer::UpperEstimated;
      result_.pose.uncertaintyCm+=config_.climbAddedUncertaintyCm;
      result_.pose.headingUncertaintyDeg+=3; result_.climbEstimated=true;
      if(!poseSafe()) { halt(StopReason::PoseUncertain); return; }
      result_.mode=postClimbMode_;
      if(postClimbMode_==Mode::Navigate) {
        const Pose current=result_.pose;
        result_.pose.xCm=goalX_; result_.pose.yCm=goalY_;
        const bool safe=poseSafe(); result_.pose=current;
        if(!safe) { halt(StopReason::InvalidGoal); return; }
        setPhase(Phase::Travel,f.nowMs); result_.reason=StopReason::None;
      } else if(postClimbMode_==Mode::Battle) {
        result_.hasGoal=false; setPhase(Phase::Search,f.nowMs); result_.reason=StopReason::None;
      } else { setPhase(Phase::Complete,f.nowMs); result_.reason=StopReason::Complete; }
    }
  }
}
bool ArenaModel::guardDemand() {
  if(result_.forwardCmS==0) return true;
  if(result_.mode==Mode::Climb && result_.phase>=Phase::ClimbContact &&
      result_.phase<=Phase::ClimbSettle) return true; // 登台走独立入口与预算约束。
  Pose old=result_.pose;
  const float theta=old.headingDeg*Pi/180;
  result_.pose.xCm+=result_.forwardCmS*config_.predictionSeconds*std::cos(theta);
  result_.pose.yCm+=result_.forwardCmS*config_.predictionSeconds*std::sin(theta);
  const bool safe=poseSafe(); result_.pose=old;
  if(!safe) { halt(StopReason::OutsideSafeRegion); return false; }
  return true;
}
Decision ArenaModel::tick(const Frame& f) {
  result_.forwardCmS=result_.yawDegS=0;
  if(!finite(f.dtS) || f.dtS<=0 || f.dtS>0.25f || !finite(f.deltaCm) ||
      !finite(f.deltaYawDeg) || !finite(f.tiltDeg) || f.tiltDeg<0 || f.tiltDeg>180 ||
      f.irCount<1 || f.irCount>12 || std::fabs(f.deltaCm)>60 || std::fabs(f.deltaYawDeg)>90) {
    result_.pose.valid=false; halt(StopReason::InvalidFrame); return result_;
  }
  if(frameSeen_ && f.nowMs==lastFrameMs_) { return result_; } // 增量不得重复消费。
  const bool stale=frameSeen_ && elapsed(f.nowMs,lastFrameMs_,251);
  frameSeen_=true; lastFrameMs_=f.nowMs;
  integrate(f);
  if(result_.mode==Mode::Idle || result_.mode==Mode::Halted) return result_;
  if(!config_.armed) { halt(StopReason::NotArmed); return result_; }
  if(stale || !f.imuOk || !f.imuFresh || !f.accelOk || !f.digitalOk || !f.digitalFresh) {
    halt(StopReason::SensorUnavailable); return result_;
  }
  if(result_.pose.uncertaintyCm>config_.maxUncertaintyCm || result_.pose.headingUncertaintyDeg>30) {
    halt(StopReason::PoseUncertain); return result_;
  }
  if(f.tiltDeg>config_.maxTiltDeg) { halt(StopReason::ExcessiveTilt); return result_; }
  if(elapsed(f.nowMs,taskStartedMs_,config_.taskLimitMs)) { halt(StopReason::TaskTimeout); return result_; }
  const bool climbing=result_.mode==Mode::Climb && result_.phase>=Phase::ClimbContact;
  if(!poseSafe(climbing)) { halt(StopReason::OutsideSafeRegion); return result_; }
  if(edgeDemand(f)) { guardDemand(); return result_; }
  if(result_.mode==Mode::Navigate) {
    if(navigationDemand(goalX_,goalY_,result_.pose.layer==Layer::Lower)) {
      result_.mode=Mode::Idle; setPhase(Phase::Complete,f.nowMs); result_.reason=StopReason::Complete;
    }
  } else if(result_.mode==Mode::Battle) battle(f);
  else if(result_.mode==Mode::Climb) climb(f);
  guardDemand(); return result_;
}

const char* reasonName(StopReason r) {
  switch(r) {
    case StopReason::None:return "none"; case StopReason::Cancelled:return "cancelled";
    case StopReason::NotArmed:return "not_armed"; case StopReason::InvalidConfig:return "invalid_config";
    case StopReason::PoseRequired:return "pose_required"; case StopReason::InvalidGoal:return "invalid_goal";
    case StopReason::InvalidFrame:return "invalid_frame"; case StopReason::SensorUnavailable:return "sensor_unavailable";
    case StopReason::PoseUncertain:return "pose_uncertain"; case StopReason::OutsideSafeRegion:return "outside_safe_region";
    case StopReason::TaskTimeout:return "task_timeout"; case StopReason::EdgeBlocked:return "edge_blocked";
    case StopReason::EscapeTimeout:return "escape_timeout"; case StopReason::ClimbUnsupported:return "climb_unsupported";
    case StopReason::ClimbTimeout:return "climb_timeout"; case StopReason::ClimbDistanceLimit:return "climb_distance_limit";
    case StopReason::ClimbEvidenceInsufficient:return "climb_evidence_insufficient";
    case StopReason::ExcessiveTilt:return "excessive_tilt"; case StopReason::Complete:return "complete";
  }
  return "unknown";
}
const char* modeName(Mode m) {
  switch(m) { case Mode::Idle:return "idle"; case Mode::Navigate:return "navigate";
    case Mode::Battle:return "battle"; case Mode::Climb:return "climb";
    case Mode::Escape:return "escape"; case Mode::Halted:return "halted"; }
  return "unknown";
}
const char* phaseName(Phase p) {
  switch(p) { case Phase::None:return "none"; case Phase::Travel:return "travel";
    case Phase::Align:return "align"; case Phase::Search:return "search";
    case Phase::Approach:return "approach"; case Phase::Push:return "push";
    case Phase::Retreat:return "retreat"; case Phase::ClimbApproach:return "climb_approach";
    case Phase::ClimbAlign:return "climb_align"; case Phase::ClimbContact:return "climb_contact";
    case Phase::ClimbCrest:return "climb_crest"; case Phase::ClimbSettle:return "climb_settle";
    case Phase::Complete:return "complete"; }
  return "unknown";
}
const char* layerName(Layer l) {
  switch(l) { case Layer::Unknown:return "unknown"; case Layer::Lower:return "lower";
    case Layer::UpperAnchored:return "upper_anchored"; case Layer::UpperEstimated:return "upper_estimated";
    case Layer::TransitionUnknown:return "transition_unknown"; }
  return "unknown";
}
}} // namespace bot::arena
