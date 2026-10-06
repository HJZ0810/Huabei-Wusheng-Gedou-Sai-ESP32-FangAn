/**
 * @file    command_gateway.cpp
 * @brief   将不同传输来源转换为同一种控制请求。
 *
 * 数据流：已认证消息 → 严格类型及范围检查 → 唯一控制器。
 * 云端限制：解锁、标定、配置和重启保留在本地；期限使用设备单调时钟。
 */
#include "command_gateway.h"
#include "config.h"
#include "controller.h"
#include <cmath>
#include <cstring>

namespace bot {
namespace {
bool numeric(JsonVariantConst v,float& value,float lo,float hi) {
  if(v.is<bool>() || !(v.is<float>() || v.is<double>() || v.is<int>() || v.is<uint32_t>())) return false;
  value=v.as<float>(); return std::isfinite(value) && value>=lo && value<=hi;
}
}
bool commandDispatch(JsonVariantConst body,uint32_t client,String& error,bool cloud,uint32_t expiresAt) {
  if(!body.is<JsonObjectConst>() || !client) { error="指令或控制会话无效"; return false; }
  const char* type=body["t"] | ""; if(!*type) type=body["type"] | "";
  const Config c=configSnapshot(); Command cmd;
  cmd.client=client; cmd.speed=c.defaultSpeed; cmd.receivedMs=millis();
  cmd.expires=cloud; cmd.expiresAt=expiresAt;
  if(cloud && static_cast<int32_t>(millis()-expiresAt)>=0) { error="云端指令已过期"; return false; }
  if(cloud && (!strcmp(type,"unlock") || !strcmp(type,"cal"))) { error="此操作仅允许本地执行"; return false; }
  if(!strcmp(type,"stop")) cmd.type=CommandType::Stop;
  else if(!strcmp(type,"estop")) cmd.type=CommandType::Estop;
  else if(!strcmp(type,"takeover")) cmd.type=CommandType::Takeover;
  else if(!strcmp(type,"unlock")) cmd.type=CommandType::Unlock;
  else if(!strcmp(type,"hb")) {
    // 自主任务拥有独立有限预算；浏览器心跳只证明操作者仍在线。
    if(controllerSnapshot().owner==AutonomousOwner) { error=""; return true; }
    cmd.type=CommandType::Heartbeat;
  } else if(!strcmp(type,"drv")) {
    cmd.type=CommandType::Drive; float percent=0;
    if(!numeric(body["x"],cmd.x,-1,1) || !numeric(body["y"],cmd.y,-1,1) || !numeric(body["spd"],percent,0,100)) {
      error="摇杆及速度百分比无效"; return false;
    }
    cmd.speed=c.maxSpeed*percent/100;
    const float radius=std::sqrt(cmd.x*cmd.x+cmd.y*cmd.y);
    if(radius<=0.08f) cmd.x=cmd.y=0;
    else { const float scale=std::pow((std::fmin(radius,1.0f)-0.08f)/0.92f,1.5f)/radius; cmd.x*=scale; cmd.y*=scale; }
  } else if(!strcmp(type,"move")) {
    cmd.type=CommandType::Move;
    if(!numeric(body["dist"],cmd.value,-1000,1000) || std::fabs(cmd.value)<0.5f) { error="距离须为±0.5～1000cm"; return false; }
  } else if(!strcmp(type,"turn")) {
    cmd.type=CommandType::Turn;
    if(!numeric(body["deg"],cmd.value,-1440,1440) || std::fabs(cmd.value)<1) { error="转角须为±1～1440°"; return false; }
  } else if(!strcmp(type,"servo")) {
    cmd.type=CommandType::Servo;
    if(!numeric(body["pos"],cmd.value,0,1)) { error="舵机位置须为0～1"; return false; }
  } else if(!strcmp(type,"pose") || !strcmp(type,"goto")) {
    cmd.type=!strcmp(type,"pose")?CommandType::Pose:CommandType::Navigate;
    const float half=c.arenaOuterCm/2;
    if(!numeric(body["x"],cmd.x,-half,half) || !numeric(body["y"],cmd.y,-half,half)) { error="目标坐标须在场地范围内"; return false; }
    if(cmd.type==CommandType::Pose) {
      if(!numeric(body["heading"],cmd.value,-360,360)) { error="起点航向无效"; return false; }
      const char* floor=body["floor"] | "";
      if(strcmp(floor,"lower") && strcmp(floor,"upper")) { error="须明确选择低层或高台"; return false; }
      cmd.upper=!strcmp(floor,"upper");
    }
  } else if(!strcmp(type,"auto")) cmd.type=CommandType::Battle;
  else if(!strcmp(type,"climb")) cmd.type=CommandType::Climb;
  else if(!strcmp(type,"cal")) {
    const char* mode=body["mode"] | "";
    if(!strcmp(mode,"imu")) cmd.type=CommandType::ImuCal;
    else if(!strcmp(mode,"odo")) { cmd.type=CommandType::OdoCalStart; cmd.value=100; }
    else if(!strcmp(mode,"turn")) { cmd.type=CommandType::TurnCalStart; cmd.value=360; }
    else { error="未知标定类型"; return false; }
  } else { error="未知指令"; return false; }
  if(!controllerEnqueue(cmd)) { error="控制忙、其他操作者占用或安全锁定"; return false; }
  error=""; return true;
}
}
