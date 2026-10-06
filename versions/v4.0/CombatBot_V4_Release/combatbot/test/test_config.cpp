/**
 * @file test_config.cpp
 * @brief 使用真实配置实现验证字段校验、序列化及存储失败路径。
 *
 * ============================================================================
 * 测试范围
 *   ArduinoJson 与 config.cpp 使用真实实现；Preferences 和互斥锁由主机替身提供。
 *   内存映射模拟存储数据，不替代 Flash 掉电持久化、磨损或真实并发验证。
 * ============================================================================
 */
#include <cstdio>
#include <cassert>
#include <cmath>
#include <Arduino.h>
struct HostSerial { void println(const char*){} } Serial;
#include "../src/config.cpp"

static bool apply(const char* json,String& error){
  JsonDocument doc;assert(!deserializeJson(doc,json));return bot::configApply(doc.as<JsonVariantConst>(),error);
}
int main(){
  // ============================================================================
  // 场景一：默认值、有效字段更新与省略密码保留
  // ============================================================================
  // 新工程必须与两套旧固件的命名空间隔离，不能在首次启动继承不兼容参数。
  hostNvs.clear();
  const std::string oldA=R"({"wheelMm":96,"safetyEnabled":true})";
  const std::string oldB=R"({"wheelDia":99,"vKp":0.03})";
  hostNvs["combatbot/config"]=oldA; hostNvs["cbot/cfg"]=oldB;
  String error;bot::configBegin();
  assert(bot::configSnapshot().wheelMm==72);
  const auto defaults=bot::configSnapshot();
  assert(!defaults.safetyEnabled && defaults.edgeProtection && defaults.irProtection && defaults.tiltProtection);
  assert(defaults.irFailSafeStop && defaults.stallTimeoutMs==1500 && defaults.telemetryHz==15);
  assert(defaults.irScale==1 && defaults.impactThresholdG==2 && defaults.irSlowdownCm==60);
  assert(defaults.turnAcceleration==180 && defaults.turnDeceleration==240);
  assert(apply(R"({"apSsid":"TestBot","staSsid":"Router","staPass":"secret-pass","staEnabled":true,"wheelMm":80,"speedPid":{"kp":4,"ki":2,"kd":0.1},"invert":[true,false,true,false],"digitalDebounceMs":70})",error));
  auto cfg=bot::configSnapshot();assert(cfg.wheelMm==80 && cfg.invert[0] && cfg.speedPid.kp==4);
  assert(apply(R"({"maxSpeed":90})",error));
  assert(std::string(bot::configSnapshot().staPass)=="secret-pass");
  // ============================================================================
  // 场景二：字段类型、范围和非有限值拒绝
  // ============================================================================
  const auto stored=hostNvs["combatfusion/config"];
  for(const char* bad:{R"({"apPass":"short"})",R"({"hostname":"bad.local"})",R"({"irCount":13})",R"({"invert":[1,0,1,0]})",R"({"gyroSign":0})",R"({"servoMinUs":2400,"servoMaxUs":500})",R"({"maxSpeed":"100"})",R"({"newUnknownKey":1})",R"({"heartbeatMs":0})",R"({"digitalDebounceMs":5.5})"}){
    assert(!apply(bad,error));assert(!error.empty());assert(hostNvs["combatfusion/config"]==stored);
  }
  JsonDocument nonfinite;nonfinite["maxSpeed"]=NAN;
  assert(!bot::configApply(nonfinite.as<JsonVariantConst>(),error));
  // 新保护参数由服务端收口校验，绕过网页提交也不能写入非法组合。
  for(const char* bad:{R"({"edgeProtection":1})",R"({"irProtection":"true"})",R"({"tiltProtection":null})",
      R"({"irFailSafeStop":0})",R"({"stallTimeoutMs":299})",R"({"stallTimeoutMs":5001})",
      R"({"stallTimeoutMs":1000.5})",R"({"stallTimeoutMs":4294967295})",R"({"telemetryHz":0})",
      R"({"telemetryHz":31})",R"({"telemetryHz":256})",R"({"telemetryHz":2.5})",
      R"({"irScale":0.49})",R"({"irScale":2.01})",R"({"impactThresholdG":1.19})",R"({"impactThresholdG":16.01})",
      R"({"irSlowdownCm":25})",R"({"irThresholdCm":60})",R"({"irSlowdownCm":151})",
      R"({"turnAcceleration":0})",R"({"turnDeceleration":3601})"}) {
    assert(!apply(bad,error)); assert(!error.empty()); assert(hostNvs["combatfusion/config"]==stored);
  }
  nonfinite.clear(); nonfinite["irScale"]=INFINITY;
  assert(!bot::configApply(nonfinite.as<JsonVariantConst>(),error));
  // ============================================================================
  // 场景三：存储写入失败时保留当前配置
  // ============================================================================
  hostNvsWrite=false;assert(!apply(R"({"wheelMm":81})",error));
  assert(bot::configSnapshot().wheelMm==80 && hostNvs["combatfusion/config"]==stored);hostNvsWrite=true;
  // ============================================================================
  // 场景四：序列化往返与重新加载
  // ============================================================================
  JsonDocument exported;bot::configJson(bot::configSnapshot(),exported.to<JsonObject>());
  std::string json;serializeJson(exported,json);assert(apply(json.c_str(),error));
  bot::configBegin();assert(bot::configSnapshot().wheelMm==80 && bot::configSnapshot().digitalDebounceMs==70);
  // ============================================================================
  // 场景五：网络重置保持机械参数，非法存储回退默认值
  // ============================================================================
  assert(bot::configClearWifi(error));cfg=bot::configSnapshot();
  assert(!cfg.staEnabled && !cfg.staSsid[0] && std::string(cfg.apSsid)=="CombatBot-AP");
  assert(cfg.wheelMm==80);
  hostNvs["combatfusion/config"]="corrupt-json";bot::configBegin();assert(bot::configSnapshot().wheelMm==72);
  assert(bot::configDefaults(error));
  // 旧 A 配置缺少融合新增键时，加载原参数并以默认值补齐新增项。
  hostNvs["combatfusion/config"]=R"({"wheelMm":82,"safetyEnabled":false,"heartbeatMs":900})";
  bot::configBegin(); cfg=bot::configSnapshot();
  assert(cfg.wheelMm==82 && !cfg.safetyEnabled && cfg.heartbeatMs==900);
  assert(cfg.edgeProtection && cfg.irProtection && cfg.tiltProtection && cfg.irFailSafeStop);
  assert(cfg.stallTimeoutMs==1500 && cfg.telemetryHz==15 && cfg.irScale==1);
  // 完整旧 A 导出的高停车距离有明示兼容；普通部分更新不触发迁移。
  assert(apply(R"({"speedPid":{"kp":3,"ki":8,"kd":0},"hostname":"combatbot","servoMinUs":500,"irThresholdCm":100})",error));
  assert(bot::configSnapshot().irThresholdCm==100 && bot::configSnapshot().irSlowdownCm==110);
  assert(!apply(R"({"irThresholdCm":120})",error));
  assert(!apply(R"({"speedPid":{"kp":3,"ki":8,"kd":0},"hostname":"combatbot","servoMinUs":500,"irThresholdCm":150})",error));
  assert(error.find("150")!=std::string::npos);
  assert(!apply(R"({"speedPid":{"kp":3,"ki":8,"kd":0},"hostname":"combatbot","servoMinUs":500,"irThresholdCm":120,"edgeProtection":true})",error));
  // 两组有效边界均可持久化，且导出/导入/重载保持同一字段语义。
  assert(apply(R"({"safetyEnabled":true,"edgeProtection":false,"irProtection":false,"tiltProtection":false,"irFailSafeStop":false,"stallTimeoutMs":300,"telemetryHz":1,"irScale":0.5,"impactThresholdG":1.2,"irThresholdCm":20,"irSlowdownCm":21,"turnAcceleration":1,"turnDeceleration":1})",error));
  assert(apply(R"({"stallTimeoutMs":5000,"telemetryHz":30,"irScale":2,"impactThresholdG":16,"irThresholdCm":149,"irSlowdownCm":150,"turnAcceleration":3600,"turnDeceleration":3600})",error));
  exported.clear(); bot::configJson(bot::configSnapshot(),exported.to<JsonObject>());
  json.clear(); serializeJson(exported,json); assert(apply(json.c_str(),error));
  bot::configBegin(); cfg=bot::configSnapshot();
  assert(cfg.safetyEnabled && !cfg.edgeProtection && !cfg.irProtection && !cfg.tiltProtection && !cfg.irFailSafeStop);
  assert(cfg.stallTimeoutMs==5000 && cfg.telemetryHz==30 && cfg.irScale==2 && cfg.impactThresholdG==16);
  assert(cfg.irThresholdCm==149 && cfg.irSlowdownCm==150 && cfg.turnAcceleration==3600 && cfg.turnDeceleration==3600);
  const auto stable=hostNvs["combatfusion/config"];
  hostNvsOpen=false; assert(!apply(R"({"telemetryHz":10})",error));
  assert(bot::configSnapshot().telemetryHz==30 && hostNvs["combatfusion/config"]==stable); hostNvsOpen=true;
  assert(hostNvs["combatbot/config"]==oldA && hostNvs["cbot/cfg"]==oldB);
  std::puts("config validation/persistence-path tests passed");
}
