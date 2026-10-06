/**
 * @file Wire.h
 * @brief 可注入短读与离线状态的寄存器级 I2C 替身。
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace sensor_host {
inline uint8_t registers[128][256]={0};
inline bool online[128]={false};
inline bool shortRead=false;
}
class SensorHostWire {
  uint8_t address=0,reg=0;
  std::vector<uint8_t> pending,received;
  size_t index=0;
public:
  void beginTransmission(uint8_t value) { address=value; pending.clear(); }
  void write(uint8_t value) { pending.push_back(value); }
  uint8_t endTransmission(bool=true) {
    if(!sensor_host::online[address]) return 4;
    if(pending.empty()) return 4;
    reg=pending[0];
    for(size_t i=1;i<pending.size();++i) sensor_host::registers[address][uint8_t(reg+i-1)]=pending[i];
    return 0;
  }
  size_t requestFrom(uint8_t device,uint8_t count,uint8_t) {
    received.clear(); index=0;
    if(!sensor_host::online[device]) return 0;
    const size_t actual=sensor_host::shortRead && count?count-1:count;
    for(size_t i=0;i<actual;++i) received.push_back(sensor_host::registers[device][uint8_t(reg+i)]);
    return received.size();
  }
  int available() { return int(received.size()-index); }
  uint8_t read() { return received.at(index++); }
};
inline SensorHostWire Wire;
