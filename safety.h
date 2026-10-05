/**
 ******************************************************************************
 * @file    safety.h
 * @brief   安全仲裁接口（心跳看门狗 / 急停 / 低压 / 堵转 / 边缘保护）
 *
 * @details 设计原则（**至关重要**）：
 *          所有安全判定都在 **控制核 200Hz 循环内** 执行，一旦触发立即把 PWM 清零，
 *          **不依赖网络侧的任何动作** —— 断网、手机没电、浏览器崩溃都必须能停车。
 *
 *          判定优先级（越靠前越优先）：
 *            1. 急停锁定        —— 人工按下，最高优先级；
 *            2. 心跳超时        —— 连续模式下超过 hbTimeout 未收到心跳；
 *            3. 严重低压        —— 低于 batCrit 直接停机；
 *            4. 堵转            —— PWM 高但持续无脉冲；
 *            5. 边缘（悬空）    —— E18 判定悬空则刹车；
 *            6. 运动方向障碍    —— 只统计"朝着行进方向"的那几路红外；
 *            7. 一般低压        —— 不限停，只做限速。
 *
 * @version V2.0.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>统一企业级注释规范
 *          <tr><td>2026-10-05 <td>V2.0  <td>电控组 <td>融合版：红外限速改为
 *                                                    按行进方向过滤
 *          </table>
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

#ifndef __SAFETY_H
#define __SAFETY_H

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>
#include "cfg.h"

/**
 * @brief   安全仲裁输出（单次判定的结果快照）
 */
struct SafetyOut {
    bool  forceStop  = false;   /**< 立即停车（急停/失联/严重低压/堵转/边缘） */
    float speedScale = 1.0f;    /**< 限速系数（低压 ×0.5，红外接近 ×0.3）     */
    bool  lowBat     = false;   /**< 低压告警（触发限速）                     */
    bool  critBat    = false;   /**< 严重低压（触发停机）                     */
    bool  edge       = false;   /**< 边缘（悬空）触发                         */
    bool  irLimit    = false;   /**< 红外接近限速生效                         */
    bool  stall      = false;   /**< 存在堵转电机                             */
    bool  hbLost     = false;   /**< 心跳超时                                 */
};

/**
 * @brief   判断「行进方向」上是否存在足够近的障碍
 * @param[in] moveDirDeg  行进方向（车体坐标，0=车头，正=逆时针），单位 °
 * @param[in] turning     是否正在原地转向（转向时整车都在扫掠，必须全向看）
 * @param[in] alarmCm     判定距离阈值，单位 cm
 * @return  true 行进方向上有障碍
 *
 * @details ★ 融合版新增，解决的是一个很具体的"假刹车"问题：
 *          旧实现只要**任意一路**红外小于阈值就全局限速到 0.3。
 *          于是车屁股后面贴着墙时往前开也会被限速 —— 明明前面空得很，
 *          却怎么也跑不起来，赛场上表现为"车突然变肉了"。
 *
 *          现在只统计安装方向与行进方向夹角在 ±60° 以内的那几路：
 *            - 前进：看「前 / 左前 / 右前」；
 *            - 后退：看「后 / 左后 / 右后」；
 *            - 原地转向：车体整体扫掠，退化为全向检测。
 *
 * @note    有效性标志在这里是硬性前提：掉线的通道永远不参与判定，
 *          否则"传感器坏了"会被当成"障碍贴脸"，把车锁死在限速状态。
 */
bool obstacleInDirection(float moveDirDeg, bool turning, float alarmCm);

/**
 * @brief   执行一次安全仲裁
 * @param[in]  c            当前配置（调用方传入快照，避免本函数再次加锁）
 * @param[in]  now          当前毫秒时间戳
 * @param[in]  lastHbMs     最近一次收到心跳的时间戳（0 表示从未收到）
 * @param[in]  manualActive 当前是否处于「连续实时模式」
 * @param[in]  anyStall     是否有任一电机处于堵转
 * @param[in]  moveDirDeg   当前行进方向（车体坐标 °，0=车头）
 * @param[in]  turning      是否正在原地转向
 * @param[out] out          仲裁结果
 * @return   无
 * @note     心跳看门狗只在连续模式下生效：精准模式是「一次性闭环动作」，
 *           即使心跳断了也应把动作走完，否则会造成半途停车、位置丢失。
 */
void safetyUpdate(const Cfg& c, uint32_t now, uint32_t lastHbMs,
                  bool manualActive, bool anyStall,
                  float moveDirDeg, bool turning, SafetyOut& out);

#endif /* __SAFETY_H */
