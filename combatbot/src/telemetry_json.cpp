/**
 * @file    telemetry_json.cpp
 * @brief   本地与云端共享的只读遥测转换。
 *
 * 数据语义：无效量测输出 null；地图位置是估计值，误差规模不是实测置信区间。
 * 安全边界：仅输出白名单字段，WiFi 密码、设备密钥与操作者凭据不参与序列化。
 */
#include "telemetry_json.h"
#include "controller.h"
#include "config.h"
#include "wifi_mgr.h"
#include "cloud_client.h"
#include "safety.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
namespace bot {
void telemetryJson(JsonObject doc) {
  const auto tm=controllerSnapshot(); const auto cfg=configSnapshot();
  doc["t"]="tm"; doc["firmware"]=FirmwareVersion;
  doc["configRevision"]=cfg.revision;
  doc["developmentMode"]=cfg.developmentMode;
  doc["spd"]=tm.speed; doc["yaw"]=tm.yaw; doc["odo"]=tm.odo;
  if(tm.sensors.batteryValid) doc["bat"]=tm.sensors.batteryV; else doc["bat"]=nullptr;
  auto rpm=doc["rpm"].to<JsonArray>(); auto pulse=doc["pulses"].to<JsonArray>(); auto pwm=doc["pwm"].to<JsonArray>();
  for(int i=0;i<4;++i) { rpm.add(tm.rpm[i]); pulse.add(tm.pulses[i]); pwm.add(tm.pwm[i]); }
  auto ir=doc["ir"].to<JsonArray>(); auto valid=doc["irValid"].to<JsonArray>();
  for(int i=0;i<cfg.irCount;++i) { if(tm.sensors.irValid[i]) ir.add(tm.sensors.ir[i]); else ir.add(nullptr); valid.add(tm.sensors.irValid[i]); }
  auto gr=doc["gr"].to<JsonArray>(); for(bool v:tm.sensors.gray) gr.add(v);
  auto e18=doc["e18"].to<JsonArray>(); for(bool v:tm.sensors.e18) e18.add(v);
  auto acc=doc["acc"].to<JsonArray>(); for(float v:tm.sensors.acc) acc.add(v);
  doc["accelSource"]=tm.sensors.accelSource; doc["accelSaturated"]=tm.sensors.accelSaturated;
  doc["impact"]=tm.sensors.impact; doc["impactG"]=tm.sensors.impactG;
  doc["irLimited"]=tm.irLimited; doc["irUnavailable"]=tm.irUnavailable; doc["irSpeedScale"]=tm.irSpeedScale;
  const uint32_t now=millis();
  doc["imuOk"]=tm.sensors.imuOk && !timedOut(now,tm.sensors.imuMs,80);
  doc["accelOk"]=tm.sensors.accelOk;
  doc["ioOk"]=tm.sensors.ioOk && !timedOut(now,tm.sensors.digitalMs,80);
  doc["net"]=networkMode(); doc["rssi"]=networkRssi(); doc["apIp"]=apIp(); doc["staIp"]=staIp();
  doc["ip"]=staIp().length()?staIp():apIp(); doc["mdns"]=String(cfg.hostname)+".local";
  doc["st"]=tm.state; doc["fault"]=tm.fault; doc["estop"]=tm.estop; doc["lowBattery"]=tm.lowBattery;
  doc["owner"]=tm.owner; doc["progress"]=tm.progress*100; doc["imuCalibrating"]=tm.imuCalibrating;
  doc["resultReady"]=tm.resultReady; doc["resultId"]=tm.resultId; doc["resultType"]=tm.resultType;
  doc["resultTarget"]=tm.target; doc["resultActual"]=tm.actual; doc["resultError"]=tm.error;
  doc["resultConfigRevision"]=tm.resultConfigRevision;
  doc["calibrationSessionId"]=tm.calibrationSessionId;
  doc["calibrationPulses"]=tm.calibrationPulses; doc["calibrationTurnFactor"]=tm.calibrationTurnFactor;
  doc["calibrationMaxTiltDeg"]=tm.calibrationMaxTiltDeg;
  doc["calibrationDurationMs"]=tm.calibrationDurationMs;
  doc["calibrationSupportChanges"]=tm.calibrationSupportChanges;
  doc["cloud"]=cloudState(); doc["freeHeap"]=ESP.getFreeHeap(); doc["minFreeHeap"]=ESP.getMinFreeHeap();
  doc["largestFreeBlock"]=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  auto a=doc["arena"].to<JsonObject>(); const auto& p=tm.arena.pose;
  if(p.valid) { a["x"]=p.xCm; a["y"]=p.yCm; a["heading"]=p.headingDeg; a["uncertainty"]=p.uncertaintyCm; }
  else { a["x"]=nullptr; a["y"]=nullptr; a["heading"]=nullptr; a["uncertainty"]=nullptr; }
  a["quality"]=!p.valid?"unknown":p.uncertaintyCm<=5?"high":p.uncertaintyCm<=15?"medium":"low";
  const char* layer=p.layer==arena::Layer::UpperAnchored?"upper":p.layer==arena::Layer::UpperEstimated?"upper_estimated":
    p.layer==arena::Layer::Lower?"lower":p.layer==arena::Layer::TransitionUnknown?"transition":"unknown";
  a["floor"]=layer; a["mode"]=arena::modeName(tm.arena.mode); a["phase"]=arena::phaseName(tm.arena.phase);
  a["reason"]=arena::reasonName(tm.arena.reason); a["active"]=tm.owner==AutonomousOwner;
  a["outerSize"]=cfg.arenaOuterCm; a["platformSize"]=cfg.arenaPlatformCm;
  a["entryX"]=cfg.arenaEntryX; a["entryY"]=cfg.arenaEntryY;
  a["targetValid"]=tm.arena.target.valid; a["targetBearing"]=tm.arena.target.bearingDeg; a["targetDistance"]=tm.arena.target.distanceCm;
  a["climbEstimated"]=tm.arena.climbEstimated;
  if(tm.arena.hasGoal) { a["targetX"]=tm.arena.goalX; a["targetY"]=tm.arena.goalY; }
  else { a["targetX"]=nullptr; a["targetY"]=nullptr; }
  a["routeValid"]=tm.arena.routeValid;
  a["routeSource"]=tm.arena.routeValid?"estimated_pose":"none";
  a["pathCount"]=tm.arena.routeValid?tm.arena.routeCount:0;
  auto route=a["path"].to<JsonArray>();
  for(uint8_t i=0;i<tm.arena.routeCount && i<arena::MaxRoutePoints;++i) {
    JsonObject point=route.add<JsonObject>();
    point["x"]=tm.arena.route[i].xCm; point["y"]=tm.arena.route[i].yCm;
  }
}
}
