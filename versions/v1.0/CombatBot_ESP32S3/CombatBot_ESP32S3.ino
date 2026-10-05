/**
 ******************************************************************************
 * @file    CombatBot_ESP32S3.ino
 * @brief   ESP32-S3 四驱格斗车无线控制系统 · 工程入口（Arduino 主程序）
 *
 * @details 本文件只做三件事：装配、调度、喂狗。所有业务逻辑都在同目录的功能模块中，
 *          这样 Arduino IDE 会把每个模块列成独立标签页，便于阅读与跳转：
 *            - cfg        —— 配置结构体 + NVS 持久化 + JSON 导入导出
 *            - motor      —— 4×JGB37-3625 驱动（LEDC + 方向 IO + FG 带符号计数）+ 舵机
 *            - pid        —— PID 控制器与梯形速度规划器
 *            - kinematics —— 运动学换算（脉冲 ↔ 厘米 ↔ 角度）
 *            - imu        —— MPU6050 偏航积分 + ADXL345 姿态/冲击 + 互补滤波
 *            - sensors    —— CD74HC4067（红外×6 + 电池）+ MCP23017（灰度×4 + E18×3）
 *            - safety     —— 心跳看门狗 / 急停 / 低压 / 堵转 / 边缘保护
 *            - robot      —— 模式状态机 + 三级闭环（独立 FreeRTOS 任务，200Hz）
 *            - wifi_mgr   —— AP 常驻 + STA 可选状态机 + mDNS + 强制门户
 *            - web        —— HTTP + WebSocket + REST
 *            - web_ui     —— 内嵌单文件网页（PROGMEM）
 *
 *          双核分工（重要）：
 *            - Core 0：robot 控制任务，硬实时 200Hz，负责传感器采集、闭环运算、安全判定；
 *            - Core 1：Arduino loopTask，负责 WiFi 协议栈、WebSocket、REST、DNS。
 *          两者通过「命令队列 + 遥测互斥量 + 完成消息队列」解耦，互不阻塞。
 *
 * @author  CombatBot 电控组
 * @version V1.1.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建，完成固件与内嵌网页
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>源文件由 src/ 平铺至根目录以适配 Arduino IDE；
 *                                                    统一企业级注释规范
 *          </table>
 *
 * @note    上电默认 PWM=0、舵机回中，禁止任何自启动动作（安全设计基线）。
 * @warning 24V 电机电源必须与 ESP32 电源分离但共地，且电机端需 470µF 去耦，
 *          否则电机启停的地弹会复位主控。
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>

#include "cfg.h"        /**< 配置与 NVS 持久化             */
#include "robot.h"      /**< 机器人主控制器（模式机 + 闭环）*/
#include "wifi_mgr.h"   /**< 网络状态机（AP/STA + mDNS）   */
#include "web.h"        /**< HTTP / WebSocket / REST       */
#include "sensors.h"    /**< 传感器数据（仅供调试打印使用） */

/**
 * @brief  系统初始化（上电仅执行一次）
 * @param  无
 * @return 无
 * @note   初始化顺序不可调换：
 *         ① cfgInit  —— 先取回配置，后续模块才能按用户参数初始化；
 *         ② wifiInit —— AP 立即开启，保证任何情况下都能连上控制页；
 *         ③ robotInit—— 初始化 IMU/传感器/电机，并启动控制任务（core0）；
 *         ④ webInit  —— 最后挂载 HTTP/WebSocket，避免启动期被请求打断。
 */
void setup() {
    Serial.begin(115200);
    delay(200);                                     /* 等待 USB CDC 枚举完成 */
    Serial.println("\n=== CombatBot ESP32-S3 启动 ===");

    cfgInit();                                      /* ① 配置加载           */
    wifiInit();                                     /* ② 网络（AP 常驻）    */
    robotInit();                                    /* ③ 硬件 + 控制任务    */
    webInit();                                      /* ④ Web 服务           */

    Serial.println("=== 就绪：http://" + String(gCfg.mdns) +
                   ".local 或 http://" + wifiAPIP() + " ===");
}

/**
 * @brief  主循环（运行于 Core 1，与网络协议栈同核）
 * @param  无
 * @return 无
 * @note   主循环内禁止出现任何阻塞式 delay 与耗时运算，
 *         所有实时任务均已下沉到 robot 控制任务中。
 */
void loop() {
    uint32_t now = millis();

    wifiLoop(now);    /* STA 状态机（15s 重试）+ 强制门户 DNS 处理 */
    webLoop(now);     /* 遥测定时推送 + 事件回传 + WS 客户端清理    */

    delay(1);         /* 让出 CPU 并喂看门狗，保证 loopTask 不饿死  */
}
