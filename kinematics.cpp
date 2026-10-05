/**
 ******************************************************************************
 * @file    kinematics.cpp
 * @brief   运动学换算实现（脉冲 ↔ 厘米 ↔ 角度）
 *
 * @details 所有换算都实时读取配置，因此网页里改了轮径 / K_odo / K_turn 立即生效，
 *          无需重启。单位约定：对外距离一律用 **cm**，角度一律用 **度**。
 *
 * @author  CombatBot 电控组
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

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include "kinematics.h"
#include "cfg.h"

/* ==========================================================================
 *                              对外函数实现
 * ========================================================================== */

/**
 * @brief   计算轮周长
 * @param   无
 * @return  轮周长 cm
 */
float kinCircumferenceCm() {
    CfgSnap cs;
    return 3.14159265f * cs.c.wheelDia / 10.0f;     /* D(mm) → C(cm) */
}

/**
 * @brief   获取「脉冲数 / 厘米」
 * @param   无
 * @return  脉冲数每厘米
 */
float kinPulsesPerCm() {
    CfgSnap cs;

    if (cs.c.kOdo > 0.05f) return cs.c.kOdo;        /* 优先使用标定值 */

    float ppr = (cs.c.ppr > 1.0f) ? cs.c.ppr : 90.0f;
    return ppr / (3.14159265f * cs.c.wheelDia / 10.0f);   /* 理论兜底 */
}

/**
 * @brief   距离 → 脉冲数
 * @param[in] cm  距离 cm
 * @return  脉冲数
 */
float kinCmToPulses(float cm)     { return cm * kinPulsesPerCm(); }

/**
 * @brief   脉冲数 → 距离
 * @param[in] pulses  脉冲数
 * @return  距离 cm
 */
float kinPulsesToCm(float pulses) { return pulses / kinPulsesPerCm(); }

/**
 * @brief   获取轮距（cm）
 * @param   无
 * @return  轮距 cm
 */
float kinTrackCm() { CfgSnap cs; return cs.c.track / 10.0f; }

/**
 * @brief   角度 → 单侧轮目标脉冲数（原地转向）
 * @param[in] deg  角度 °，正值 = 逆时针
 * @return  单侧轮脉冲数
 * @note    N = θ(rad) · T(cm) / 2 · K_odo · K_turn
 */
float kinDegToPulses(float deg) {
    CfgSnap cs;
    float rad = deg * 3.14159265f / 180.0f;
    return rad * (cs.c.track / 10.0f) * 0.5f * kinPulsesPerCm() * cs.c.kTurn;
}

/**
 * @brief   脉冲数 → 角度（原地转向）
 * @param[in] pulses  单侧轮脉冲数
 * @return  角度 °
 */
float kinPulsesToDeg(float pulses) {
    CfgSnap cs;
    float rad = pulses / (kinPulsesPerCm() * cs.c.kTurn) * 2.0f / (cs.c.track / 10.0f);
    return rad * 180.0f / 3.14159265f;
}

/**
 * @brief   给定转弯半径解算左右轮速度
 * @param[in]  radiusCm  转弯半径 cm
 * @param[in]  vBase     车体中心速度 cm/s
 * @param[out] vL        左轮速度 cm/s
 * @param[out] vR        右轮速度 cm/s
 * @return   无
 */
void kinArcSpeeds(float radiusCm, float vBase, float& vL, float& vR) {
    float t  = kinTrackCm();
    float rl = radiusCm - t * 0.5f;         /* 左轮轨迹半径 */
    float rr = radiusCm + t * 0.5f;         /* 右轮轨迹半径 */
    vL = vBase * rl / radiusCm;
    vR = vBase * rr / radiusCm;
}

/**
 * @brief   角速度 → 左右轮速差偏移
 * @param[in] omegaDegS  角速度 °/s，正值 = 逆时针
 * @return  速度偏移 cm/s
 */
float kinOmegaToDiff(float omegaDegS) {
    float rad = omegaDegS * 3.14159265f / 180.0f;
    return rad * kinTrackCm() * 0.5f;       /* ω·T/2 */
}
