/**
 ******************************************************************************
 * @file    motor.h
 * @brief   4×JGB37-3625 无刷减速电机驱动接口 + 铲子舵机接口
 *
 * @details 该电机「自带驱动板」，正确的驱动方式是 **方向电平 + 调速电压**，
 *          而不是 H 桥。四线制定义：
 *            - 红 / 黑：24V 电源（不进 MCU）
 *            - 白线   ：方向电平（悬空/高 = CCW，低 = CW）
 *            - 蓝线   ：0~5V 调速电压（由 20kHz PWM 经电平转换 + RC 滤波生成）
 *            - 黄线   ：FG 测速脉冲（5V 方波，单相、无方向信息）
 *
 *          两个最容易踩的坑（代码里已针对性处理）：
 *            1. FG 无方向 → 方向由 MCU 下发的指令决定，ISR 内带符号累加（@ref s_dir）；
 *            2. 静摩擦大 → 占空比必须跨过死区，否则低速段 PID 输出转不动轮子。
 *
 * @author  HJZ
 * @version V1.1.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>补充 LEDC 跨版本兼容宏；统一注释规范
 *          </table>
 *
 * @warning FG 为 5V 电平，必须经 1kΩ+2kΩ 分压（或电平转换器）降到 3.3V 后再接 ESP32，
 *          5V 直连会永久损坏 IO。
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

#ifndef __MOTOR_H
#define __MOTOR_H

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>

/* ==========================================================================
 *                          公开常量与宏定义
 * ========================================================================== */
#define MOTOR_COUNT 4       /**< 电机数量（四驱）                              */

/* --------------------------------------------------------------------------
 * 引脚分配：已避开 strapping（GPIO0/3/45/46）、USB（19/20）、Flash/PSRAM（26~37）
 * -------------------------------------------------------------------------- */
#define PIN_M1_DIR 12       /**< 左前电机 · 方向线                             */
#define PIN_M1_PWM 13       /**< 左前电机 · 调速 PWM                           */
#define PIN_M1_FG  14       /**< 左前电机 · FG 测速脉冲                        */
#define PIN_M2_DIR 15       /**< 右前电机 · 方向线                             */
#define PIN_M2_PWM 16       /**< 右前电机 · 调速 PWM                           */
#define PIN_M2_FG  17       /**< 右前电机 · FG 测速脉冲                        */
#define PIN_M3_DIR 18       /**< 左后电机 · 方向线                             */
#define PIN_M3_PWM 21       /**< 左后电机 · 调速 PWM                           */
#define PIN_M3_FG  38       /**< 左后电机 · FG 测速脉冲                        */
#define PIN_M4_DIR 39       /**< 右后电机 · 方向线                             */
#define PIN_M4_PWM 40       /**< 右后电机 · 调速 PWM                           */
#define PIN_M4_FG  41       /**< 右后电机 · FG 测速脉冲                        */
#define PIN_SERVO  11       /**< 铲子舵机 · 50Hz PWM                           */

/* --------------------------------------------------------------------------
 * LEDC 参数：通道 0~3 给电机，通道 4 给舵机（S3 共 8 路，留 3 路余量）
 * -------------------------------------------------------------------------- */
#define PWM_FREQ_MOTOR  20000   /**< 电机 PWM 频率 20kHz（高于音频，避免啸叫）  */
#define PWM_RES_MOTOR   10      /**< 电机 PWM 分辨率 10bit → 占空比 0~1023      */
#define PWM_FREQ_SERVO  50      /**< 舵机 PWM 频率 50Hz（周期 20000µs）         */
#define PWM_RES_SERVO   16      /**< 舵机 PWM 分辨率 16bit → 0.305µs/计数       */

/* ==========================================================================
 *                              对外接口 · 电机
 * ========================================================================== */

/**
 * @brief   初始化电机子系统（LEDC / 方向 IO / FG 中断）与舵机
 * @param   无
 * @return  无
 * @note    上电后所有 PWM = 0、方向线置低、舵机回中，禁止任何自启动动作。
 *          FG 引脚以 RISING 触发中断（一个上升沿 = 一个脉冲；
 *          若改用 CHANGE 双边沿计数，等效 PPR 会翻倍，务必同步修改配置）。
 */
void motorInit();

