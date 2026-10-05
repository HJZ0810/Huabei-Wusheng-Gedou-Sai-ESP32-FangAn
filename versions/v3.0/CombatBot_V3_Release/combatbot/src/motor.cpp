/**
 * @file    motor.cpp
 * @brief   四轮电机驱动、单相 FG 里程采样与舵机 PWM 输出。
 *
 * ============================================================================
 * 模块职责：将逻辑方向和 PWM 指令转换为 GPIO / LEDC 输出，维护各轮累计里程。
 * 运行约定：目标采样周期为 10 ms；距离单位为 cm，速度为 cm/s，转速为 rpm。
 * 设计边界：单相 FG 只报告边沿数量，方向由已施加的 DIR 推断，无法测量真实方向。
 *           PWM 置零表示撤去驱动；复位期间的安全电平仍依赖外部硬件下拉。
 * ============================================================================
 */
#include "hardware.h"
#include "hardware_internal.h"
#include <Arduino.h>
#include <esp_arduino_version.h>
#include <Wire.h>
#include <driver/pcnt.h>
#include <math.h>

namespace bot {
namespace {
// ============================================================================
// 引脚分配与每轮状态：四路计数器分别对应四路电机
// ============================================================================
constexpr int dirPins[4]={12,15,18,39}, pwmPins[4]={13,16,21,40}, fgPins[4]={14,17,38,41};
constexpr int countLimit=30000;
struct Wheel {
  bool pcnt=false;
  bool appliedInvert=false;
  int sign=1, lastCount=0; // sign 是逻辑驱动方向，不是 FG 实测方向。
  int64_t total=0;
  float filteredSpeed=0;
  uint32_t lastPulseMs=0;
  uint16_t duty=0;
  volatile uint32_t interruptCount=0; // PCNT 初始化失败时，由上升沿中断接替计数。
};
Wheel wheels[4];
void IRAM_ATTR fg0(){ ++wheels[0].interruptCount; }
void IRAM_ATTR fg1(){ ++wheels[1].interruptCount; }
void IRAM_ATTR fg2(){ ++wheels[2].interruptCount; }
void IRAM_ATTR fg3(){ ++wheels[3].interruptCount; }
void (* const fgIsr[4])()={fg0,fg1,fg2,fg3};
bool initialized=false;
bool pwmReady=false;
constexpr uint8_t servoChannel=4, servoBits=14;
constexpr uint32_t servoPeriodUs=20000, servoDutySteps=1UL<<servoBits;

// ============================================================================
// LEDC 兼容层：上层统一按通道写入，隔离 Arduino-ESP32 2.x / 3.x 的 API 差异
// ============================================================================
/**
 * @brief 建立指定通道的 PWM，失败时保留调用方预先设置的低电平。
 * @note 2.x 以通道配置；3.x 显式绑定通道，并按频率/分辨率分配定时器。
 */
bool attachPwm(uint8_t channel,uint8_t pin,uint32_t frequency,uint8_t bits) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  return ledcAttachChannel(pin,frequency,bits,channel);
#else
  if(ledcSetup(channel,frequency,bits)<=0) return false;
  ledcAttachPin(pin,channel);
  return true;
#endif
}
/**
 * @brief 按通道写入占空比；3.x 的 ledcWrite 首参为 GPIO，不能沿用旧调用。
 * @note 2.x 写入接口无返回值；此分支只能确认配置成功，不能检测运行时写入故障。
 */
bool writePwm(uint8_t channel,uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  return ledcWriteChannel(channel,duty);
#else
  ledcWrite(channel,duty);
  return true;
#endif
}
}
// ============================================================================
// 硬件启动：先撤去电机驱动，再建立计数与辅助外设
// ============================================================================
/**
 * @brief   初始化电机、舵机、I2C 总线及传感器。
 * @param cfg 本次启动使用的极性、舵机中心及传感器配置。
 * @note 初始化会执行同步 I2C 事务；Wire 的单次超时设置为 3 ms。
 */
