/**
 * @file test_gateway.cpp
 * @brief 使用真实 ArduinoJson 与生产入口验证类型、权限和设备时钟期限。
 * @note 控制器仅记录投递内容；调度和电机效果由控制器集成测试覆盖。
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include "command_gateway.h"
#include "config.h"
#include "controller.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
uint32_t hostMillis=1000;
static bot::Config cfg;
static bot::Command captured;
static bot::Telemetry telemetry;
static bool acceptQueue=true;
static unsigned dispatches=0;
namespace bot {
Config configSnapshot(){return cfg;}
bool controllerEnqueue(const Command& c){captured=c;++dispatches;return acceptQueue;}
Telemetry controllerSnapshot(){return telemetry;}
}
#include "../src/command_gateway.cpp"
static bool dispatch(const char* json,bool cloud=false,uint32_t expires=1500,uint32_t client=7){
  JsonDocument doc;assert(!deserializeJson(doc,json));String error;
  const bool ok=bot::commandDispatch(doc.as<JsonVariantConst>(),client,error,cloud,expires);
  if(!ok) { assert(!error.empty()); }
  return ok;
}
int main(){
  using bot::CommandType;
  for(const char* bad:{R"([])",R"(null)",R"({})",R"({"t":12})",R"({"t":"unknown"})",
    R"({"t":"drv","x":"0.5","y":1,"spd":40})",R"({"t":"drv","x":true,"y":1,"spd":40})",
    R"({"t":"drv","x":0,"y":null,"spd":40})",R"({"t":"drv","x":0,"y":1,"spd":101})",
    R"({"t":"drv","x":1.01,"y":0,"spd":40})",R"({"t":"move","dist":0.49})",
    R"({"t":"move","dist":1001})",R"({"t":"turn","deg":0.9})",R"({"t":"turn","deg":1441})",
    R"({"t":"servo","pos":-0.01})",R"({"t":"goto","x":191,"y":0})",
    R"({"t":"pose","x":0,"y":0,"heading":361,"floor":"lower"})",
    R"({"t":"pose","x":0,"y":0,"heading":0,"floor":"upper_estimated"})",
    R"({"t":"pose","x":0,"y":0,"heading":0,"floor":true})",R"({"t":"cal","mode":"unknown"})"}){
    const auto before=dispatches;assert(!dispatch(bad));assert(dispatches==before);
  }
  assert(!dispatch(R"({"t":"stop"})",false,1500,0));
  assert(dispatch(R"({"t":"drv","x":0.04,"y":0.04,"spd":40})"));
  assert(captured.type==CommandType::Drive&&captured.x==0&&captured.y==0&&captured.speed==40);
  assert(dispatch(R"({"t":"drv","x":1,"y":1,"spd":100})"));
  assert(std::fabs(std::hypot(captured.x,captured.y)-1)<0.0001f);
  assert(dispatch(R"({"t":"move","dist":-1000})"));assert(captured.type==CommandType::Move&&captured.value==-1000);
  assert(dispatch(R"({"t":"turn","deg":1440})"));assert(captured.type==CommandType::Turn);
  assert(dispatch(R"({"t":"pose","x":-150,"y":0,"heading":90,"floor":"lower"})"));
  assert(captured.type==CommandType::Pose&&!captured.upper&&captured.value==90);
  assert(dispatch(R"({"t":"pose","x":0,"y":0,"heading":0,"floor":"upper"})"));assert(captured.upper);
  assert(dispatch(R"({"t":"goto","x":100,"y":40})"));assert(captured.type==CommandType::Navigate);
  assert(dispatch(R"({"t":"auto"})"));assert(captured.type==CommandType::Battle);
  assert(dispatch(R"({"t":"climb"})"));assert(captured.type==CommandType::Climb);
  assert(dispatch(R"({"t":"takeover"})"));assert(captured.type==CommandType::Takeover);
  assert(dispatch(R"({"t":"cal","mode":"imu"})"));assert(captured.type==CommandType::ImuCal);
  assert(dispatch(R"({"t":"cal","mode":"odo"})"));assert(captured.type==CommandType::OdoCalStart);
  assert(dispatch(R"({"t":"cal","mode":"turn"})"));assert(captured.type==CommandType::TurnCalStart);
  assert(dispatch(R"({"t":"cal","mode":"turn_left"})"));assert(captured.type==CommandType::TurnCalStart&&captured.directionalCalibration&&captured.value==360);
  assert(dispatch(R"({"t":"cal","mode":"turn_right"})"));assert(captured.type==CommandType::TurnCalStart&&captured.directionalCalibration&&captured.value==-360);
  assert(dispatch(R"({"t":"cal","mode":"climb_observe"})"));assert(captured.type==CommandType::ClimbObserveStart);
  assert(dispatch(R"({"t":"cal","mode":"climb_observe_end"})"));assert(captured.type==CommandType::ClimbObserveEnd);
  for(const float value:{NAN,INFINITY,-INFINITY}){
    JsonDocument doc;doc["t"]="move";doc["dist"]=value;String error;const auto before=dispatches;
    assert(!bot::commandDispatch(doc.as<JsonVariantConst>(),7,error));assert(dispatches==before);
  }
  // 云端期限来自设备单调时钟，入口不把到期指令重签为新指令。
  assert(!dispatch(R"({"t":"move","dist":30})",true,hostMillis));
  assert(!dispatch(R"({"t":"stop"})",true,hostMillis-1));
  assert(!dispatch(R"({"t":"unlock"})",true));assert(!dispatch(R"({"t":"cal","mode":"imu"})",true));
  assert(!dispatch(R"({"t":"cal","mode":"turn_left"})",true));
  assert(!dispatch(R"({"t":"cal","mode":"turn_right"})",true));
  assert(!dispatch(R"({"t":"cal","mode":"climb_observe"})",true));
  assert(!dispatch(R"({"t":"cal","mode":"climb_observe_end"})",true));
  for(const char* json:{R"({"t":"goto","x":50,"y":0})",R"({"t":"auto"})",R"({"t":"climb"})",R"({"t":"takeover"})",R"({"t":"estop"})"}){
    assert(dispatch(json,true,hostMillis+200));assert(captured.expires&&captured.expiresAt==hostMillis+200&&captured.client==7);
  }
  hostMillis=0xFFFFFFF0u;assert(dispatch(R"({"t":"move","dist":30})",true,30));assert(captured.expiresAt==30);
  acceptQueue=false;assert(!dispatch(R"({"t":"move","dist":30})"));acceptQueue=true;
  telemetry.owner=bot::AutonomousOwner;const auto before=dispatches;assert(dispatch(R"({"t":"hb"})"));assert(dispatches==before);
  std::puts("PASS: real gateway JSON types / finite bounds / aliases / local calibration / cloud permissions / monotonic expiry and rollover / autonomous heartbeat");
}
