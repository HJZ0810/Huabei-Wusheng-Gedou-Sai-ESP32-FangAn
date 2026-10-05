/**
 * @file    kinematics.h
 * @brief   四轮差速底盘的脉冲、位移与偏航换算。
 *
 * ============================================================================
 * 运行约定
 *   轮序为左前、右前、左后、右后；前进为正，逆时针偏航为正。
 *   配置尺寸以 mm 存储，控制计算中的线位移以 cm 表示。
 *
 * 设计边界
 *   转向按有效轮距 trackMm × turnFactor 建模；侧滑、载荷与地面摩擦会改变结果。
 *   融合没有绝对航向参考，轮脉冲与陀螺仪误差仍会形成长期漂移。
 * ============================================================================
 */
#pragma once
#include "types.h"
#include <cmath>
namespace bot {
// ============================================================================
// 几何比例与四轮行程
// ============================================================================
constexpr float pi=3.14159265358979323846f;
/**
 * @brief   返回每 cm 对应的有效 FG 上升沿计数。
 * @details 优先使用正的标定值，否则按 PPR / 轮周长推导；轮径由 mm 换成 cm。
 * @pre 轮径与 PPR 已通过配置校验，且计数口径与标定时一致。
 */
inline float pulsesPerCm(const Config& c) { return c.pulsesPerCm>0?c.pulsesPerCm:c.ppr/(c.wheelMm*pi/10); }
/**
 * @brief   将转角换成单侧车轮应走的弧长，单位 cm。
 * @details s = θ × 有效轮距 / 2；正转角对应左侧后退、右侧前进。
 *          线性比例同样适用于角速度 °/s 到单侧轮速 cm/s 的换算。
 */
inline float wheelTurnDistance(float degrees,const Config& c) { return degrees*pi/180*(c.trackMm/10)*c.turnFactor/2; }
/** @brief 用右侧减左侧的行程差估算偏航增量；输入 cm，输出 °。 */
inline float odoYawDegrees(float leftCm,float rightCm,const Config& c) { return (rightCm-leftCm)/(c.trackMm/10*c.turnFactor)*180/pi; }
/** @brief 对四轮带符号量求均值；原地差速转向时左右理想值相互抵消。 */
inline float wheelMean(const float wheel[4]) { return (wheel[0]+wheel[1]+wheel[2]+wheel[3])/4; }
/**
 * @brief   计算四轮带符号脉冲增量均值，并换算为 cm。
 * @note 单相 FG 的方向由电机已施加方向推断，外力推动或余转会引入估计误差。
 */
inline float pulseTravel(const int64_t now[4], const int64_t start[4], const Config& c) {
  double sum=0; for(int i=0;i<4;++i) sum+=double(now[i]-start[i]); return float(sum/4/pulsesPerCm(c));
}
/**
 * @brief   返回四轮脉冲增量绝对值的均值，单位为计数。
 * @note 该量用于标定，即使左右车轮方向相反也不会相互抵消；尚未换算为 cm。
 */
inline float absolutePulseTravel(const int64_t now[4],const int64_t start[4]) {
  double sum=0; for(int i=0;i<4;++i) sum+=std::fabs(double(now[i]-start[i])); return float(sum/4);
}
// ============================================================================
// 偏航增量融合
// ============================================================================
/**
 * @brief   对陀螺仪增量与轮里程偏航增量加权，返回本周期偏航增量 °。
 * @param gyroDps 轴映射后的角速度，单位 °/s；调用方已扣自适应补偿。
 * @param dt 采样积分时间，单位 s。
 * @param wheelDeltaYaw 轮里程估算的本周期偏航增量，单位 °。
 * @param c 配置，其中 gyroBias 是持久化零偏，fusionAlpha 是陀螺仪权重。
 * @details 陀螺增量 = (gyroDps - gyroBias) × dt；本函数只扣持久化零偏一次。
 *          调用方连续累加返回值，不在 ±180° 处回绕，以支持带符号多圈转向。
 */
inline float fusedYawIncrement(float gyroDps,float dt,float wheelDeltaYaw,const Config& c) {
  return c.fusionAlpha*(gyroDps-c.gyroBias)*dt+(1-c.fusionAlpha)*wheelDeltaYaw;
}
}