void hardwareBegin(const Config& cfg) {
  bool outputsOk=true;
  pwmReady=false;
  // 1. 软件接管前，PWM 蓝线应由外部下拉保持 0 V；软件无法覆盖上电/复位窗口。
  for(int i=0;i<4;++i) {
    pinMode(pwmPins[i],OUTPUT); digitalWrite(pwmPins[i],LOW);
    pinMode(dirPins[i],OUTPUT);
    digitalWrite(dirPins[i],cfg.invert[i]?LOW:HIGH);
    pinMode(fgPins[i],INPUT);
    // 20 kHz、10 位占空比：上层 PWM 指令统一使用 0~1023 的计数范围。
    const bool attached=attachPwm(i,pwmPins[i],20000,10);
    if(!attached || !writePwm(i,0)) outputsOk=false;
    pcnt_config_t c={};
    c.pulse_gpio_num=fgPins[i]; c.ctrl_gpio_num=PCNT_PIN_NOT_USED;
    c.channel=PCNT_CHANNEL_0; c.unit=(pcnt_unit_t)i;
    c.pos_mode=PCNT_COUNT_INC; c.neg_mode=PCNT_COUNT_DIS;
    c.lctrl_mode=PCNT_MODE_KEEP; c.hctrl_mode=PCNT_MODE_KEEP;
    c.counter_h_lim=countLimit; c.counter_l_lim=-1;
    // 2. 单相 FG 仅累计上升沿；优先交由 PCNT，减少脉冲中断对主循环的干扰。
    bool ok=pcnt_unit_config(&c)==ESP_OK;
    if(ok) {
      pcnt_set_filter_value((pcnt_unit_t)i,100); // APB 为 80 MHz 时，100 个周期约 1.25 μs。
      pcnt_filter_enable((pcnt_unit_t)i);
      pcnt_counter_pause((pcnt_unit_t)i);
      pcnt_counter_clear((pcnt_unit_t)i);
      ok=pcnt_counter_resume((pcnt_unit_t)i)==ESP_OK;
    }
    wheels[i].pcnt=ok;
    wheels[i].appliedInvert=cfg.invert[i];
    wheels[i].lastPulseMs=millis();
    if(!ok) attachInterrupt(digitalPinToInterrupt(fgPins[i]),fgIsr[i],RISING);
  }
  // 3. ESP32-S3 的 LEDC 最大为 14 位；50 Hz 舵机与 20 kHz 电机使用不同定时器。
  //    2.x 通道 4 对应 timer 2；3.x 按不同的频率/分辨率自动分配独立定时器。
  pinMode(11,OUTPUT); digitalWrite(11,LOW);
  const bool servoAttached=attachPwm(servoChannel,11,50,servoBits);
  if(!servoAttached || !writePwm(servoChannel,uint32_t(cfg.servoCenterUs)*servoDutySteps/servoPeriodUs)) outputsOk=false;
  // 任一路输出初始化失败，整车保持撤驱动；健康检查会拒绝后续运动请求。
  pwmReady=outputsOk;
  if(!pwmReady) { motorStop(); Serial.println("PWM initialization failed; motion disabled"); }
  Wire.begin(9,10,400000); Wire.setTimeOut(3);
  imuBegin(); sensorsBegin(cfg); initialized=true;
}
// ============================================================================
// 反馈采样：边沿增量 → 有符号里程 → 低通速度 → 轮转速
// ============================================================================
/**
 * @brief   更新四轮累计脉冲、滤波线速度与轮转速。
 * @param dt 距上次采样的时间，单位 s；内部限幅至 0.0001~1 s。
 * @param cfg 轮径、有效 PPR 和可选实测里程系数。
 * @param pulses 输出四轮有符号累计 FG 边沿数。
 * @param speed 输出四轮滤波线速度，单位 cm/s。
 * @param rpm 输出四轮滤波转速，单位 rpm。
 * @pre 输出数组均至少含 4 个元素；轮径及计数系数已经配置校验。
 * @note 每次采样间隔内必须少于 30000 个 FG 边沿，才能正确展开 PCNT 回卷。
 */
