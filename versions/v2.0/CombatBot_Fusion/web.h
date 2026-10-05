/**
 ******************************************************************************
 * @file    web.h
 * @brief   HTTP / WebSocket / REST 三通道服务接口
 *
 * @details 本模块是「网络核（Core 1）」对外的唯一门面，提供三条互不干扰的通道：
 *
 *          @verbatim
 *              ┌────────────── 页面通道  GET  /            ──────────────┐
 *              │  返回内嵌在 PROGMEM 的单文件网页（无外网 CDN 依赖）      │
 *              ├────────────── 实时通道  WS   /ws          ──────────────┤
 *              │  上行：drv / hb / stop / move / turn / servo /          │
 *              │        estop / unlock / cal / ping                      │
 *              │  下行：tm（遥测）/ done（动作）/ cal（标定）/ alert（告警）│
 *              ├────────────── 配置通道  REST /api/…       ──────────────┤
 *              │  config 读写 · action 动作 · calibrate 标定 ·           │
 *              │  status 状态 · reboot 重启                              │
 *              └─────────────────────────────────────────────────────────┘
 *          @endverbatim
 *
 *          线程边界（务必遵守）：
 *            - 本模块所有回调都运行在 Core 1（Arduino loopTask / 异步网络任务）；
 *            - 严禁在回调里直接调用电机、传感器、IMU 等硬件接口；
 *            - 一切动作统一封装为 @ref Cmd 后通过 @ref robotPushCmd 投递到控制核，
 *              由控制核在 200Hz 任务里统一执行，从根本上杜绝跨核竞态。
 *
 *          失联安全（最重要的一条设计约束）：
 *            - WebSocket 断开回调里会立即投递 @ref C_STOP，
 *              因此手机锁屏、切后台、走出 Wi-Fi、网页崩溃都会让车立刻停下；
 *            - 不依赖「心跳超时」这一种兜底，而是「连接断开 + 心跳超时」双保险。
 *
 * @author  HJZ
 * @version V1.1.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建，完成 HTTP/WS/REST 三通道
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>源文件由 src/ 平铺至根目录；
 *                                                    统一企业级注释规范
 *          </table>
 *
 * @note    遥测推送频率由 @ref Cfg::telemetryHz 决定（默认 15Hz），
 *          推送前会判断 WS 客户端数量，无客户端时不占用 CPU 与带宽。
 *
 * @note    异步 Web 服务有两代主流实现，本项目**同时兼容**，靠编译期宏自动切换：
 *            - **ESP32Async 版 v3.x**（mathieucarbou 维护）—— Arduino core **3.x 必须用这一版**；
 *            - me-no-dev 版 v1.2.x —— 仅适用于 Arduino core 2.x。
 *          切换点：整页下发用 `#if defined(ASYNCWEBSERVER_FORK_ESP32Async)` 分流，
 *          重定向统一传 `const char*`（两代重载的交集）。
 *
 * @warning 所有 API 均无鉴权，仅限局域网使用；禁止将设备直接暴露到公网。
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

#ifndef __WEB_H
#define __WEB_H

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>

/* ==========================================================================
 *                              对外接口
 * ========================================================================== */

/**
 * @brief   初始化 Web 服务（HTTP 服务器 + WebSocket + 全部 REST 路由）
 * @param   无
 * @return  无
 * @note    必须在 @ref wifiInit 之后调用：AP 尚未起来时绑定 80 端口会失败；
 *          同时应在 @ref robotInit 之后调用，避免启动期被 HTTP 请求打断硬件初始化。
 */
void webInit();

/**
 * @brief   Web 服务周期任务（主循环调用，非阻塞）
 * @param[in] now  当前毫秒时间戳（millis()）
 * @return   无
 * @note     内部完成三件事：
 *             ① 按 telemetryHz 定时推送遥测；
 *             ② 取空「完成 / 告警 / 标定」回传队列并广播；
 *             ③ 每 5s 清理已断开的 WebSocket 客户端，防止句柄泄漏。
 */
void webLoop(uint32_t now);

#endif /* __WEB_H */
