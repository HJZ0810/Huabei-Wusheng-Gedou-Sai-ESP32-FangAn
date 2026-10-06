/**
 * @file    web.cpp
 * @brief   实现离线网页、控制协议、配置保存与标定结果应用接口。
 *
 * ============================================================================
 * 模块职责：把网络输入转换为控制指令，并将设备快照序列化为网页可用的 JSON。
 * 运行约定：HTTP / WebSocket 在异步回调中受理，遥测和延迟重启由主循环调度。
 * 设计边界：ACK 只确认入队受理；随机会话凭据绑定 HTTP 与 WebSocket；连接凭据不提供用户身份认证。
 * ============================================================================
 */
#include "web.h"
#include "config.h"
#include "controller.h"
#include "wifi_mgr.h"
#include "web_asset.h"
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <cmath>
#include <atomic>
#include <esp_system.h>
#include <esp_random.h>
#include "session_registry.h"
#include "command_gateway.h"
#include "telemetry_json.h"
#include "calibration_model.h"

namespace bot {
namespace {
// ============================================================================
// 服务对象与会话状态 · 固件内嵌网页通过同源连接访问 /ws
// ============================================================================

AsyncWebServer server(80);
AsyncWebSocket socket("/ws");
// editing 通过原子操作仲裁配置与标定应用；重启状态独立使用原子发布。
// 会话表仅在 sessionMux 临界区访问；遥测发送状态由主循环独占。
std::atomic<bool> editing{false};
// appliedCalibration 记录本次启动会话已应用的结果编号，防止相同结果重复换算。
std::atomic<uint32_t> restartAt{0};
std::atomic<bool> restartPending{false};
uint32_t lastTelemetry=0, lastResult=0;
// appliedCalibration 只在 editing 仲裁成功后的配置操作中读写。
uint32_t appliedCalibration=0;
SessionRegistry sessions;
portMUX_TYPE sessionMux=portMUX_INITIALIZER_UNLOCKED;

/** @brief 浏览器跨站请求不得取得会话；无 Origin 兼容本地非浏览器工具。 */
bool sameOrigin(AsyncWebServerRequest* request) {
  if (!request || !request->hasHeader("Origin")) return true;
  const String origin=request->getHeader("Origin")->value();
  return origin == String("http://") + request->host();
}

/** @brief 从随机凭据解析连接编号，不接受网页提供的 owner 编号作为凭据。 */
uint32_t sessionClient(AsyncWebServerRequest* request) {
  if (!sameOrigin(request) || !request->hasHeader("X-CombatBot-Session")) return 0;
  const String token=request->getHeader("X-CombatBot-Session")->value();
  portENTER_CRITICAL(&sessionMux);
  const uint32_t client=sessions.find(token.c_str());
  portEXIT_CRITICAL(&sessionMux);
  return client;
}

/** @brief 生成 128 位随机凭据并绑定连接；凭据只在 hello 中单播。 */
bool openSession(uint32_t client,char (&token)[33]) {
  if(!client || client>=AutonomousOwner) return false;
  uint8_t random[16];
  esp_fill_random(random,sizeof(random));
  static const char hex[]="0123456789abcdef";
  for(size_t i=0;i<sizeof(random);++i) {
    token[i*2]=hex[random[i]>>4];
    token[i*2+1]=hex[random[i]&15];
  }
  token[32]='\0';
  portENTER_CRITICAL(&sessionMux);
  const bool registered=sessions.add(client,token);
  portEXIT_CRITICAL(&sessionMux);
  return registered;
}

// ============================================================================
// 协议基础设施 · 类型校验及统一响应
// ============================================================================

/**
 * @brief   读取有限数值，并检查闭区间范围。
 * @param v 待校验的 JSON 字段；字符串形式的数字不作为有效数值接收。
 * @param out 数值输出；返回 false 时调用方不得使用其内容。
 * @param lo 允许的最小值，包含边界。
 * @param hi 允许的最大值，包含边界。
 * @return 类型、有限性及范围均符合约定时返回 true。
 */
bool number(JsonVariantConst v, float& out, float lo, float hi) {
  if (!v.is<float>() && !v.is<int>() && !v.is<long>()) return false;
  out=v.as<float>();
  return std::isfinite(out) && out>=lo && out<=hi;
}
/**
 * @brief   向指定 WebSocket 连接发送类型与文字消息。
 * @param client 当前服务器回调提供的目标连接。
 * @param type 消息类型，如 error。
 * @param message 提供给客户端展示的说明。
 */
void reply(AsyncWebSocketClient* client, const char* type, const String& message) {
  JsonDocument doc; doc["t"]=type; doc["message"]=message;
  String data; serializeJson(doc,data); client->text(data);
}
/**
 * @brief   以统一的 ok / message 格式结束 HTTP 请求。
 * @param req 待响应的请求。
 * @param status HTTP 状态码；202 表示受理，不能据此判断动作已经完成。
 * @param ok 当前请求阶段是否成功。
 * @param message 成功说明或拒绝原因。
 */
void response(AsyncWebServerRequest* req, int status, bool ok, const String& message) {
  JsonDocument doc; doc["ok"]=ok; doc["message"]=message;
  String data; serializeJson(doc,data); req->send(status,"application/json",data);
}

// ============================================================================
// 控制指令入口 · 解析、边界检查与控制器入队
// ============================================================================

/**
 * @brief   校验网络指令并交由控制器受理。
 * @param body JSON 指令对象，优先读取 t，兼容 type 字段。
 * @param client 指令关联的 WebSocket 连接编号；普通运动要求连接仍然存在。
 * @param error 拒绝时写入可展示的原因。
 * @return 成功进入控制器受理流程返回 true；不代表动作执行成功。
 * @details stop、estop、unlock 不要求控制权；其安全条件仍由控制器判断。
 *          REST 连接编号从请求头的随机会话凭据解析，不信任 body.client。
 */
bool dispatch(JsonVariantConst body, uint32_t client, String& error) {
  if(!body.is<JsonObjectConst>() || !client || socket.client(client)==nullptr) {
    error="请先建立有效控制会话"; return false;
  }
  const char* type=body["t"] | "";
  if(!*type) type=body["type"] | "";
  const bool stop=!strcmp(type,"stop") || !strcmp(type,"estop") || !strcmp(type,"takeover");
  if(!stop && editing.load()) { error="配置正在保存，请稍后操作"; return false; }
  if(!stop && restartPending.load(std::memory_order_acquire)) { error="设备即将重启"; return false; }
  return commandDispatch(body,client,error);
}

/**
 * @brief   处理连接握手、断开通知与完整文本指令。
 * @param client 触发事件的连接，握手时向网页返回其连接编号。
 * @param event 连接、断开、错误或数据事件类型。
 * @param arg 连接事件中为请求对象；数据事件中为 AwsFrameInfo 帧边界信息。
 * @param data 数据缓冲区；按 len 读取，不要求以空字符结束。
 * @param len 本次缓冲区的有效字节数。
 * @details ACK 只表示指令受理；完成状态应读取 tm / done 消息。
 */
void websocketEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType event,
                    void* arg,uint8_t* data,size_t len) {
  if (event==WS_EVT_CONNECT) {
    auto* request=static_cast<AsyncWebServerRequest*>(arg);
    char token[33];
    if (!sameOrigin(request) || !openSession(client->id(),token)) {client->close();return;}
    JsonDocument doc; doc["t"]="hello";doc["client"]=client->id();doc["session"]=token;
    doc["firmware"]=FirmwareVersion;
    String out;serializeJson(doc,out);client->text(out);
  } else if (event==WS_EVT_DISCONNECT || event==WS_EVT_ERROR) {
    portENTER_CRITICAL(&sessionMux);
    sessions.remove(client->id());
    portEXIT_CRITICAL(&sessionMux);
    controllerDisconnect(client->id());
  } else if (event==WS_EVT_DATA) {
    portENTER_CRITICAL(&sessionMux);
    const bool validSession=sessions.contains(client->id());
    portEXIT_CRITICAL(&sessionMux);
    if(!validSession){client->close();return;}
    const auto* frame=static_cast<AwsFrameInfo*>(arg);
    // 仅接收不超过 2048 字节的完整文本帧，避免把分片误当成完整 JSON。
    // data 不保证以空字符结束，因此解析时显式传入 len。
    if (!frame->final || frame->index!=0 || frame->len!=len || frame->opcode!=WS_TEXT || len>2048) {
      reply(client,"error","不支持分片、二进制或过长指令");return;
    }
    JsonDocument doc;
    if (deserializeJson(doc,data,len)) {reply(client,"error","JSON解析失败");return;}
    String error;
    if (!dispatch(doc.as<JsonVariantConst>(),client->id(),error)) reply(client,"error",error);
    else if (strcmp(doc["t"] | "","hb")) {
      JsonDocument ack;ack["t"]="ack";ack["command"]=doc["t"];
      String out;serializeJson(ack,out);client->text(out);
    }
  }
}

// ============================================================================
// REST 公共包装 · JSON 入口与配置保存仲裁
// ============================================================================

/**
 * @brief   注册仅接受 JSON 对象的 POST 路由。
 * @param path 路由地址。
 * @param callback 校验对象类型后调用的业务处理器。
 * @details 请求体上限为 8192 字节，先校验随机会话及连接编号，再交由业务校验。
 */
template<class Callback> void jsonRoute(const char* path,Callback callback) {
  auto* handler=new AsyncCallbackJsonWebHandler(path,
    [callback](AsyncWebServerRequest* req, JsonVariant& data) {
      if (!data.is<JsonObject>()) {response(req,400,false,"请求必须是JSON对象");return;}
      const uint32_t client=sessionClient(req);
      if (!client) {response(req,401,false,"会话无效，请重新连接设备");return;}
      if (!data["client"].isNull() && (!data["client"].is<uint32_t>() || data["client"].as<uint32_t>()!=client)) {
        response(req,403,false,"指令连接编号与会话不匹配");return;
      }
      callback(req,data.as<JsonVariantConst>());
    });
  handler->setMethod(HTTP_POST);
  handler->setMaxContentLength(8192);
  server.addHandler(handler);
}

/**
 * @brief   在车辆无动作且没有其他保存请求时执行配置操作。
 * @param req 待返回保存结果的 HTTP 请求。
 * @param operation 接收错误输出字符串并返回成功标志的配置操作。
 * @details editing 防止同时进入配置操作，不替代控制器及配置模块自己的同步。
 */
template<class Operation> void updateConfig(AsyncWebServerRequest* req, Operation operation) {
  if (editing.exchange(true)) {response(req,409,false,"配置保存忙");return;}
  if (restartPending.load(std::memory_order_acquire)) {editing=false;response(req,409,false,"设备即将重启");return;}
  if (!controllerTryBeginConfig()) {editing=false;response(req,409,false,"请先停止动作，或等待控制器空闲后保存配置");return;}
  // 保存窗口与真实控制器共享，不能仅靠网页侧 editing 加一次 busy 查询。
  struct ConfigRelease { ~ConfigRelease(){controllerEndConfig();} } release;
  String error;
  const bool ok=operation(error);
  editing=false;
  response(req,ok?200:400,ok,ok?"已保存；网络设置在重启后生效":error);
}

// ============================================================================
// 遥测发布 · 内部快照转换为网页协议，并推送新的完成结果
// ============================================================================

/**
 * @brief   向所有连接广播真实遥测及尚未发布的完成结果。
 * @details 无效电池 / 红外以 null 表示；不能用默认数值冒充有效测量。
 *          内部 progress 为 [0,1]，协议输出转换为 [0,100] 百分比。
 *          resultId 在本次启动会话内去重；后来连接的网页不会自动重播旧 done。
 */
void sendTelemetry() {
  const auto tm=controllerSnapshot();
  JsonDocument doc;
  telemetryJson(doc.to<JsonObject>());
  String out;serializeJson(doc,out);socket.textAll(out);
  if(tm.resultReady && tm.resultId!=lastResult){
    lastResult=tm.resultId;JsonDocument done;
    done["t"]="done";done["type"]=tm.resultType;done["target"]=tm.target;done["actual"]=tm.actual;
    done["err"]=tm.error;done["id"]=tm.resultId;done["configRevision"]=tm.resultConfigRevision;
    done["sessionId"]=tm.calibrationSessionId;
    done["calibrationPulses"]=tm.calibrationPulses;
    done["calibrationTurnFactor"]=tm.calibrationTurnFactor;
    done["calibrationMaxTiltDeg"]=tm.calibrationMaxTiltDeg;
    done["calibrationDurationMs"]=tm.calibrationDurationMs;
    done["calibrationSupportChanges"]=tm.calibrationSupportChanges;
    String result;serializeJson(done,result);socket.textAll(result);
  }
}
}

