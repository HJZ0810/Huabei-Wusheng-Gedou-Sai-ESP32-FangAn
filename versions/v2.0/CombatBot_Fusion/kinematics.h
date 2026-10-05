/**
 ******************************************************************************
 * @file    kinematics.h
 * @brief   运动学换算接口（脉冲 ↔ 厘米 ↔ 角度）
 *
 * @details 全部公式均以「可标定参数」表达，不依赖 FG 每转脉冲数的理论值：
 *          @verbatim
 *              轮周长     C   = π · D                        （D=72mm → 22.619cm）
 *              直线目标   N   = s(cm) × K_odo                （K_odo = 脉冲/厘米）
 *              原地转向   单侧行程 s = θ(rad) · T / 2
 *                         N   = θ · T · K_odo · K_turn / 2
 *              给定半径R  v_L : v_R = (R − T/2) : (R + T/2)
 *          @endverbatim
 *          其中 @c K_odo 由行程标定实测得到，@c K_turn 由转向标定解算，
 *          这样即使 FG 脉冲数批次有差异、驼峰轮打滑严重，精度也能保证。
 *
 * @author  HJZ
 * @version V1.1.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>统一企业级注释规范
 *          </table>
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

#ifndef __KINEMATICS_H
#define __KINEMATICS_H

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>

/**
 * @brief   计算轮周长
 * @param   无
 * @return  轮周长，单位 cm（D=72mm 时约为 22.619）
 */
float kinCircumferenceCm();

/**
 * @brief   获取当前「脉冲数 / 厘米」换算系数
 * @param   无
 * @return  脉冲数每厘米
 * @note    优先使用标定值 @ref Cfg::kOdo；若尚未标定则退回理论值 PPR / C。
 */
float kinPulsesPerCm();

/**
 * @brief   距离 → 目标脉冲数
 * @param[in] cm  目标距离，单位 cm（负值表示后退）
 * @return  目标脉冲数（带符号）
 */
float kinCmToPulses(float cm);

/**
 * @brief   脉冲数 → 距离
 * @param[in] pulses  已走过的脉冲数（带符号）
 * @return  实际距离，单位 cm
 */
float kinPulsesToCm(float pulses);

/**
 * @brief   角度 → 单侧轮目标脉冲数（原地坦克转向）
 * @param[in] deg  目标角度，单位 °，正值 = 逆时针（左转）
 * @return  单侧轮目标脉冲数（带符号）
 * @note    已内含 @ref Cfg::kTurn 打滑修正：打滑越多系数越大、走得越远。
 */
float kinDegToPulses(float deg);

/**
 * @brief   脉冲数 → 角度（原地转向）
 * @param[in] pulses  单侧轮脉冲数
 * @return  对应角度，单位 °
 */
float kinPulsesToDeg(float pulses);

/**
 * @brief   获取轮距（换算为 cm）
 * @param   无
 * @return  轮距，单位 cm（T=170mm → 17.0）
 */
float kinTrackCm();

/**
 * @brief   给定转弯半径，解算左右轮速度
 * @param[in]  radiusCm  转弯半径，单位 cm（以车体中心计）
 * @param[in]  vBase     车体中心速度，单位 cm/s
 * @param[out] vL        左轮速度，单位 cm/s
 * @param[out] vR        右轮速度，单位 cm/s
 * @return   无
 * @note     v_L : v_R = (R − T/2) : (R + T/2)；半径为 0 时退化为原地转向。
 */
void kinArcSpeeds(float radiusCm, float vBase, float& vL, float& vR);

/**
 * @brief   角速度 → 左右轮速差（差速混合用）
 * @param[in] omegaDegS  车体角速度，单位 °/s，正值 = 逆时针（左转）
 * @return  单侧轮需要叠加的速度偏移量，单位 cm/s
 */
float kinOmegaToDiff(float omegaDegS);

#endif /* __KINEMATICS_H */
