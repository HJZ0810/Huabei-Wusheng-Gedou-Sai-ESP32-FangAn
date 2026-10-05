/**
 * @file test_sensor_model.cpp
 * @brief 使用真实采集实现验证量程、有效性与比例变更后的缓存撤销。
 * @note I2C/ADC/时钟由确定性替身提供，不验证器件电气连接、ADC 精度或实际采样延迟。
 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include <Arduino.h>
#include <Wire.h>
#include "hardware.h"
#include "hardware_internal.h"

static void big16(uint8_t address,int reg,int16_t value) {
  sensor_host::registers[address][reg]=uint16_t(value)>>8;
  sensor_host::registers[address][reg+1]=uint16_t(value)&255;
}
static void little16(uint8_t address,int reg,int16_t value) {
  sensor_host::registers[address][reg]=uint16_t(value)&255;
  sensor_host::registers[address][reg+1]=uint16_t(value)>>8;
}
static bool near(float actual,float expected) { return std::fabs(actual-expected)<0.01f; }
static bot::Sensors sample(const bot::Config& cfg,int iterations=1,uint32_t interval=10) {
  bot::Sensors s;
  for(int i=0;i<iterations;++i) { sensor_host::now+=interval; s=bot::sensorsRead(cfg); }
  return s;
}
int main() {
  bot::Config cfg;
  sensor_host::online[0x20]=sensor_host::online[0x53]=sensor_host::online[0x68]=true;
  sensor_host::registers[0x68][0x75]=0x68;
  sensor_host::registers[0x53][0x00]=0xE5;
  big16(0x68,0x3F,16384); // MPU Z=1 g。
  little16(0x53,0x36,256); // ADXL Z≈1 g。
  for(int i=0;i<16;++i) sensor_host::millivolts[i]=1200;
  sensor_host::millivolts[6]=2200;
  bot::imuBegin(); bot::sensorsBegin(cfg);
  assert(sensor_host::registers[0x53][0x31]==0x0B); // 真实初始化写入 ±16 g。
  auto s=sample(cfg,14);
  assert(s.imuOk && s.accelOk && s.accelSource==2 && !s.accelSaturated && !s.impact);
  assert(s.ioOk && s.batteryValid && near(s.batteryV,24.2f));
  for(int i=0;i<cfg.irCount;++i) assert(s.irValid[i] && near(s.ir[i],60.0f/1.15f));

  // ADXL 冲击只改变采集告警，不产生电机控制或故障状态。
  little16(0x53,0x32,768); // X≈3 g。
  s=sample(cfg); assert(s.impact && s.impactG>3 && s.accelSource==2);
  cfg.impactThresholdG=8;
  s=sample(cfg); assert(!s.impact);
  little16(0x53,0x32,4095);
  s=sample(cfg); assert(s.accelSaturated && s.impact);

  // ADXL 掉线后回落到 MPU ±2 g，饱和状态与来源必须一起更新。
  sensor_host::online[0x53]=false;
  s=sample(cfg); assert(s.accelOk && s.accelSource==1 && !s.accelSaturated && !s.impact);
  big16(0x68,0x3B,32767);
  s=sample(cfg); assert(s.accelSource==1 && s.accelSaturated && !s.impact);
  sensor_host::online[0x68]=false;
  s=sample(cfg); assert(!s.imuOk && !s.accelOk && s.accelSource==0 && !s.impact && s.impactG==0);

  // 比例变更立即撤销旧 IR 有效缓存；逐路重新采样，电池缓存保留。
  cfg.irScale=2;
  s=sample(cfg);
  int valid=0; for(int i=0;i<cfg.irCount;++i) valid+=s.irValid[i]?1:0;
  assert(valid<=1 && s.batteryValid);
  s=sample(cfg,14);
  for(int i=0;i<cfg.irCount;++i) assert(s.irValid[i] && near(s.ir[i],2*60.0f/1.15f));

  // 原始电压不合法时，即使修正值落入显示范围，也不可报告有效。
  sensor_host::millivolts[0]=200;
  s=sample(cfg,14); assert(!s.irValid[0]);
  cfg.irScale=0.5f;
  s=sample(cfg,14); assert(!s.irValid[0]);

  // 扩展到 12 路仍跳过电池 CH6；逻辑 IR6 对应物理 CH7。
  cfg.irCount=12; sensor_host::millivolts[7]=1000;
  s=sample(cfg,26);
  assert(s.irValid[6] && near(s.ir[6],0.5f*60.0f/0.95f));
  assert(s.batteryValid && near(s.batteryV,24.2f));
  // 停顿后仅本次采到的一路有效，其余过期反馈不能继续参与保护。
  s=sample(cfg,1,1100);
  valid=0; for(int i=0;i<cfg.irCount;++i) valid+=s.irValid[i]?1:0;
  assert(valid<=1);
  std::puts("sensor validity/range/cache tests passed");
}