// ============================================================================
// 服务启动 · 网页资源、配置接口、动作接口与设备管理
// ============================================================================

/**
 * @brief   一次性注册路由并启动异步 HTTP 服务。
 * @details 首页直接发送固件内嵌的 gzip 资源，页面及配置响应禁止缓存。
 */
void webBegin() {
  socket.onEvent(websocketEvent);server.addHandler(&socket);
  server.on("/",HTTP_GET,[](AsyncWebServerRequest* req){
    // 显式发送二进制资源及长度：维护版采用当前 API，旧版保留 Flash 响应入口。
#if defined(ASYNCWEBSERVER_FORK_ESP32Async)
    auto* res=req->beginResponse(200,"text/html; charset=utf-8",WEB_GZIP,WEB_GZIP_SIZE);
#else
    auto* res=req->beginResponse_P(200,"text/html; charset=utf-8",WEB_GZIP,WEB_GZIP_SIZE);
#endif
    res->addHeader("Content-Encoding","gzip");res->addHeader("Cache-Control","no-store");
    res->addHeader("X-Content-Type-Options","nosniff");req->send(res);
  });
  server.on("/api/config",HTTP_GET,[](AsyncWebServerRequest* req){
    if (!sessionClient(req)) {response(req,401,false,"会话无效，请重新连接设备");return;}
    JsonDocument doc;configJson(configSnapshot(),doc.to<JsonObject>(),true);
    doc["cloudDeviceKey"]=""; // 写入后不回显密钥；省略字段即可保留。
    String out;serializeJson(doc,out);auto* res=req->beginResponse(200,"application/json",out);
    res->addHeader("Cache-Control","no-store");req->send(res);
  });
  jsonRoute("/api/config",[](AsyncWebServerRequest* req,JsonVariantConst body){
    updateConfig(req,[body](String& error){return configApply(body,error);});
  });
  jsonRoute("/api/config/defaults",[](AsyncWebServerRequest* req,JsonVariantConst){
    updateConfig(req,[](String& error){return configDefaults(error);});
  });
  jsonRoute("/api/config/clear-wifi",[](AsyncWebServerRequest* req,JsonVariantConst){
    updateConfig(req,[](String& error){return configClearWifi(error);});
  });
  jsonRoute("/api/action",[](AsyncWebServerRequest* req,JsonVariantConst body){
    const uint32_t client=sessionClient(req);String error;
    const bool ok=dispatch(body,client,error);response(req,ok?202:409,ok,ok?"指令已接收":error);
  });
  // 标定分两步：无 measured 时请求动作；有 measured 时应用外部测量。
  // IMU 零偏由控制器自行采样和保存，不经过行程 / 转角的外部测量换算。
  jsonRoute("/api/calibrate",[](AsyncWebServerRequest* req,JsonVariantConst body){
    const char* mode=body["mode"] | "";
    if (!strcmp(mode,"imu") || body["measured"].isNull()) {
      JsonDocument command;command["t"]="cal";command["mode"]=mode;
      String error;bool ok=dispatch(command.as<JsonVariantConst>(),sessionClient(req),error);
      response(req,ok?202:409,ok,ok?"标定动作已接收":error);return;
    }
    // 只允许应用最近的匹配结果，且该 resultId 在本次启动内最多应用一次。
    // 这里的编号是会话状态，重启后会重置，不能用作跨重启的标定历史。
    updateConfig(req,[body](String& error){
      const char* mode=body["mode"] | "";
      const auto tm=controllerSnapshot();float measured;
      const Config current=configSnapshot();
      Config cfg;
      // 检查与消费在同一保存仲裁范围内完成，避免并发应用同一结果。
      if(!body["id"].is<uint32_t>() || !body["sessionId"].is<uint32_t>() || !body["configRevision"].is<uint32_t>() ||
         !number(body["measured"],measured,1,1800) ||
         !prepareCalibration(current,tm,mode,body["id"].as<uint32_t>(),body["sessionId"].as<uint32_t>(),
           body["configRevision"].as<uint32_t>(),appliedCalibration,measured,cfg)){
        error="需先完成对应标定动作，再填写有效实测值；每次结果只能应用一次";return false;
      }
      if(!configSave(cfg,error))return false;
      appliedCalibration=tm.resultId;return true;
    });
  });
  // 先提交停车，再延迟 500 ms 重启，为 HTTP 响应发送留出时间。
  jsonRoute("/api/reboot",[](AsyncWebServerRequest* req,JsonVariantConst){
    if (editing.exchange(true)) {response(req,409,false,"配置保存忙");return;}
    if(restartPending.load(std::memory_order_acquire)) {
      editing=false;response(req,409,false,"设备已安排重启");return;
    }
    Command stop;stop.type=CommandType::Stop;stop.receivedMs=millis();controllerEnqueue(stop);
    restartAt.store(millis()+500,std::memory_order_relaxed);
    restartPending.store(true,std::memory_order_release);
    editing=false;response(req,202,true,"设备将重启，AP保持相同配置");
  });
  server.onNotFound([](AsyncWebServerRequest* req){
    if(req->url().startsWith("/api/")){response(req,404,false,"接口不存在");return;}
    req->redirect(String("http://")+apIp()+"/");
  });
  server.begin();
}

// ============================================================================
// 主循环调度 · 可配置频率遥测发布与延迟重启
// ============================================================================

/**
 * @brief   调度连接清理、遥测广播及到期重启。
 * @details telemetryHz 决定调度间隔，实际发送间隔受主循环负载影响；不是实时性保证。
 *          重启截止时间使用有符号差值比较，兼顾 millis() 回绕。
 */
void webTick() {
  const uint32_t now=millis();
  const uint32_t interval=1000U/static_cast<uint32_t>(configSnapshot().telemetryHz);
  if(now-lastTelemetry>=interval){lastTelemetry=now;socket.cleanupClients();if(socket.count())sendTelemetry();}
  if(restartPending.load(std::memory_order_acquire)) {
    const uint32_t deadline=restartAt.load(std::memory_order_relaxed);
    if(static_cast<int32_t>(now-deadline)>=0)ESP.restart();
  }
}
}
