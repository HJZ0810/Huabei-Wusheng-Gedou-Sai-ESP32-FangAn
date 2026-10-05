/**
 ******************************************************************************
 * @file    sensors.h
 * @brief   外围传感器采集接口（红外测距 / 电池电压 / 灰度 / 光电开关）
 *
 * @details 硬件拓扑（只用 1 个 ADC 引脚 + 4 个地址脚，扩展性强）：
 *            - **CD74HC4067**（16 路模拟开关）：CH0~5 = 红外测距 ×6，CH6 = 电池分压；
 *            - **MCP23017**（I²C IO 扩展，0x20）：GPA0~3 = 灰度 ×4，GPA4~6 = E18 ×3。
 *
 *          为什么必须走 4067 而不是直接接 ADC：
 *          开启 WiFi 后 **ADC2 完全不可用**，而 ESP32-S3 的 ADC1 引脚有限，
 *          用一路 ADC + 4 个地址脚换取 16 路模拟量是最省引脚的方案。
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

#ifndef __SENSORS_H
#define __SENSORS_H

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>

/* ==========================================================================
 *                          公开常量与宏定义
 * ========================================================================== */
#define MUX_SIG   4         /**< CD74HC4067 模拟输出 → GPIO4（ADC1_CH3）      */
#define MUX_S0    5         /**< 4067 地址位 A                                */
#define MUX_S1    6         /**< 4067 地址位 B                                */
#define MUX_S2    7         /**< 4067 地址位 C                                */
#define MUX_S3    8         /**< 4067 地址位 D                                */
#define IR_COUNT  6         /**< 红外测距路数（先装 6 路，可扩展至 12）       */

/**
 * @brief   传感器采集结果（由控制核独占更新）
 */
struct SensorData {
    float ir[IR_COUNT];     /**< 六路红外测距，单位 cm（超量程返回 200）      */
    float bat;              /**< 电池电压，单位 V                             */
    bool  gray[4];          /**< 四路灰度数字量                               */
    bool  e18[3];           /**< 三路光电开关触发状态                         */
    bool  edgeDrop;         /**< 任一 E18 判定为悬空（边缘保护用）            */
    bool  irNear;           /**< 任一红外小于报警距离（限速用）               */
    float irMin;            /**< 最近的一路距离，单位 cm                      */
};

/** @brief 全局传感器数据实例 */
extern SensorData gSen;

/**
 * @brief   初始化传感器子系统（4067 地址脚 + ADC + MCP23017）
 * @param   无
 * @return  true MCP23017 在线（灰度/E18 可用）
 * @note    必须在 imuInit() 之后调用：MCP23017 与 IMU 共用 Wire，
 *          而 Wire 由 imuInit() 负责 begin()。
 */
bool sensorsInit();

/**
 * @brief   扫描一轮全部传感器
 * @param   无
 * @return  无
 * @note    单轮约 1~2ms，控制核以 20Hz 调用即可，不要放在高频环路里。
 */
void sensorsUpdate();

/**
 * @brief   读取 4067 某一通道的电压（调试用）
 * @param[in] ch  通道号 0~15
 * @return  电压值，单位 V
 */
float irRawVoltage(uint8_t ch);

#endif /* __SENSORS_H */
