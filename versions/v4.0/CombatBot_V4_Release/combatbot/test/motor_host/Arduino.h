/**
 * @file    Arduino.h
 * @brief   电机回归测试使用的 Arduino / LEDC 主机替身。
 * @details 只记录硬件调用，不产生真实 GPIO 输出；两套 API 按 core 版本隔离。
 */
#pragma once
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <vector>
#include <esp_arduino_version.h>

#define IRAM_ATTR
#define OUTPUT 1
#define INPUT 0
#define HIGH 1
#define LOW 0
#define RISING 1
#define PI 3.14159265358979323846
using std::isfinite;

namespace motor_host {
struct Attachment { int pin, channel; uint32_t frequency; uint8_t bits; };
static std::vector<Attachment> attachments;
static bool attached[8]={false};
static int channelPins[8]={-1,-1,-1,-1,-1,-1,-1,-1};
static uint32_t duties[8]={0};
static int pinLevels[64]={0};
static int16_t counts[4]={0};
static int failChannel=-1;
static int failWriteChannel=-1;
static uint32_t now=1000;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static unsigned pinWrites=0;
#endif
static unsigned nonzeroMotorWrites=0;
static bool attach(int pin,int channel,uint32_t frequency,uint8_t bits) {
  attachments.push_back({pin,channel,frequency,bits});
  if(channel==failChannel || channel<0 || channel>=8 || bits>14) return false;
  attached[channel]=true; channelPins[channel]=pin; return true;
}
static bool write(int channel,uint32_t duty) {
  if(channel<0 || channel>=8 || !attached[channel] || channel==failWriteChannel) return false;
  duties[channel]=duty;
  if(channel<4 && duty) ++nonzeroMotorWrites;
  return true;
}
}
template<class T> T constrain(T value,T low,T high) {
  return value<low?low:(value>high?high:value);
}
inline uint32_t millis() { return motor_host::now; }
inline void pinMode(int,int) {}
inline void digitalWrite(int pin,int level) { motor_host::pinLevels[pin]=level; }
inline int digitalPinToInterrupt(int pin) { return pin; }
inline void attachInterrupt(int,void(*)(),int) {}
struct MotorHostSerial {
  template<class... T> void printf(const char*,T...) {}
  template<class T> void println(const T&) {}
};
static MotorHostSerial Serial;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
inline bool ledcAttachChannel(uint8_t pin,uint32_t frequency,uint8_t bits,uint8_t channel) {
  return motor_host::attach(pin,channel,frequency,bits);
}
inline bool ledcWriteChannel(uint8_t channel,uint32_t duty) { return motor_host::write(channel,duty); }
// 3.x 的 ledcWrite 参数是 GPIO；保留替身以识别错误沿用 2.x 通道写法。
inline bool ledcWrite(uint8_t pin,uint32_t duty) {
  ++motor_host::pinWrites;
  for(int channel=0;channel<8;++channel)
    if(motor_host::channelPins[channel]==pin) return motor_host::write(channel,duty);
  return false;
}
#else
inline double ledcSetup(uint8_t channel,double frequency,uint8_t bits) {
  return motor_host::attach(-1,channel,uint32_t(frequency),bits)?frequency:0;
}
inline void ledcAttachPin(uint8_t pin,uint8_t channel) { motor_host::channelPins[channel]=pin; }
inline void ledcWrite(uint8_t channel,uint32_t duty) { motor_host::write(channel,duty); }
#endif
