/**
 * @file    cloud_client.cpp
 * @brief   独立网络任务中的设备认证、许可轮换与遥测上传。
 *
 * 接入条件：STA 已连接、配置完整、时间已同步、CA 校验开启。
 * 消息约定：启动/连接/租约代次匹配，序号递增，使用设备签发的短期许可。
 * 失联行为：清远控租约并停止该人工来源；已由车端受理的自主任务继续运行。
 */
#include "cloud_client.h"
#include "cloud_protocol.h"
#include "command_gateway.h"
#include "config.h"
#include "controller.h"
#include "telemetry_json.h"
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <time.h>

namespace bot {
namespace {
enum Status { Disabled, WaitingSta, WaitingTime, BadCa, Connecting, Online, Disconnected, TaskFailed };
std::atomic<unsigned> status{Disabled};
WebSocketsClient socket;
CloudProtocol protocol;
Config settings;
char boot[33]={0}; String headers;
bool configured=false, connected=false, ready=false;
uint32_t lastPermit=0,lastTelemetry=0;

/** @brief 使用芯片随机源生成128位十六进制代次与许可；不写日志。 */
void randomToken(char token[33]) {
  for(int i=0;i<4;++i) snprintf(token+i*8,9,"%08lx",static_cast<unsigned long>(esp_random()));
}
bool send(JsonDocument& doc) { String text; serializeJson(doc,text); return socket.sendTXT(text); }
void permit(bool hello) {
  char token[33]; randomToken(token); lastPermit=millis(); protocol.issue(token,lastPermit);
  JsonDocument doc; doc["t"]=hello?"device_hello":"permit";
  doc["boot"]=protocol.boot(); doc["link"]=protocol.link(); doc["permit"]=token;
  doc["permitTtlMs"]=static_cast<uint32_t>(CloudProtocol::PermitMs); send(doc);
}
void clearRemote() {
  connected=ready=false; protocol.reset(); controllerDisconnect(CloudOwner);
}
void event(WStype_t type,uint8_t* payload,size_t length) {
  if(type==WStype_CONNECTED) {
    char link[33]; randomToken(link); protocol.begin(boot,link);
    connected=true; ready=false; permit(true); return;
  }
  if(type==WStype_DISCONNECTED || type==WStype_ERROR) { clearRemote(); status.store(Disconnected); return; }
  if(type==WStype_FRAGMENT_TEXT_START || type==WStype_FRAGMENT_BIN_START || type==WStype_FRAGMENT || type==WStype_FRAGMENT_FIN || type==WStype_BIN) {
    socket.disconnect(); clearRemote(); return;
  }
  if(type!=WStype_TEXT) return;
  if(length>2048) { socket.disconnect(); clearRemote(); return; }
  JsonDocument doc;
  if(deserializeJson(doc,payload,length) || !doc.is<JsonObject>()) { socket.disconnect(); clearRemote(); return; }
  const char* kind=doc["t"] | "";
  const char* epochBoot=doc["boot"] | ""; const char* epochLink=doc["link"] | "";
  if(!strcmp(kind,"ready")) {
    if(protocol.epoch(epochBoot,epochLink)) { ready=true; status.store(Online); }
    return;
  }
  if(!strcmp(kind,"lease")) {
    const char* lease=doc["lease"] | "";
    if(!protocol.epoch(epochBoot,epochLink)) return;
    const bool changed=strcmp(lease,protocol.lease())!=0;
    if(protocol.setLease(epochBoot,epochLink,lease) && changed) controllerDisconnect(CloudOwner);
    return;
  }
  if(!strcmp(kind,"lease_end")) {
    if(protocol.endLease(epochBoot,epochLink,doc["lease"] | "")) controllerDisconnect(CloudOwner);
    return;
  }
  if(strcmp(kind,"command") || !ready) return;
  const char* lease=doc["lease"] | ""; const char* token=doc["permit"] | "";
  uint32_t deadline=0; String error;
  const bool valid=doc["seq"].is<uint32_t>() && protocol.accept(epochBoot,epochLink,lease,token,doc["seq"].as<uint32_t>(),millis(),deadline);
  const bool ok=valid && commandDispatch(doc["command"].as<JsonVariantConst>(),CloudOwner,error,true,deadline);
  JsonDocument ack; ack["t"]="ack"; ack["lease"]=lease; ack["seq"]=doc["seq"];
  ack["ok"]=ok; ack["message"]=valid?(ok?"指令已受理":error.c_str()):"旧租约、重放或过期许可";
  send(ack);
}
void run(void*) {
  uint32_t revision=0; bool timeRequested=false;
  for(;;) {
    const Config current=configSnapshot();
    if(revision!=current.revision) {
      socket.disconnect(); clearRemote(); settings=current; revision=current.revision; configured=false;
    }
    if(!settings.cloudEnabled || !settings.staEnabled) { status.store(Disabled); vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    if(WiFi.status()!=WL_CONNECTED) {
      if(configured) socket.disconnect();
      clearRemote(); configured=false;
      status.store(WaitingSta); vTaskDelay(pdMS_TO_TICKS(200)); continue;
    }
    if(!strstr(settings.cloudCaPem,"-----BEGIN CERTIFICATE-----") || !strstr(settings.cloudCaPem,"-----END CERTIFICATE-----")) {
      status.store(BadCa); vTaskDelay(pdMS_TO_TICKS(500)); continue;
    }
    if(time(nullptr)<1700000000) {
      if(!timeRequested) { configTime(0,0,"pool.ntp.org","time.cloudflare.com"); timeRequested=true; }
      status.store(WaitingTime); vTaskDelay(pdMS_TO_TICKS(200)); continue;
    }
    if(!configured) {
      headers=String("X-CombatBot-Device: ")+settings.cloudDeviceId+"\r\nX-CombatBot-Key: "+settings.cloudDeviceKey+"\r\n";
      socket.beginSslWithCA(settings.cloudHost,settings.cloudPort,settings.cloudPath,settings.cloudCaPem,"");
      socket.setExtraHeaders(headers.c_str()); socket.onEvent(event);
      socket.setReconnectInterval(5000); socket.enableHeartbeat(5000,2000,2);
      configured=true; status.store(Connecting);
    }
    socket.loop();
    const uint32_t now=millis();
    if(connected && ready) {
      if(uint32_t(now-lastPermit)>=250) permit(false);
      if(uint32_t(now-lastTelemetry)>=1000U/static_cast<uint32_t>(settings.telemetryHz)) {
        lastTelemetry=now; JsonDocument doc; telemetryJson(doc.to<JsonObject>());
        if(!send(doc)) { socket.disconnect(); clearRemote(); }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
}
void cloudBegin() {
  randomToken(boot);
  if(xTaskCreatePinnedToCore(run,"cloud",20480,nullptr,1,nullptr,0)!=pdPASS) status.store(TaskFailed);
}
const char* cloudState() {
  static const char* names[]={"disabled","waiting_sta","waiting_time","invalid_ca","connecting","online","disconnected","task_failed"};
  return names[status.load()];
}
}
