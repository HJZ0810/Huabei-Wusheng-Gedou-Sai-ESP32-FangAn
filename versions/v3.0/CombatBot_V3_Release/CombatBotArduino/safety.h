/**
 * @file    safety.h
 * @brief   边缘、倾倒、方向障碍与时间超限判定。
 *
 * ============================================================================
 * 运行约定
 *   数字输入已经完成有效电平映射和去抖；这里的 true 表示危险条件有效。
 *   红外按配置决定无效缓存的停车策略，加速度判据要求传感器安装 Z 轴朝上。
 *
 * 设计边界
 *   这些函数提供软件判据；盲区、无效测距、安装误差与动态冲击仍需装机验证。
 *   停车响应还取决于任务调度与电机硬件，判据本身不保证机械安全距离。
 * ============================================================================
 */
#pragma once
#include "types.h"
#include "kinematics.h"
#include "pid.h"
#include <cmath>
namespace bot {
// ============================================================================
// 数字边缘与加速度倾倒判定
// ============================================================================
/** @brief 任一灰度或 E18 输入有效时，报告边缘危险。 */
inline bool sensorEdge(const Sensors& s) {
  for(bool v:s.gray) if(v) return true;
  for(bool v:s.e18) if(v) return true;
  return false;
}
/**
 * @brief   使用归一化 Z 分量检测明显倾倒。
 * @details Z / |a| < 0.5 对应静态倾角超过约 60°；模长低于 0.3 g 时不判定。
 * @note 无有效加速度时返回 false。碰撞和机体加减速会改变加速度方向，
 *       此结果不是经过姿态滤波的精确倾角测量。
 */
inline bool sensorTilt(const Sensors& s) {
  if(!s.accelOk) return false;
  const float magnitude=std::sqrt(s.acc[0]*s.acc[0]+s.acc[1]*s.acc[1]+s.acc[2]*s.acc[2]);
  return magnitude>0.3f && s.acc[2]/magnitude<0.5f;
}
// ============================================================================
// 按运动方向筛选红外障碍
// ============================================================================
/**
 * @brief   检查计划运动方向上是否存在有效的近距离障碍。
 * @details 通道从正前方开始顺时针等角排列；6 路依次为前、右前、右后、
 *          后、左后、左前，12 路则每路间隔 30°。
 *          平移按前进或后退半平面筛选；转向时任一方向近障碍均会拒绝。
 * @note translation 用符号表示平移方向，rotation 的非零幅值表示旋转需求。
 */
struct DirectionSafety {
  float speedScale=1; ///< 有效距离决定的速度比例；无效通道绝不代表安全距离。
  bool unavailable=false; ///< 所需方向至少有一条无效测距，供网页告警。
};
/**
 * @brief 按运动方向计算红外渐进限速；归零距离以内直接撤驱动。
 * @details irThresholdCm 为归零距离，irSlowdownCm 为开始限速距离。
 *          旋转检查全周；平移仅检查迎向运动的半平面。严格策略下相关无效
 *          通道将速度比例置零；忽略策略仅使用有效通道，同时保留无效告警。
 * @note 超量程、过期和电压异常均属于无效，不能解释成道路畅通。
 */
inline DirectionSafety directionSafety(const Sensors& s,const Config& c,float translation,float rotation) {
  DirectionSafety out;
  for(int i=0;i<c.irCount && i<12;++i) {
    const float bearing=2*pi*i/c.irCount;
    // 旋转不能沿用驾驶死区：很小的目标也可能经死区补偿产生实际驱动力。
    if(std::fabs(rotation)<=0.0001f && translation*std::cos(bearing)<=0.0001f) continue;
    if(!s.irValid[i] || !std::isfinite(s.ir[i])) {
      out.unavailable=true;
      if(c.irFailSafeStop) out.speedScale=0;
      continue;
    }
    const float span=c.irSlowdownCm-c.irThresholdCm;
    const float scale=span>0?clampf((s.ir[i]-c.irThresholdCm)/span,0,1):0;
    out.speedScale=std::min(out.speedScale,scale);
  }
  return out;
}
/** @brief 保留原有归零障碍查询接口；含严格模式无效测距。 */
inline bool obstacleInDirection(const Sensors& s,const Config& c,float translation,float rotation) {
  return directionSafety(s,c,translation,rotation).speedScale<=0;
}
// ============================================================================
// millis() 时间差判定
// ============================================================================
/**
 * @brief   判断经过时间是否严格超过 duration，单位 ms。
 * @note 异步更新的时间戳可能比本周期起点晚几毫秒，这时不能视为超时。
 *       以半个计数周期区分过去/近期未来；门限和检查间隔须小于2^31ms。
 */
inline bool timedOut(uint32_t now,uint32_t then,uint32_t duration) {
  const uint32_t elapsed=uint32_t(now-then);
  return elapsed<0x80000000u && elapsed>duration;
}
}
