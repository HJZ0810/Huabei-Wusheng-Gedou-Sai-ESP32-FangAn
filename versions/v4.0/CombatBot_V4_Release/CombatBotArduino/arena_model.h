/**
 * @file    arena_model.h
 * @brief   分层场地、估计定位与自主行为的纯 C++ 接口。
 *
 * 运行约定：世界坐标以场地中心为原点，X 向右、Y 向上，航向从 +X 逆时针计量。
 *           距离 cm，速度 cm/s，角度 °；地面输入 true 表示探头见到支撑面。
 * 并发约定：实例由唯一控制任务持有；网络侧不得直接操作实例或写入电机。
 * 设计边界：误差规模为启发式估计；UpperEstimated 是登台事件链的推断结果。
 *           本模块不承诺机械爬台能力、绝对定位精度或实际停车距离。
 */
#pragma once
#include <stdint.h>

namespace bot { namespace arena {
enum class Layer : uint8_t { Unknown, Lower, UpperAnchored, UpperEstimated, TransitionUnknown };
enum class Mode : uint8_t { Idle, Navigate, Battle, Climb, Escape, Halted };
enum class Phase : uint8_t { None, Travel, Align, Search, Approach, Push, Retreat,
  ClimbApproach, ClimbAlign, ClimbContact, ClimbCrest, ClimbSettle, Complete };
enum class StopReason : uint8_t { None, Cancelled, NotArmed, InvalidConfig, PoseRequired,
  InvalidGoal, InvalidFrame, SensorUnavailable, PoseUncertain, OutsideSafeRegion,
  TaskTimeout, EdgeBlocked, EscapeTimeout, ClimbUnsupported, ClimbTimeout,
  ClimbDistanceLimit, ClimbEvidenceInsufficient, ExcessiveTilt, Complete };
enum class EntrySide : uint8_t { East, North, West, South };

struct Config {
  bool armed=false, climbEnabled=false;
  float outerSizeCm=380, platformSizeCm=240;
  float bodyRadiusCm=18, safetyMarginCm=6, maxUncertaintyCm=35;
  float anchorUncertaintyCm=2, anchorHeadingUncertaintyDeg=2;
  float driftPerCm=0.012f, driftPerTurnDeg=0.015f, driftPerSecondCm=0.015f;
  float searchSpeedCmS=12, navigationSpeedCmS=12, attackSpeedCmS=18;
  float maxYawDegS=60, headingToleranceDeg=8, goalToleranceCm=4;
  uint32_t taskLimitMs=120000, targetHoldMs=350, pushLimitMs=1500;
  float targetMaxCm=120, targetMinCm=20, targetAgreementCm=30;
  float escapeSpeedCmS=8, escapeMaxCm=18;
  uint32_t escapeLimitMs=2000, groundClearMs=200;
  float sensorFrontCm=18, sensorRearCm=18, grayFrontCm=14.5f, grayRearCm=14.5f;
  float grayLeftCm=11, grayRightCm=11;
  EntrySide entrySide=EntrySide::East;
  float entryOffsetCm=0, entryToleranceCm=12;
  float climbSpeedCmS=20, climbMaxCm=80;
  float climbRiseDeg=12, climbLevelDeg=7, maxTiltDeg=45;
  uint32_t climbLimitMs=5000, climbSettleMs=450;
  float climbAddedUncertaintyCm=8;
  float predictionSeconds=0.35f;
};

struct Pose {
  float xCm=0, yCm=0, headingDeg=0;
  float uncertaintyCm=0, headingUncertaintyDeg=0;
  uint32_t updatedMs=0, anchoredMs=0;
  Layer layer=Layer::Unknown;
  bool valid=false;
};
struct Frame {
  uint32_t nowMs=0;
  float dtS=0.01f, deltaCm=0, deltaYawDeg=0;
  bool imuOk=false, imuFresh=false, accelOk=false, accelSaturated=false, impact=false;
  float tiltDeg=0;
  bool digitalOk=false, digitalFresh=false;
  bool groundPresent[3]={false,false,false};
  bool rawGroundPresent[3]={false,false,false}; ///< 快路径候选：false 即缺地，不等待去抖。
  bool grayBright[4]={false,false,false,false}; ///< 左前、右前、左后、右后；仅为亮色候选。
  bool irValid[12]={false};
  float irCm[12]={0};
  uint32_t irSampledMs[12]={0}; ///< 各路实际采样时刻；控制周期不得为缓存测量续期。
  uint8_t irCount=6;
};
struct Target {
  bool valid=false;
  float bearingDeg=0, distanceCm=0;
  uint32_t observedMs=0;
};
struct Decision {
  float forwardCmS=0, yawDegS=0;
  Mode mode=Mode::Idle;
  Phase phase=Phase::None;
  StopReason reason=StopReason::None;
  Pose pose;
  Target target;
  bool climbEstimated=false;
  bool hasGoal=false;
  float goalX=0, goalY=0; ///< 世界目标坐标 cm；候选敌人不是固定世界目标。
};

/** @brief 校验配置；不能代替硬件标定。 */
bool configValid(const Config&);
/** @brief 将角度归一化至 [-180,180)，仅用于方向比较和显示。 */
float wrapDegrees(float);
/** @brief 返回稳定协议字符串，便于网页和日志解释状态。 */
const char* reasonName(StopReason);
const char* modeName(Mode);
const char* phaseName(Phase);
const char* layerName(Layer);

class ArenaModel {
 public:
  explicit ArenaModel(const Config& config=Config());
  /** @brief 更换配置并取消行为；已锚定位姿保留但下周期重新检查误差和边界。 */
  bool configure(const Config&);
  /** @brief 停止状态下建立人工位置参考；不得在运动期间静默改坐标。 */
  bool setPoseAnchor(float xCm,float yCm,float headingDeg,Layer,uint32_t nowMs);
  bool startNavigate(float xCm,float yCm,uint32_t nowMs);
  bool startBattle(uint32_t nowMs);
  bool startClimb(uint32_t nowMs);
  void cancel(StopReason reason=StopReason::Cancelled);
  /** @brief 消费一次测量增量；返回速度需求，不操作电机。 */
  Decision tick(const Frame&);
  Decision snapshot() const { return result_; }
 private:
  Config config_;
  Decision result_;
  Mode resumeMode_=Mode::Idle;
  Mode postClimbMode_=Mode::Idle;
  Phase resumePhase_=Phase::None;
  float goalX_=0, goalY_=0, travelCm_=0, escapeStartTravel_=0;
  float climbStartTravel_=0, climbEntryX_=0, climbEntryY_=0, climbHeading_=0;
  float climbRiseTravel_=0;
  uint32_t taskStartedMs_=0, phaseStartedMs_=0, clearStartedMs_=0;
  uint32_t climbStartedMs_=0, lastFrameMs_=0;
  bool frameSeen_=false, clearTiming_=false, climbRiseSeen_=false;
  bool disturbanceSeen_=false;
  unsigned consistentTargetFrames_=0;
  uint32_t consumedIrMs_[12]={0};
  bool irSeen_[12]={false};
  uint8_t candidateChannel_=0;
  Target candidate_;
  bool start(Mode,uint32_t);
  void halt(StopReason);
  void integrate(const Frame&);
  bool poseSafe(bool transitionAllowed=false) const;
  bool navigationDemand(float x,float y,bool lowerRoute);
  void battle(const Frame&);
  void climb(const Frame&);
  bool edgeDemand(const Frame&);
  bool guardDemand();
  void setPhase(Phase,uint32_t);
};
}} // namespace bot::arena
