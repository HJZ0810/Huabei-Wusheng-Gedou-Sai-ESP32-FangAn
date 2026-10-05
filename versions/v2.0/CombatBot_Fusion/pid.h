/**
 ******************************************************************************
 * @file    pid.h
 * @brief   通用 PID 控制器与梯形速度规划器（纯算法，无硬件依赖）
 *
 * @details 两个可复用组件：
 *            - @ref PID       —— 位置式 PID，带输出限幅与积分抗饱和；
 *            - @ref Trapezoid —— 梯形速度规划，把「剩余行程」映射为「当前允许速度」。
 *
 *          梯形规划原理（禁止满速启停，否则驼峰轮必然打滑）：
 *          @verbatim
 *              速度 ▲
 *            vmax ├──────────┐
 *                 │        ↗     ↘
 *                 │     ↗          ↘
 *                0└────────────────────► 时间
 *                   加速段   匀速段  减速段
 *          @endverbatim
 *          减速点由剩余距离反推：@c s_brake = v² / (2·a_dec)，
 *          当「剩余距离 ≤ s_brake」时切入减速段。
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

#ifndef __PID_H
#define __PID_H

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>
#include "cfg.h"        /**< 依赖 cfgFinite() 做输入有限性判定              */

/**
 * @brief   位置式 PID 控制器
 * @note    输出 = Kp·e + Ki·∫e·dt + Kd·de/dt，并做输出限幅与积分限幅。
 *          典型用法（速度环）：
 *          @code
 *              PID pid;
 *              pid.set(0.03f, 0.35f, 0.0f);
 *              pid.setLimit(-1.0f, 1.0f, -0.6f, 0.6f);
 *              float u = pid.update(target - measure, dt);
 *          @endcode
 */
class PID {
public:
    float kp = 0;       /**< 比例系数                                        */
    float ki = 0;       /**< 积分系数                                        */
    float kd = 0;       /**< 微分系数                                        */
    float outMin = -1.0f;   /**< 输出下限                                    */
    float outMax =  1.0f;   /**< 输出上限                                    */
    float iMin   = -1.0f;   /**< 积分项下限（抗饱和）                        */
    float iMax   =  1.0f;   /**< 积分项上限（抗饱和）                        */

    /**
     * @brief   设置 PID 参数
     * @param[in] p  比例系数
     * @param[in] i  积分系数
     * @param[in] d  微分系数
     * @return   无
     */
    void set(float p, float i, float d) { kp = p; ki = i; kd = d; }

    /**
     * @brief   设置输出与积分限幅
     * @param[in] omin  输出下限
     * @param[in] omax  输出上限
     * @param[in] imin  积分项下限
     * @param[in] imax  积分项上限
     * @return   无
     */
    void setLimit(float omin, float omax, float imin, float imax) {
        outMin = omin; outMax = omax; iMin = imin; iMax = imax;
    }

    /**
     * @brief   复位控制器历史状态（积分项、上一次误差与首拍标志）
     * @param   无
     * @return  无
     * @note    切换控制模式、或目标发生跳变时必须调用，否则积分项会造成巨大超调。
     */
    void reset() { iTerm = 0; prev = 0; primed = false; }

    /**
     * @brief   执行一次 PID 运算（带条件积分抗饱和）
     * @param[in] err  当前误差（目标 − 反馈）
     * @param[in] dt   采样间隔，单位 s
     * @return  限幅后的控制输出
     *
     * @details 融合版相比初版的两处强化：
     *          ① **首拍微分保护**：reset() 后的第一拍不做误差差分，
     *             否则 (err − 0)/dt 会产生一个巨大的微分冲击（derivative kick），
     *             表现为每次启动动作时电机"抽搐"一下。
     *          ② **条件积分（conditional integration）**：
     *             初版只把积分项硬截断在 [iMin, iMax]，输出一旦饱和，
     *             积分仍会在原地继续累积，等误差反向时要先"泄掉"这段积分才响应，
     *             这就是经典的积分饱和（windup）滞后。
     *             这里改为：仅当「输出未饱和」或「误差方向有助于退出饱和」时才接受新积分。
     *
     * @note    NaN / Inf 输入一律返回 0，防止污染扩散到 PWM 输出。
     */
    float update(float err, float dt) {
        if (!(dt > 0)) return 0;                    /* 同时拦截 dt=0 与 dt=NaN */
        if (!cfgFinite(err)) return 0;               /* 坏输入 = 零输出          */

        const float dTerm = primed ? (err - prev) / dt : 0.0f;   /* ① 首拍不差分 */
        const float proposed = iTerm + ki * err * dt;            /* 候选积分项   */
        const float raw = kp * err + proposed + kd * dTerm;      /* 未限幅输出   */

        /* ② 条件积分：未饱和 或 误差正在把输出拉回区间内 → 接受新积分 */
        const bool unsaturated = (raw >= outMin && raw <= outMax);
        const bool unwinding   = (raw > outMax && err < 0) || (raw < outMin && err > 0);
        if (unsaturated || unwinding) {
            iTerm = constrain(proposed, iMin, iMax);
        }

        prev   = err;
        primed = true;

        return constrain(kp * err + iTerm + kd * dTerm, outMin, outMax);
    }

private:
    float iTerm  = 0;       /**< 积分累积值                                  */
    float prev   = 0;       /**< 上一次误差（用于微分）                      */
    bool  primed = false;   /**< 是否已完成首拍（用于抑制微分冲击）          */
};

/**
 * @brief   梯形速度规划器
 * @details 由「剩余行程」反推当前允许速度，保证起步与到位都是平滑的：
 *          @code
 *              v_limit = sqrt(2 · a_dec · |remain|)   // 减速点反推
 *              target  = min(v_max, v_limit)
 *              v      += 限速到 ±acc·dt               // 加速度上限
 *          @endcode
 */
class Trapezoid {
public:
    float v = 0;        /**< 当前规划速度（带符号语义由调用方决定）          */

    /**
     * @brief   复位规划器
     * @param   无
     * @return  无
     */
    void reset() { v = 0; }

    /**
     * @brief   推进一个控制周期
     * @param[in] remainAbs  剩余行程的绝对值（脉冲 / 厘米 / 度，单位自洽即可）
     * @param[in] vmax       允许的最大速度
     * @param[in] acc        加速度上限
     * @param[in] dec        减速度上限
     * @param[in] dt         控制周期，单位 s
     * @return   本周期规划出的速度大小（0 ~ vmax）
     * @note     返回的是「速度大小」，方向由调用方按剩余量符号自行叠加。
     */
    float update(float remainAbs, float vmax, float acc, float dec, float dt) {
        if (dt <= 0) return v;

        /* 由剩余距离反推当前允许的最高速度（减速段约束） */
        float vLim  = sqrtf(2.0f * fmaxf(dec, 1e-3f) * fmaxf(remainAbs, 0.0f));
        float target = fminf(vmax, vLim);

        /* 加速度/减速度上限约束（起步不猛冲、减速不急停） */
        if (target > v) v = fminf(target, v + acc * dt);
        else            v = fmaxf(target, v - dec * dt);

        return v;
    }
};

#endif /* __PID_H */
