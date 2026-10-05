/**
 ******************************************************************************
 * @file    safety.cpp
 * @brief   安全仲裁实现（心跳看门狗 / 急停 / 低压 / 堵转 / 边缘保护）
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
#include "safety.h"
#include "sensors.h"

/* ==========================================================================
 *                              对外函数实现
 * ========================================================================== */

/**
 * @brief   执行一次安全仲裁
 * @param[in]  c            当前配置
 * @param[in]  now          当前毫秒时间戳
 * @param[in]  lastHbMs     最近一次心跳时间戳
 * @param[in]  manualActive 是否处于连续实时模式
 * @param[in]  anyStall     是否有电机堵转
 * @param[out] out          仲裁结果
 * @return   无
 */
void safetyUpdate(const Cfg& c, uint32_t now, uint32_t lastHbMs,
                  bool manualActive, bool anyStall, SafetyOut& out) {
    /* ---- 每个周期先复位，保证结果是「本周期判定」而非累积 ---- */
    out.forceStop  = false;
    out.speedScale = 1.0f;
    out.lowBat     = false;
    out.critBat    = false;
    out.edge       = false;
    out.irLimit    = false;
    out.stall      = anyStall;
    out.hbLost     = false;

    /* ---- 1. 心跳看门狗 ----
     * 验收标准：断网 / 关页面后 ≤1s 内自动停车。
     * 只在连续模式生效，精准模式需要把动作执行完。 */
    if (manualActive && lastHbMs > 0 && (now - lastHbMs) > c.hbTimeout) {
        out.hbLost    = true;
        out.forceStop = true;
    }

    /* ---- 2. 电池电压 ----
     * 6S 锂电：满电 25.2V，亏电约 21V。
     * 低压只限速，严重低压才停机（避免比赛中因瞬时压降直接趴窝）。 */
    float bat = gSen.bat;
    if (bat > 1.0f) {                           /* >1V 才认为检测有效       */
        if (bat < c.batCrit) {
            out.critBat   = true;
            out.forceStop = true;
        } else if (bat < c.batLow) {
            out.lowBat     = true;
            out.speedScale *= 0.5f;
        }
    }

    /* ---- 3. 红外接近限速（可关） ---- */
    if (c.safetyIR && gSen.irNear) {
        out.irLimit     = true;
        out.speedScale *= 0.3f;
    }

    /* ---- 4. 边缘保护：E18 判定悬空 → 自动刹车（可关） ---- */
    if (c.safetyEdge && gSen.edgeDrop) {
        out.edge      = true;
        out.forceStop = true;
    }

    /* ---- 5. 堵转：PWM 高但 FG 无脉冲（判定逻辑在 motor.cpp） ---- */
    if (anyStall) out.forceStop = true;
}
