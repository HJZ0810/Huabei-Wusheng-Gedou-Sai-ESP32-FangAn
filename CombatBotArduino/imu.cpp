/**
 * @file    imu.cpp
 * @brief   MPU 角速度、加速度采样及 ADXL345 加速度补充采样。
 *
 * ============================================================================
 * 模块职责：探测固定地址的惯性器件，将寄存器原始值换算为 °/s 与 g。
 * 运行约定：MPU 位于 0x68，ADXL345 位于 0x53；缺失器件每 5 s 尝试重新探测。
 * 设计边界：I2C 读取是同步事务；缺失或读取失败时通过有效标志通知调用方。
 *           本层不扣除陀螺零偏，也不积分偏航角，避免与控制层重复处理。
 * ============================================================================
 */
#include "hardware_internal.h"
#include <Wire.h>
#include <math.h>

namespace bot {
namespace {
// ============================================================================
// 器件状态与寄存器字节序
// ============================================================================
bool mpuPresent=false, adxlPresent=false;
uint32_t lastProbe=0;
int16_t big16(const uint8_t* p) { return (int16_t)((uint16_t(p[0])<<8)|p[1]); }
int16_t little16(const uint8_t* p) { return (int16_t)((uint16_t(p[1])<<8)|p[0]); }
/** @brief 探测尚未在线的器件，并在身份校验通过后写入采样配置。 */
void probe() {
  uint8_t who=0;
  if(!mpuPresent) {
    mpuPresent=i2cRead(0x68,0x75,&who,1) && (who==0x68 || who==0x69);
    if(mpuPresent) {
      mpuPresent=i2cWrite(0x68,0x6B,0x01) && i2cWrite(0x68,0x1A,0x03)
        && i2cWrite(0x68,0x19,0x04) && i2cWrite(0x68,0x1B,0x18)
        && i2cWrite(0x68,0x1C,0x00); // 200 Hz，陀螺 ±2000 °/s，加速度 ±2 g。
    }
  }
  who=0;
  if(!adxlPresent) {
    adxlPresent=i2cRead(0x53,0x00,&who,1) && who==0xE5;
    // ADXL345：全分辨率 ±16 g、100 Hz、测量模式，保留格斗冲击测量余量。
    if(adxlPresent) adxlPresent=i2cWrite(0x53,0x31,0x0B)
      && i2cWrite(0x53,0x2C,0x0A) && i2cWrite(0x53,0x2D,0x08);
  }
  lastProbe=millis();
}
}
// ============================================================================
// I2C 事务：连续读取完整数据后才向调用方报告成功
// ============================================================================
/**
 * @brief   向指定器件写入一个寄存器值。
 * @param address 7 位 I2C 地址。
 * @param reg 目标寄存器地址。
 * @param value 待写入的单字节值。
 * @return Wire 事务正常结束时返回 true。
 * @pre Wire 总线已初始化。
 * @note 同步事务受 Wire 超时约束；硬件启动将超时设置为 3 ms。
 */
bool i2cWrite(uint8_t address,uint8_t reg,uint8_t value) {
  Wire.beginTransmission(address); Wire.write(reg); Wire.write(value);
  return Wire.endTransmission()==0;
}
/**
 * @brief   使用重复起始条件，读取连续的寄存器数据。
 * @param address 7 位 I2C 地址。
 * @param reg 连续读取的起始寄存器。
 * @param data 调用方提供的输出缓冲区。
 * @param length 期望读取的字节数，需在 uint8_t 可表达的范围内。
 * @return 寄存器定位成功且实际字节数恰好等于 length 时返回 true。
 * @pre Wire 已初始化，data 至少能容纳 length 字节。
 * @note 短读时清空接收缓存，并保留 data 的既有内容。
 */
bool i2cRead(uint8_t address,uint8_t reg,uint8_t* data,size_t length) {
  Wire.beginTransmission(address); Wire.write(reg);
  if(Wire.endTransmission(false)!=0) return false;
  size_t got=Wire.requestFrom(address,(uint8_t)length,(uint8_t)true);
  if(got!=length) { while(Wire.available()) Wire.read(); return false; }
  for(size_t n=0;n<length;++n) data[n]=Wire.read();
  return true;
}
// ============================================================================
// 惯性反馈：有效标志、轴映射与单位转换
// ============================================================================
/** @brief 执行首次器件探测；配置失败的器件留待后续读取时重试。 */
void imuBegin() { probe(); }
/** @brief 返回 MPU 最近一次探测/读取维护的在线状态，不执行即时 I2C 检查。 */
bool imuAvailable() { return mpuPresent; }
/**
 * @brief   更新本次惯性采样结果，并分别设置角速度及加速度有效标志。
 * @param s 传感器缓存；读取失败时数值可保留旧值，有效性由 imuOk / accelOk 表示。
 * @param cfg 陀螺轴选择与安装方向符号配置。
 * @details MPU 成功时提供角速度与加速度；ADXL345 成功时覆盖加速度字段。
 *          ADXL345 单独在线不能使 imuOk 成立，因为它不提供角速度。
 */
void imuRead(Sensors& s,const Config& cfg) {
  if((!mpuPresent || !adxlPresent) && uint32_t(millis()-lastProbe)>=5000) probe();
  uint8_t data[14];
  s.imuOk=false; s.accelOk=false; s.accelSource=0;
  s.accelSaturated=false; s.impact=false; s.impactG=0;
  if(mpuPresent) {
    if(i2cRead(0x68,0x3B,data,sizeof(data))) {
      // 1. 选择安装轴并调整符号。±2000 °/s 档位的灵敏度为 16.4 LSB/(°/s)。
      //    仅输出换算值，持久化 gyroBias 由控制层扣除，防止零偏被扣两次。
      int axis=constrain(cfg.gyroAxis,0,2);
      s.gyroDps=big16(data+8+axis*2)/16.4f*(cfg.gyroSign<0?-1:1);
      // MPU 使用大端 16 位数据；±2 g 档位按 16384 LSB/g 换算。
      for(int i=0;i<3;++i) {
        const int raw=big16(data+i*2);
        s.acc[i]=raw/16384.0f;
        if(abs(raw)>=32700) s.accelSaturated=true;
      }
      s.imuOk=true; s.accelOk=true;
      s.imuMs=millis();
      s.accelSource=1; // MPU 后备量程仅 ±2 g，不能承诺测出高强度冲击。
    } else mpuPresent=false;
  }
  uint8_t xyz[6];
  if(adxlPresent) {
    if(i2cRead(0x53,0x32,xyz,6)) {
      // 2. ADXL345 使用小端数据，全分辨率比例近似为 0.0039 g/LSB。
      //    加速度可用于碰撞/姿态辅助判断，本模块不据此估算偏航角。
      s.accelSaturated=false;
      for(int i=0;i<3;++i) {
        const int raw=little16(xyz+i*2);
        s.acc[i]=raw*0.0039f;
        if(abs(raw)>=4090) s.accelSaturated=true;
      }
      s.accelOk=true; s.accelSource=2;
    } else adxlPresent=false;
  }
  // 冲击仅供页面告警；碰撞是格斗车的正常工作情形，不据此自动停车。
  // 量程饱和另行报告，模长不够阈值时不推测超量程冲击的真实幅度。
  if(s.accelOk) {
    s.impactG=sqrtf(s.acc[0]*s.acc[0]+s.acc[1]*s.acc[1]+s.acc[2]*s.acc[2]);
    s.impact=s.impactG>=cfg.impactThresholdG;
  }
}
}
