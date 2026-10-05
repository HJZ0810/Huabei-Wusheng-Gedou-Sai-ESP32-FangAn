/**
 * @file Arduino.h
 * @brief 传感器测试的时钟、GPIO 和 ADC 替身；不模拟真实电气特性。
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#define OUTPUT 1
#define INPUT 0
#define LOW 0
#define ADC_11db 3
namespace sensor_host {
inline uint32_t now=100;
inline int levels[64]={0};
inline uint32_t millivolts[16]={0};
}
template<class T> inline T constrain(T value,T low,T high) {
  return value<low?low:(value>high?high:value);
}
inline uint32_t millis() { return sensor_host::now; }
inline void pinMode(int,int) {}
inline void digitalWrite(int pin,int value) { sensor_host::levels[pin]=value; }
inline void analogReadResolution(int) {}
inline void analogSetPinAttenuation(int,int) {}
inline uint32_t analogReadMilliVolts(int) {
  int channel=0;
  for(int i=0;i<4;++i) channel|=sensor_host::levels[5+i]<<i;
  return sensor_host::millivolts[channel];
}
