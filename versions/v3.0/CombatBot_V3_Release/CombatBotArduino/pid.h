/**
 * @file    pid.h
 * @brief   离散 PID、速度斜坡与理想停车速度约束。
 *
 * ============================================================================
 * 模块职责
 *   提供运动控制使用的轻量数值工具，不直接访问硬件或共享状态。
 *
 * 设计边界
 *   PID 状态由所属控制任务持有；输入与输出单位由调用场景决定。
 *   速度规划只约束控制目标，实际制动距离还受电机、负载与地面条件影响。
 * ============================================================================
 */
#pragma once
#include "types.h"
#include <cmath>
#include <algorithm>

namespace bot {
// ============================================================================
// 数值限幅与离散 PID
// ============================================================================
inline float clampf(float x, float lo, float hi) { return std::max(lo, std::min(x, hi)); }
/**
 * @brief   带条件积分的离散 PID，避免饱和时继续积累同向积分。
 * @details 每个对象对应一个回路。速度回路：误差为 cm/s，输出为 PWM 计数；
 *          位置回路：误差为 cm，输出为 cm/s；航向回路：误差为 °，输出为 °/s。
 *          增益的量纲随回路变化，不能将不同回路的参数直接互换。
 */
class Pid {
  float integral_=0, previous_=0; ///< 累计误差积分与上一次误差；单位随所属回路变化。
  bool initialized_=false; ///< 首次计算不做误差差分，避免初始化引入微分突变。
public:
  /** @brief 清空回路历史；动作切换或零速输出时避免沿用旧积分。 */
  void reset() { integral_=previous_=0; initialized_=false; }
  /**
   * @brief 计算一次 PID 输出，并限制在给定输出区间内。
   * @param error 本周期目标值减测量值。
   * @param dt 本周期积分时间，单位 s；须为有限正值。
   * @param g 比例、积分和微分增益。
   * @param lo 输出下界。
   * @param hi 输出上界。
   * @pre lo 不大于 hi，增益与输出边界为有限值。
   * @return 限幅后的控制量；非正 dt 或非有限误差返回 0。
   */
  float step(float error, float dt, const Gains& g, float lo, float hi) {
    if (!(dt>0) || !std::isfinite(error)) return 0;
    const float derivative=initialized_?(error-previous_)/dt:0;
    const float proposed=integral_+error*dt;
    const float raw=g.kp*error+g.ki*proposed+g.kd*derivative;
    // 仅在输出未饱和，或当前误差有助于退出饱和时更新积分。
    // 用误差方向判断是否解饱和，避免输出已受限却继续积累同向误差。
    if ((raw>=lo && raw<=hi) || (raw>hi && error<0) || (raw<lo && error>0)) integral_=proposed;
    previous_=error; initialized_=true;
    return clampf(g.kp*error+g.ki*integral_+g.kd*derivative,lo,hi);
  }
};
/**
 * @brief 合成轮速前馈与 PID，并让积分器感知补偿后的真实 PWM 上限。
 * @param demand 有符号目标轮速，cm/s；measured 使用相同逻辑方向约定。
 * @param trim 当前轮所属侧的输出比例；由配置校验保证为有限正值。
 * @details correction 的可用区间由前馈及 trim 反推，不再固定为 ±1023。
 *          死区仅提供最小驱动力，无法消除低速区的机械非线性与极限环。
 * @note 本函数计算请求值。若驱动层因换向等待拒绝请求，调用方须清除积分。
 */
inline float wheelSpeedPwm(Pid& loop,float demand,float measured,float dt,const Config& c,float trim) {
  if(!std::isfinite(demand) || !std::isfinite(measured) || !(dt>0) || !std::isfinite(dt) ||
     !(trim>0) || !std::isfinite(trim) || !(c.maxSpeed>0)) { loop.reset(); return 0; }
  if(std::fabs(demand)<0.05f) { loop.reset(); return 0; }
  const float sign=demand>0?1:-1;
  const float feed=1023*std::fabs(demand)/c.maxSpeed;
  const float ceiling=std::min(1023.0f,1023.0f/trim);
  const float correction=loop.step(std::fabs(demand)-sign*measured,dt,c.speedPid,-feed,ceiling-feed);
  float output=clampf(feed+correction,0,ceiling);
  if(output>0) output=std::max(output,std::min(c.pwmDeadzone,ceiling));
  return sign*clampf(output*trim,0,1023);
}
// ============================================================================
// 速度目标规划：换向斜坡与停车包络
// ============================================================================
/**
 * @brief   按加减速度限制改变速度目标；换向时先减至零。
 * @param current 当前目标速度，通常为 cm/s。
 * @param desired 待追踪的目标速度，与 current 使用相同单位。
 * @param accel 加速上限，通常为 cm/s²。
 * @param decel 减速上限，通常为 cm/s²。
 * @param dt 本周期时间，单位 s。
 * @pre 输入为有限值，dt 为正值。
 * @return 本周期允许到达的速度目标。
 */
inline float rampVelocity(float current, float desired, float accel, float decel, float dt) {
  // 反向目标先替换为零：本周期按减速度撤去旧方向速度，
  // 后续调用在到达零速后才允许向新方向加速。
  if (current*desired<0) desired=0;
  const bool accelerating=std::fabs(desired)>std::fabs(current);
  const float step=std::max(0.0f,accelerating?accel:decel)*dt;
  return current+clampf(desired-current,-step,step);
}
/**
 * @brief   根据剩余行程生成带方向的理想停车速度上限。
 * @details 使用 v = sqrt(2 a |s|)，并受 maxSpeed 限制。平移可用 cm、cm/s²、
 *          cm/s；转向也可用 °、°/s²、°/s，但一次调用内必须保持单位一致。
 * @note 该公式限制目标速度，不是对实际停车距离或到位精度的保证。
 */
inline float stoppingVelocity(float distance, float deceleration, float maxSpeed) {
  const float speed=std::min(std::max(0.0f,maxSpeed),std::sqrt(2*std::max(0.0f,deceleration)*std::fabs(distance)));
  return distance<0?-speed:speed;
}
}
