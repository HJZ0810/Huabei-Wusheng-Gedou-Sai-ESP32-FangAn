/**
 ******************************************************************************
 * @file    imu.h
 * @brief   姿态与偏航解算接口（MPU6050 + ADXL345 互补融合）
 *
 * @details 两块 IMU 的职责必须分清：
 *            - **MPU6050**：提供 Z 轴角速度，积分得到偏航角（Yaw）—— 唯一可信的转向反馈；
 *            - **GY-291（ADXL345）**：纯三轴加速度计，**没有陀螺仪**，
 *              只能用于倾角估算、翻车检测、冲击（被撞）检测、振动监测。
 *
 *          融合策略（互补滤波，增量式）：
 *          @verbatim
 *              yaw += α · dYaw_imu + (1 − α) · dYaw_odo
 *          @endverbatim
 *          采用「增量式」而非「两路各自积分再加权求和」，是因为后者会让两路互相发散；
 *          增量式与本方案文档中的 α 加权在数学上等价，但数值上更稳定。
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
 * @note    MPU6050 零偏随温度漂移，长时间作业前请重做一次零偏标定；
 *          运行中检测到车辆静止（经方差检验确认）时会缓慢修正零偏，
 *          修正量限制在标定值 ±0.1°/s 以内，且不会回写 NVS。
 *
 * @version V2.0.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>统一企业级注释规范
 *          <tr><td>2026-10-05 <td>V2.0  <td>电控组 <td>融合版：零偏自更新加方差
 *                                                    判据，新增自适应偏置复位接口
 *          </table>
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

#ifndef __IMU_H
#define __IMU_H

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>

/**
 * @brief   姿态解算输出数据（由控制核独占更新，其它模块只读）
 */
struct ImuData {
    bool  mpuOK   = false;  /**< MPU6050 是否在线                             */
    bool  adxlOK  = false;  /**< ADXL345 是否在线                             */
    float yaw     = 0;      /**< 融合偏航角，单位 °，逆时针为正，累积不取模   */
    float yawRate = 0;      /**< 偏航角速度，单位 °/s                         */
    float gz      = 0;      /**< 陀螺 Z 轴原始角速度，单位 rad/s              */
    float ax = 0, ay = 0, az = 0;   /**< 三轴加速度，单位 g                   */
    float pitch = 0, roll = 0;      /**< 俯仰 / 横滚，单位 °                  */
    float bias  = 0;        /**< 当前使用的 Z 轴零偏，单位 rad/s              */
    bool  impact = false;   /**< 撞击事件（合加速度超阈值）                   */
};

/** @brief 全局姿态数据实例 */
extern ImuData gImu;

/**
 * @brief   初始化 I²C 与两块 IMU
 * @param   无
 * @return  true 至少有一块 IMU 在线
 * @note    内部会执行 @c Wire.begin(9, 10)，必须在 sensorsInit() 之前调用，
 *          因为 MCP23017 共用同一条 I²C 总线。
 */
bool imuInit();

/**
 * @brief   周期更新姿态（融合偏航、倾角、冲击）
 * @param[in] dt          控制周期，单位 s
 * @param[in] dYawOdo     本周期由里程计推算的偏航增量，单位 °
 * @param[in] stationary  车辆是否静止（四轮无输出且速度接近 0）
 * @return   无
 * @note     静止时会缓慢更新零偏（一阶平滑，避免突变冲击闭环）。
 */
void imuUpdate(float dt, float dYawOdo, bool stationary);

/**
 * @brief   偏航角归零
 * @param   无
 * @return  无
 */
void imuResetYaw();

/**
 * @brief   直接设定当前偏航角
 * @param[in] v  目标偏航角，单位 °
 * @return   无
 */
void imuSetYaw(float v);

/**
 * @brief   复位自适应偏置（显式标定完成后调用）
 * @param   无
 * @return   无
 * @details 一次成功的显式零偏标定会给出全新的基准值，此时必须把长期累积的
 *          自适应增量清零，并丢弃半截的静止观测窗口，
 *          否则旧漂移会叠加在新基准上，反而更不准。
 */
void imuResetAdaptive();

/**
 * @brief   零偏标定：静止采样 ω_z 求均值
 * @param[in] sampleMs  采样时长，单位 ms（建议 3000）
 * @return   标定得到的零偏，单位 rad/s（已写入 NVS）
 * @warning  调用前必须确保车辆完全静止、电机 PWM=0，否则标定结果会被污染。
 */
float imuCalibrateBias(uint32_t sampleMs);

#endif /* __IMU_H */
