/**
 * @file    test_motor.cpp
 * @brief   直接执行真实 motor.cpp，验证 core 2.x / 3.x 的 PWM 行为。
 *
 * ============================================================================
 * 覆盖范围：通道、频率、分辨率、舵机脉宽、撤驱动、换向及初始化失败门控。
 * 运行约定：每个故障场景启动独立进程，模拟全新的硬件启动生命周期。
 * 设计边界：替身仅验证软件 API 契约，不证明实际电平、时序或机械行为。
 * ============================================================================
 */
#include <Arduino.h>
#include <cstdlib>
#include <limits>
#include <iostream>
#include "../src/motor.cpp"

namespace bot {
void imuBegin() {}
bool imuAvailable() { return true; }
void sensorsBegin(const Config&) {}
}

static void check(bool condition,const char* reason) {
  if(!condition) { std::cerr<<"FAIL: "<<reason<<'\n'; std::exit(1); }
}
static void allStopped() {
  uint16_t output[4]; bot::motorOutputs(output);
  for(int i=0;i<4;++i) {
    check(output[i]==0,"telemetry must report zero motor duty");
    check(motor_host::duties[i]==0,"hardware motor duty must be zero");
  }
}
int main(int argc,char** argv) {
  bot::Config cfg;
  if(argc==3) {
    if(std::string(argv[1])=="attach-failure") motor_host::failChannel=std::atoi(argv[2]);
    else if(std::string(argv[1])=="write-failure") motor_host::failWriteChannel=std::atoi(argv[2]);
    else check(false,"unknown scenario");
    bot::hardwareBegin(cfg);
    check(!bot::hardwareHealthy(),"PWM initialization failure must be visible to controller");
    for(int i=0;i<4;++i) bot::motorWrite(i,600,cfg);
    bot::motorStop(); allStopped();
    check(motor_host::nonzeroMotorWrites==0,"one failed channel must disable every motor");
    std::cout<<"PASS core "<<ESP_ARDUINO_VERSION_MAJOR<<": "<<argv[1]<<' '<<argv[2]<<'\n';
    return 0;
  }

  bot::hardwareBegin(cfg);
  const uint32_t session=bot::hardwareBootId();
  check(session!=0 && session==bot::hardwareBootId(),"boot session must be nonzero and stable");
  check(bot::hardwareHealthy(),"successful initialization must be healthy");
  check(bot::hardwareOutputsHealthy(),"output health must be independent of optional sensors");
  check(motor_host::attachments.size()==5,"four motors and one servo must be attached");
  const int pins[5]={13,16,21,40,11};
  for(int i=0;i<5;++i) {
    const auto& a=motor_host::attachments[i];
    check(a.channel==i,"PWM channel mapping changed");
    check(motor_host::channelPins[i]==pins[i],"PWM GPIO mapping changed");
    check(a.frequency==(i<4?20000u:50u),"motor and servo frequencies must remain separate");
    check(a.bits==(i<4?10:14),"ESP32-S3 servo resolution must not exceed 14 bits");
  }
  allStopped();
  check(motor_host::duties[4]==1228,"1500 us center must map to 1228 counts at 14 bits");
  cfg.servoDownDeg=90; cfg.servoUpDeg=90;
  bot::servoWrite(0.5f,cfg);
  check(motor_host::duties[4]==1228,"servoWrite must use the same pulse-width scale as initialization");
  bot::servoWrite(std::numeric_limits<float>::quiet_NaN(),cfg);
  check(motor_host::duties[4]==1228,"invalid servo request must preserve the previous output");

  for(int i=0;i<4;++i) bot::motorWrite(i,500,cfg);
  for(int i=0;i<4;++i) check(motor_host::duties[i]==500,"each motor must receive its own nonzero duty");
  bot::motorWrite(0,2000,cfg); check(motor_host::duties[0]==1023,"motor duty must clamp to 10-bit maximum");
  bot::motorWrite(0,0,cfg); check(motor_host::duties[0]==0,"zero request must withdraw drive");
  bot::motorWrite(1,std::numeric_limits<float>::infinity(),cfg);
  check(motor_host::duties[1]==0,"non-finite request must withdraw drive");
  bot::motorStop(); allStopped();

  // 反向请求先撤驱动，达到 120 ms 静止窗口后才更改 DIR。
  bot::motorWrite(0,300,cfg);
  motor_host::now=1119; bot::motorWrite(0,-300,cfg);
  check(motor_host::duties[0]==0 && motor_host::pinLevels[12]==HIGH,"early reversal must stop and retain DIR");
  motor_host::now=1120; bot::motorWrite(0,-300,cfg);
  check(motor_host::duties[0]==300 && motor_host::pinLevels[12]==LOW,"stationary reversal must apply the new direction");

  // 新 FG 边沿会重置静止窗口；速度滤波尚未衰减时同样禁止换向。
  int64_t pulses[4]; float speed[4],rpm[4];
  motor_host::counts[0]=10; motor_host::now=1130;
  bot::motorSample(0.01f,cfg,pulses,speed,rpm);
  check(pulses[0]==-10,"FG distance must use the applied logical direction");
  motor_host::now=1300; bot::motorWrite(0,300,cfg);
  check(motor_host::duties[0]==0,"remaining filtered speed must inhibit reversal");
  for(int i=0;i<100;++i) {
    motor_host::now+=10; bot::motorSample(0.01f,cfg,pulses,speed,rpm);
  }
  bot::motorWrite(0,300,cfg);
  check(motor_host::duties[0]==300 && motor_host::pinLevels[12]==HIGH,"reversal must resume after pulse silence and speed decay");
  bot::motorStop(); allStopped();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  check(motor_host::pinWrites==0,"core 3 must write channels through ledcWriteChannel, not GPIO-based ledcWrite");
  for(int failedChannel=0;failedChannel<4;++failedChannel) {
    motor_host::failWriteChannel=-1;bot::hardwareBegin(cfg);
    for(int i=0;i<4;++i)bot::motorWrite(i,500,cfg);
    motor_host::failWriteChannel=failedChannel;bot::motorStop();
    check(!bot::hardwareOutputsHealthy(),"failed zero write must close output admission");
    uint16_t reported[4];bot::motorOutputs(reported);
    for(int i=0;i<4;++i) {
      if(i==failedChannel) {
        check(motor_host::duties[i]==500,"failed stop must not pretend physical channel is zero");
        check(reported[i]==500,"failed zero write must retain last confirmed software duty");
      } else {
        check(motor_host::duties[i]==0&&reported[i]==0,"stop must still attempt all remaining channels");
      }
    }
    const auto requests=motor_host::nonzeroMotorWrites;
    for(int i=0;i<4;++i)bot::motorWrite(i,700,cfg);
    check(motor_host::nonzeroMotorWrites==requests,"zero-write failure must reject later output requests");
    motor_host::failWriteChannel=-1;bot::motorStop();allStopped();
    check(!bot::hardwareOutputsHealthy(),"successful retry must not silently reopen fault admission");
  }
  bot::hardwareBegin(cfg);
  for(int i=0;i<4;++i) bot::motorWrite(i,500,cfg);
  motor_host::failWriteChannel=2;bot::motorWrite(2,600,cfg);
  check(!bot::hardwareOutputsHealthy(),"runtime PWM failure must close output admission");
  for(int i=0;i<4;++i) if(i!=2) check(motor_host::duties[i]==0,"available channels must stop after one runtime failure");
  uint16_t reported[4];bot::motorOutputs(reported);
  check(reported[2]==500&&motor_host::duties[2]==500,"failed nonzero write must not replace last confirmed request");
  const auto before=motor_host::nonzeroMotorWrites;
  for(int i=0;i<4;++i) bot::motorWrite(i,500,cfg);
  check(motor_host::nonzeroMotorWrites==before,"closed admission must refuse subsequent motor commands");
#endif
  std::cout<<"PASS core "<<ESP_ARDUINO_VERSION_MAJOR<<": motor / servo / stop / reversal\n";
}