/**
 * @brief   设置单个电机的归一化输出
 * @param[in] i     电机索引：0=左前，1=右前，2=左后，3=右后
 * @param[in] norm  归一化速度指令，[-1.0, +1.0]，正数为前进
 * @return   无
 * @note     输出链路依次施加：极性反转 → 左/右侧出力缩放 → 死区补偿 → 限幅。
 * @warning  方向符号只在 |norm| > 0.001 时更新；停车滑行阶段保持上一次的符号，
 *           这正是里程计在惯性滑行时仍能正确累计的前提。
 */
void motorSetNorm(uint8_t i, float norm);

/**
 * @brief   停止单个电机（PWM = 0，惰行）
 * @param[in] i  电机索引
 * @return   无
 */
void motorStop(uint8_t i);

/**
 * @brief   停止全部四个电机
 * @param   无
 * @return  无
 * @note    仅切断 PWM（惰行）；如需急停请配合短路刹车的硬件方案。
 */
void motorStopAll();

/**
 * @brief   周期结算：转速、线速度、堵转判定（由控制环按固定周期调用）
 * @param[in] dt  调用间隔，单位 s（控制环为 0.005）
 * @return   无
 * @note     转速采用 50ms 滑动窗口结算：窗口太短分辨率不足，太长响应迟钝；
 *           停转超过 200ms 时主动把转速清零，避免低速残留。
 */
void motorUpdate(float dt);

/**
 * @brief   读取某电机的累计带符号脉冲数
 * @param[in] i  电机索引
 * @return  累计脉冲数（前进为正，后退为负）
 */
int32_t motorPulses(uint8_t i);

/**
 * @brief   清零某电机的脉冲计数
 * @param[in] i  电机索引
 * @return   无
 */
void motorResetPulses(uint8_t i);

/**
 * @brief   清零全部四个电机的脉冲计数
 * @param   无
 * @return  无
 */
void motorResetAllPulses();

/**
 * @brief   读取某电机的转速
 * @param[in] i  电机索引
 * @return  带符号转速，单位 rpm
 */
float motorRPM(uint8_t i);

/**
 * @brief   读取某电机的线速度
 * @param[in] i  电机索引
 * @return  带符号线速度，单位 cm/s
 */
float motorSpeed(uint8_t i);

/**
 * @brief   读取某电机当前实际输出的占空比
 * @param[in] i  电机索引
 * @return  占空比值 0~1023（用于堵转判定与网页显示）
 */
uint16_t motorDuty(uint8_t i);

/**
 * @brief   查询某电机是否处于堵转状态
 * @param[in] i  电机索引
 * @return  true 堵转（占空比高但持续无脉冲）
 * @note    判定条件由 motorUpdate() 维护，阈值见配置 @ref Cfg::stallTime。
 */
bool motorIsStalled(uint8_t i);

/**
 * @brief   查询某电机距最近一次 FG 边沿的时间
 * @param[in] i  电机索引
 * @return  无脉冲持续时长，单位 ms
 * @note    本值由 motorUpdate() 维护，可用于「余转是否已停」的上层判据。
 *          融合版新增接口：配合 motor.cpp 内部的换向保护使用同一套时间基准。
 */
uint32_t motorIdleMs(uint8_t i);

/* ==========================================================================
 *                              对外接口 · 舵机
 * ========================================================================== */

/**
 * @brief   初始化舵机（50Hz / 16bit LEDC）
 * @param   无
 * @return  无
 */
void servoInit();

/**
 * @brief   按脉宽设置舵机位置
 * @param[in] us  目标脉宽，单位 µs（典型 500~2500µs）
 * @return   无
 * @note     内部会按 @ref Cfg::servoMin / @ref Cfg::servoMax 截断，
 *           因此调用方无需关心机械限位。
 */
void servoSetUS(int us);

/**
 * @brief   舵机回中
 * @param   无
 * @return  无
 */
void servoCenter();

/**
 * @brief   举铲（武器抬起）
 * @param   无
 * @return  无
 */
void servoUp();

/**
 * @brief   放铲（武器落下）
 * @param   无
 * @return  无
 */
void servoDown();

/**
 * @brief   读取舵机当前脉宽
 * @param   无
 * @return  当前脉宽，单位 µs
 */
int servoCurrentUS();

#endif /* __MOTOR_H */
