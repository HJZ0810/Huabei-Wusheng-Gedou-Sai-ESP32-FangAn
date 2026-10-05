/** @file Wire.h @brief 只验证电机初始化调用能够链接，不模拟 I2C 设备。 */
#pragma once
struct MotorHostWire {
  bool begin(int,int,uint32_t) { return true; }
  void setTimeOut(uint16_t) {}
};
static MotorHostWire Wire;
