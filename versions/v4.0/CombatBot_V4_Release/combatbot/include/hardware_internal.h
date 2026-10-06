/**
 * @file    hardware_internal.h
 * @brief   硬件实现内部共用的惯性、采集及 I2C 辅助接口。
 *
 * ============================================================================
 * 模块职责：连接电机初始化、惯性采样及 IO 采集实现，集中声明底层通信入口。
 * 运行约定：I2C 总线由 hardwareBegin 建立；访问按当前主循环的调用顺序执行。
 * 设计边界：同步 I2C 调用存在等待，3 ms 超时不能保证整个控制 tick 不超时。
 *           在线探测状态和本次采样有效状态含义不同，使用方应按用途区分。
 * ============================================================================
 */
#pragma once
#include "types.h"
#include <Arduino.h>
namespace bot {
// ============================================================================
// 惯性采样与数字 IO 探测
// ============================================================================
/** @brief 首次探测 MPU 和 ADXL345，并配置采样参数。 */
void imuBegin();
/** @brief 读取惯性反馈及有效标志；陀螺输出尚未扣除控制层零偏。 */
void imuRead(Sensors&, const Config&);
/** @brief 返回 MPU 最近维护的在线状态，不执行即时探测。 */
bool imuAvailable();
/** @brief 初始化模拟轮询与数字 IO 扩展器。 */
void sensorsBegin(const Config&);
/** @brief 返回 IO 扩展器在线状态；数字输入首次去抖就绪另看 Sensors::ioOk。 */
bool sensorIoAvailable();
// ============================================================================
// 同步 I2C 事务：失败交由上层撤销有效标志或安排重试
// ============================================================================
/**
 * @brief   写入单字节寄存器。
 * @param address 7 位器件地址。
 * @param reg 寄存器地址。
 * @param value 待写入的数据。
 * @return Wire 事务成功时为 true。
 * @pre Wire 已初始化。
 */
bool i2cWrite(uint8_t address, uint8_t reg, uint8_t value);
/**
 * @brief   连续读取寄存器，仅在完整读取后返回成功。
 * @param address 7 位器件地址。
 * @param reg 起始寄存器地址。
 * @param data 输出缓冲区，至少容纳 length 字节。
 * @param length 读取长度，需处于 uint8_t 可表达范围内。
 * @return 寄存器定位及数据长度均正确时为 true。
 * @pre Wire 已初始化。
 */
bool i2cRead(uint8_t address, uint8_t reg, uint8_t* data, size_t length);
}
