/**
 * @file    sensors.cpp
 * @brief   数字输入去抖、模拟多路轮询与传感器快照汇总。
 *
 * ============================================================================
 * 模块职责：汇总惯性数据，采集 MCP23017 数字输入及 CD74HC4067 模拟通道。
 * 运行约定：模拟红外依次占用除电池通道外的通道；电池作为最后一个采样槽位。
 * 设计边界：多路选择后的稳定等待跨控制 tick 完成，I2C 与 ADC 调用仍为同步。
 *           缓存包含不同时间采集的值；使用方必须检查各类有效标志。
 * ============================================================================
 */
#include "hardware.h"
#include "hardware_internal.h"
#include <Arduino.h>
#include <math.h>

namespace bot {
namespace {
// ============================================================================
// 轮询状态：物理通道、逻辑槽位与数字输入候选状态
// ============================================================================
constexpr int selectPins[4]={5,6,7,8};
Sensors cache;
bool ioPresent=false;
uint32_t lastDigital=0,lastProbe=0,selectedMs=0;
int slot=0,lastIrCount=6,lastBatteryChannel=6;
float lastIrScale=1;
uint32_t analogMs[13]={0}; // 12 路红外加 1 路电池，记录各逻辑槽位最后一次采样时间。
bool digitalSeen[7]={false}, digitalReady[7]={false}, digitalCandidate[7]={false};
uint32_t candidateSince[7]={0};
/**
 * @brief   将逻辑采样槽位映射到 4067 的物理通道。
 * @param sample 0~irCount-1 为红外槽位，irCount 为电池槽位。
 * @param cfg 红外路数与电池物理通道配置。
 * @return 已跳过电池预留通道的物理通道索引。
 */
int channelFor(int sample,const Config& cfg) {
  if(sample==cfg.irCount) return cfg.batteryChannel;
  return sample>=cfg.batteryChannel?sample+1:sample;
}
/**
 * @brief   写入四位选择地址，并记录开始等待模拟信号稳定的时间。
 * @param channel 4067 的物理通道索引。
 */
void selectChannel(int channel) {
  for(int i=0;i<4;++i) digitalWrite(selectPins[i],(channel>>i)&1);
  selectedMs=millis();
}
/** @brief 探测地址 0x20 的 MCP23017，将双端口设为输入并启用 GPA0~6 上拉。 */
void probeIo() {
  uint8_t iodir=0;
  ioPresent=i2cRead(0x20,0x00,&iodir,1)
    && i2cWrite(0x20,0x00,0xFF) && i2cWrite(0x20,0x01,0xFF)
    && i2cWrite(0x20,0x0C,0x7F);
  lastProbe=millis();
}
}
// ============================================================================
// 初始化与缓存读取
// ============================================================================
/**
 * @brief   初始化模拟选择引脚、ADC 和数字 IO 扩展器。
 * @param cfg 已校验的模拟通道配置。
 * @note ADC 使用 GPIO 4、12 位分辨率与 ADC_11db 衰减；实际电压仍需实测标定。
 */
void sensorsBegin(const Config& cfg) {
  for(int pin:selectPins) { pinMode(pin,OUTPUT); digitalWrite(pin,LOW); }
  pinMode(4,INPUT); analogReadResolution(12); analogSetPinAttenuation(4,ADC_11db);
  lastIrCount=cfg.irCount; lastBatteryChannel=cfg.batteryChannel; lastIrScale=cfg.irScale;
  probeIo(); selectChannel(channelFor(0,cfg));
}
/** @brief 返回 IO 扩展器的探测状态；所有数字输入稳定后另由快照 ioOk 表示。 */
bool sensorIoAvailable() { return ioPresent; }
/**
 * @brief   推进传感器轮询，返回当前缓存快照。
 * @param cfg 数字有效电平、去抖时长、红外路数及电池换算参数。
 * @return 最新缓存；sampledMs 是本次汇总时刻，不等于每一路的实际采样时刻。
 * @pre 红外路数、电池通道和去抖参数已通过配置校验。
 * @details 数字输入每 20 ms 检查一次，候选电平连续稳定后才提交；模拟输入
 *          每次最多采一槽，切换后至少等待 2 ms，超过 1 s 未更新则标记失效。
 */
Sensors sensorsRead(const Config& cfg) {
  uint32_t now=millis();
  imuRead(cache,cfg);
  if(!ioPresent && uint32_t(now-lastProbe)>5000) probeIo();
  // 1. 将电平按设备极性归一化；数字去抖只针对离散读取到的电平变化。
  if(uint32_t(now-lastDigital)>=20) {
    uint8_t data=0;
    if(ioPresent && i2cRead(0x20,0x12,&data,1)) {
      bool allReady=true;
      for(int i=0;i<7;++i) {
        bool sensed=bool(data&(1<<i))==(i<4?cfg.grayActiveHigh:cfg.e18ActiveHigh);
        if(i>=4) cache.groundRaw[i-4]=sensed;
        if(!digitalSeen[i] || sensed!=digitalCandidate[i]) {
          digitalSeen[i]=true; digitalCandidate[i]=sensed; candidateSince[i]=now;
        }
        if(uint32_t(now-candidateSince[i])>=(uint32_t)cfg.digitalDebounceMs) {
          digitalReady[i]=true;
          if(i<4) cache.gray[i]=digitalCandidate[i]; else cache.e18[i-4]=digitalCandidate[i];
        }
        allReady=allReady && digitalReady[i];
      }
      // 首次启动/重连需等待全部 7 路完成首次稳定，ioOk 才成立。
      // 已就绪的通道出现新候选值时，旧输出暂时保留，ioOk 不因候选变化撤销。
      cache.ioOk=allReady;
      cache.digitalMs=now;
    } else {
      cache.ioOk=false; ioPresent=false;
      for(int i=0;i<7;++i) { digitalSeen[i]=false; digitalReady[i]=false; }
    }
    lastDigital=now;
  }
  // 2. 通道布局变化后重新开始轮询；旧数值可以保留，时间戳归零使其暂时无效。
  if(lastIrCount!=cfg.irCount || lastBatteryChannel!=cfg.batteryChannel) {
    slot=0; lastIrCount=cfg.irCount; lastBatteryChannel=cfg.batteryChannel;
    for(int i=0;i<13;++i) analogMs[i]=0;
    selectChannel(channelFor(slot,cfg));
  }
  // 距离比例变化后撤销全部红外缓存的有效性，直到各路重新采样。
  // 电池测量不使用该比例，保留其独立的有效缓存。
  if(lastIrScale!=cfg.irScale) {
    lastIrScale=cfg.irScale;
    for(int i=0;i<cfg.irCount;++i) { analogMs[i]=0; cache.irValid[i]=false; }
  }
  // 3. 以时间戳跨 tick 等待 4067/分压网络稳定，避免在此处使用 delay。
  //    这只消除了稳定等待；前面的 I2C 以及本次 ADC 采样仍可能占用执行时间。
  if(uint32_t(now-selectedMs)>=2) {
    float volts=analogReadMilliVolts(4)/1000.0f;
    analogMs[slot]=now;
    if(slot==cfg.irCount) {
      cache.batteryV=volts*cfg.batteryScale;
      cache.batteryValid=volts>0.2f && volts<3.15f && cache.batteryV<40;
    } else {
      // GP2Y0A02 曲线近似：d(cm) = 60 / (U(V) - 0.05)，每一路都应实测核验。
      // 电压需为 0.35~3.15 V，原始估算需为 20~150 cm，两项同时满足才有效。
      // 存储值限幅仅便于展示，不会让越界测量变有效；20 cm 内的非单调区不可靠。
      float cm=60.0f/fmaxf(0.05f,volts-0.05f);
      cache.ir[slot]=constrain(cm,20.0f,150.0f)*cfg.irScale;
      cache.irValid[slot]=volts>=0.35f && volts<=3.15f && cm>=20 && cm<=150;
      cache.irMs[slot]=now;
    }
    slot=(slot+1)%(cfg.irCount+1); selectChannel(channelFor(slot,cfg));
  }
  // 4. 未配置、尚未首次采样或超过 1 s 未更新的模拟反馈，一律撤销有效标志。
  for(int i=0;i<12;++i) {
    if(i>=cfg.irCount || analogMs[i]==0 || uint32_t(now-analogMs[i])>1000) cache.irValid[i]=false;
  }
  if(analogMs[cfg.irCount]==0 || uint32_t(now-analogMs[cfg.irCount])>1000) cache.batteryValid=false;
  cache.sampledMs=now;
  return cache;
}
}