void motorSample(float dt,const Config& cfg,int64_t pulses[4],float speed[4],float rpm[4]) {
  dt=constrain(dt,0.0001f,1.0f);
  // 1. k 的单位为边沿/cm；未标定时，用 PPR / 轮周长计算。轮径 mm / 10 转为 cm。
  const float k=cfg.pulsesPerCm>0?cfg.pulsesPerCm:cfg.ppr/(PI*cfg.wheelMm/10.0f);
  for(int i=0;i<4;++i) {
    Wheel& w=wheels[i]; int delta=0;
    if(w.pcnt) {
      int16_t raw=0;
      if(pcnt_get_counter_value((pcnt_unit_t)i,&raw)==ESP_OK) {
        delta=int(raw)-w.lastCount;
        // 2. PCNT 达到高限后归零，负差值补回一次计数周期。
        //    保留连续计数可避开“读取后再清零”的丢边沿窗口；该算法只支持一次回卷。
        //    10 ms 是调度目标，长时间挂起或过高脉冲频率仍可能造成不可恢复的漏计。
        if(delta<0) delta+=countLimit;
        w.lastCount=raw;
      }
    } else {
      uint32_t raw=w.interruptCount;
      delta=(uint32_t)(raw-(uint32_t)w.lastCount);
      w.lastCount=(int)raw;
    }
    if(delta>0) w.lastPulseMs=millis();
    w.total+=(int64_t)delta*w.sign;
    // 3. v = 有符号边沿数 / (边沿/cm × s)。单相 FG 的符号来自当前驱动方向。
    float instant=delta*w.sign/(k*dt);
    // 一阶低通时间常数 τ = 0.08 s；α = dt / (τ + dt)，随实际采样间隔调整。
    float alpha=dt/(0.08f+dt);
    w.filteredSpeed+=alpha*(instant-w.filteredSpeed);
    pulses[i]=w.total; speed[i]=w.filteredSpeed;
    // 4. rpm = v × 60 / 轮周长。里程标定与 PPR 必须采用相同的 FG 上升沿口径。
    rpm[i]=w.filteredSpeed*60.0f/(PI*cfg.wheelMm/10.0f);
  }
}
// ============================================================================
// 执行输出：换向等待与撤驱动保护
// ============================================================================
/**
 * @brief   写入单轮有符号 PWM，并在方向或极性改变时等待余转衰减。
 * @param index 轮索引，取值 0~3；越界指令直接忽略。
 * @param signedPwm 正负号指定逻辑方向，绝对值限幅至 0~1023。
 * @param cfg 各轮方向反转配置。
 * @note 非有限数或绝对值小于 0.5 时撤驱动；换向等待跨调用进行，无 delay。
 * @details 换向需同时满足：距末次 FG 边沿至少 120 ms，且滤波速度不高于
 *          0.8 cm/s。等待期间沿用旧计数方向；外力倒推无法被单相 FG 识别。
 */
void motorWrite(int index,float signedPwm,const Config& cfg) {
  if(index<0 || index>=4 || !pwmReady) return;
  Wheel& w=wheels[index];
  if(!isfinite(signedPwm)) { w.duty=0; writePwm(index,0); return; }
  if(fabsf(signedPwm)<0.5f) { w.duty=0; writePwm(index,0); return; }
  int desiredSign=signedPwm>0?1:-1;
  if(desiredSign!=w.sign || w.appliedInvert!=cfg.invert[index]) {
    w.duty=0; writePwm(index,0);
    // 先撤驱动，再观察静止条件；这里提前返回，将等待时间留给后续控制 tick。
    if(uint32_t(millis()-w.lastPulseMs)<120 || fabsf(w.filteredSpeed)>0.8f) return;
    w.sign=desiredSign;
    w.appliedInvert=cfg.invert[index];
  }
  digitalWrite(dirPins[index],((w.sign>0)!=cfg.invert[index])?HIGH:LOW);
  w.duty=(uint16_t)constrain(fabsf(signedPwm),0.0f,1023.0f);
  writePwm(index,w.duty);
}
/** @brief 撤去四轮驱动，保留方向引脚和计数状态。 */
void motorStop() {
  for(int i=0;i<4;++i) { wheels[i].duty=0; writePwm(i,0); }
  // 本接口没有独立 BRAKE 输出；PWM = 0 不能提供主动制动，车辆仍可能惯性滑行。
}
/**
 * @brief   读取最近一次已施加的四轮 PWM，占空比计数范围为 0~1023。
 * @param pwm 输出数组，至少含 4 个元素。
 */
void motorOutputs(uint16_t pwm[4]) { for(int i=0;i<4;++i) pwm[i]=wheels[i].duty; }
/**
 * @brief   将归一化机构位置换算为舵机角度和脉宽。
 * @param pos 目标位置，0 对应下端、1 对应上端；超范围值限幅，非有限数忽略。
 * @param cfg 上下端角度以及允许的脉宽范围，脉宽单位 μs。
 * @note 50 Hz 的周期为 20000 μs；14 位占空比按 脉宽 / 周期 × 16384 计算。
 */
void servoWrite(float pos,const Config& cfg) {
  if(!pwmReady || !isfinite(pos)) return;
  float degrees=cfg.servoDownDeg+(cfg.servoUpDeg-cfg.servoDownDeg)*constrain(pos,0.0f,1.0f);
  int us=cfg.servoMinUs+(cfg.servoMaxUs-cfg.servoMinUs)*constrain(degrees,0.0f,180.0f)/180.0f;
  writePwm(servoChannel,uint32_t(constrain(us,cfg.servoMinUs,cfg.servoMaxUs))*servoDutySteps/servoPeriodUs);
}
/** @brief 返回初始化、PWM 输出和 MPU 探测均成功的标志；不代表全部外设健康。 */
bool hardwareHealthy() { return initialized && pwmReady && imuAvailable(); }
}
