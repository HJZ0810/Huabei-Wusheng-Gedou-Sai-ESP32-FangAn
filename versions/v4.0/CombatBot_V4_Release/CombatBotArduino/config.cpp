/**
 * @file    config.cpp
 * @brief   配置校验与 NVS 存储：先保存成功，再发布运行快照。
 *
 * ============================================================================
 * 数据流
 *   JSON 部分更新 → 合并至临时副本 → 类型与跨字段校验 → NVS 写入 → 发布 live。
 *   配置载入和更新共用同一套校验规则，避免重启后接受运行时会拒绝的参数。
 *
 * 并发约定
 *   configMutex 保护 live 与配置写入过程；configSnapshot() 返回按值副本。
 *   写 Flash 时会持锁，控制任务取得快照可能等待，因此调用方须先停止动作。
 *
 * 存储边界
 *   写入失败时不发布新内存配置；真实掉电恢复能力仍需通过设备端测试确认。
 * ============================================================================
 */
#include "config.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <ctype.h>

namespace bot {
namespace {
// ============================================================================
// 配置状态与字段清单 · 同一清单复用于 JSON 解析和导出
// ============================================================================

Config live;
SemaphoreHandle_t configMutex=nullptr;
// 清单按数据类型分组，键名保持与 Config 成员一致；解析时仍严格检查 JSON 类型。
#define FLOAT_FIELDS(X) \
 X(wheelMm) X(wheelbaseMm) X(trackMm) X(ppr) X(pulsesPerCm) X(turnFactor) \
 X(maxSpeed) X(defaultSpeed) X(acceleration) X(deceleration) X(maxYawRate) X(turnAcceleration) X(turnDeceleration) \
 X(fusionAlpha) X(leftTrim) X(rightTrim) X(pwmDeadzone) X(irThresholdCm) \
 X(batteryLowV) X(batteryScale) X(gyroBias) X(servoDownDeg) X(servoUpDeg) \
 X(irSlowdownCm) X(irScale) X(impactThresholdG) \
 X(arenaOuterCm) X(arenaPlatformCm) X(arenaBodyLengthCm) X(arenaBodyWidthCm) X(arenaMarginCm) \
 X(arenaNavSpeed) X(arenaSearchSpeed) X(arenaPushSpeed) X(arenaClimbSpeed) \
 X(arenaEntryX) X(arenaEntryY) X(arenaEntryHeading)
#define BOOL_FIELDS(X) X(staEnabled) X(grayActiveHigh) X(e18ActiveHigh) X(safetyEnabled) X(coastOnLoss) \
 X(edgeProtection) X(irProtection) X(tiltProtection) X(irFailSafeStop) \
 X(cloudEnabled) X(arenaEnabled) X(arenaCalibrated) X(arenaClimbEnabled)
#define INT_FIELDS(X) X(irCount) X(batteryChannel) X(gyroAxis) X(gyroSign) X(servoMinUs) X(servoMaxUs) X(servoCenterUs) X(digitalDebounceMs) X(telemetryHz) X(cloudPort) X(arenaTaskMs) X(arenaClimbMs)
#define STRING_FIELDS(X) X(apSsid) X(apPass) X(staSsid) X(staPass) X(hostname) X(cloudHost) X(cloudPath) X(cloudDeviceId) X(cloudDeviceKey)

// ============================================================================
// 基础校验 · 将类型、字符串与范围错误转换为可直接展示的字段提示
// ============================================================================

/** @brief 统一构造“字段: 原因”的错误信息，便于表单定位问题。 */
bool fail(String& e,const char* field,const char* reason) { e=String(field)+": "+reason; return false; }

/**
 * @brief   读取有限数值，不将布尔值或字符串隐式转换为运动参数。
 * @param[out] out 成功时写入浮点值；失败时调用方不得使用该结果。
 * @note NaN/Infinity 会使 PID 与限幅失去意义，必须在配置入口拒绝。
 */
bool number(JsonVariantConst v,float& out,const char* field,String& e) {
  if(v.is<bool>() || !(v.is<float>() || v.is<double>() || v.is<int>() || v.is<uint32_t>()))
    return fail(e,field,"必须是数值");
  out=v.as<float>();
  return isfinite(out) || fail(e,field,"必须是有限数值");
}
/**
 * @brief   将 JSON 字符串复制至定长缓冲区，同时检查长度与控制字符。
 * @param[in] capacity 目标缓冲区总字节数，包含结尾的零字符。
 * @details 同时检查 JSON 长度和 strlen，避免嵌入零字符被当成较短的合法字符串。
 */
bool copyString(JsonVariantConst v,char* dest,size_t capacity,const char* field,String& e) {
  if(!v.is<const char*>()) return fail(e,field,"必须是字符串");
  JsonString s=v.as<JsonString>();
  if(s.size()>=capacity || strlen(s.c_str())!=s.size()) return fail(e,field,"长度超限或含零字符");
  for(size_t i=0;i<s.size();++i) if((uint8_t)s.c_str()[i]<32 || s.c_str()[i]==127)
    return fail(e,field,"不允许控制字符");
  memcpy(dest,s.c_str(),s.size()+1); return true;
}
/**
 * @brief   校验 STA 密码格式：开放网络、8～63 字节密码或 64 位十六进制密钥。
 * @note AP 接口不接受这里的 64 位密钥形式，AP 密码另在 validate() 中校验。
 */
bool passwordValid(const char* p) {
  size_t n=strlen(p); if(n==0 || (n>=8 && n<=63)) return true;
  if(n!=64) return false;
  for(size_t i=0;i<n;++i) if(!isxdigit((unsigned char)p[i])) return false;
  return true;
}
/** @brief 检查有限数值是否位于闭区间 [lo, hi]。 */
bool range(float value,float lo,float hi,const char* field,String& e) {
  return (isfinite(value) && value>=lo && value<=hi) || fail(e,field,"数值超出允许范围");
}
/** @brief 检查 PID 增益的允许范围；具体参数仍需按电机与载荷调试。 */
bool gainsValid(const Gains& g,const char* name,String& e) {
  return range(g.kp,0,10000,name,e) && range(g.ki,0,10000,name,e) && range(g.kd,0,1000,name,e);
}
// ============================================================================
// 完整配置校验 · 单字段范围与跨字段关系在此统一收口
// ============================================================================

/**
 * @brief   检查可发布配置的网络格式、物理参数、控制约束与舵机行程。
 * @details
 *   先验证网络字符串，再验证数值范围与相互关系。例如默认速度不能超过
 *   最大速度，舵机最小脉宽必须小于最大脉宽，中位脉宽须落在行程内。
 * @note 范围校验防止明显非法输入，不表示范围内的任意值都适合实际车辆。
 */
bool validate(const Config& c,String& e) {
  if(!c.apSsid[0]) return fail(e,"apSsid","不能为空");
  const size_t apPasswordLength=strlen(c.apPass);
  if(apPasswordLength!=0 && (apPasswordLength<8 || apPasswordLength>63))
    return fail(e,"apPass","须为空或8~63字节（ESP32热点接口限制）");
  if(!passwordValid(c.staPass)) return fail(e,"staPass","须为空、8~63字节，或64位十六进制密钥");
  if(c.staEnabled && !c.staSsid[0]) return fail(e,"staSsid","启用STA时不能为空");
  size_t n=strlen(c.hostname);
  if(n==0 || c.hostname[0]=='-' || c.hostname[n-1]=='-') return fail(e,"hostname","非法mDNS主机名");
  for(size_t i=0;i<n;++i) if(!isalnum((unsigned char)c.hostname[i]) && c.hostname[i]!='-')
    return fail(e,"hostname","仅允许ASCII字母、数字和连字符");
#define CHECK(field,lo,hi) if(!range(c.field,lo,hi,#field,e)) return false
  CHECK(cloudPort,1,65535);
  for(const char* p=c.cloudHost;*p;++p) if(!isalnum((unsigned char)*p) && *p!='.' && *p!='-')
    return fail(e,"cloudHost","仅允许DNS主机名或IP，不含协议与端口");
  if(c.cloudPath[0]!='/' || strstr(c.cloudPath,"..")) return fail(e,"cloudPath","须为绝对路径，不能含上级目录");
  for(const char* p=c.cloudPath;*p;++p) if((unsigned char)*p<=32 || *p=='?' || *p=='#') return fail(e,"cloudPath","不能含空白、查询串或片段");
  for(const char* p=c.cloudDeviceId;*p;++p) if(!isalnum((unsigned char)*p) && *p!='-' && *p!='_') return fail(e,"cloudDeviceId","仅允许字母、数字、连字符、下划线");
  for(const char* p=c.cloudDeviceKey;*p;++p) if(!isalnum((unsigned char)*p) && *p!='-' && *p!='_') return fail(e,"cloudDeviceKey","密钥格式非法");
  if(c.cloudEnabled && (!c.staEnabled || !c.cloudHost[0] || !c.cloudDeviceId[0] || strlen(c.cloudDeviceKey)<32))
    return fail(e,"cloudEnabled","需先启用STA并填写主机、设备编号及至少32字节独立密钥");
  if(c.cloudEnabled && (!strstr(c.cloudCaPem,"-----BEGIN CERTIFICATE-----") || !strstr(c.cloudCaPem,"-----END CERTIFICATE-----")))
    return fail(e,"cloudCaPem","需提供有效CA PEM，禁止跳过证书校验");
  CHECK(arenaOuterCm,200,1000); CHECK(arenaPlatformCm,100,c.arenaOuterCm-40);
  CHECK(arenaBodyLengthCm,10,80); CHECK(arenaBodyWidthCm,10,80); CHECK(arenaMarginCm,2,30);
  CHECK(arenaNavSpeed,1,80); CHECK(arenaSearchSpeed,1,80);
  CHECK(arenaPushSpeed,1,80); CHECK(arenaClimbSpeed,1,80);
  CHECK(arenaEntryX,-c.arenaOuterCm/2,c.arenaOuterCm/2); CHECK(arenaEntryY,-c.arenaOuterCm/2,c.arenaOuterCm/2);
  CHECK(arenaEntryHeading,-360,360); CHECK(arenaTaskMs,1000,600000); CHECK(arenaClimbMs,1000,10000);
  if(c.arenaEnabled && c.arenaBodyWidthCm+2*c.arenaMarginCm>= (c.arenaOuterCm-c.arenaPlatformCm)/2)
    return fail(e,"arenaBodyWidthCm","车宽和安全余量不能覆盖整个外圈通道");
  if(c.arenaEnabled) {
    const float clearance=0.5f*sqrtf(c.arenaBodyLengthCm*c.arenaBodyLengthCm+c.arenaBodyWidthCm*c.arenaBodyWidthCm)+c.arenaMarginCm+2;
    if(c.arenaOuterCm/2-clearance<=c.arenaPlatformCm/2+clearance+1)
      return fail(e,"arenaMarginCm","车身外包络、误差与安全余量无法通过外圈");
    if(c.maxYawRate<5) return fail(e,"maxYawRate","自主控制至少需要5度每秒转向预算");
    const float heading=fmodf(c.arenaEntryHeading+360,360);
    if(fabsf(heading/90-roundf(heading/90))>0.001f) return fail(e,"arenaEntryHeading","登台入口仅支持四个正交方向（90度整数倍）");
    const float offset=(heading==0 || heading==180)?c.arenaEntryY:c.arenaEntryX;
    if(fabsf(offset)+clearance+12>=c.arenaPlatformCm/2)
      return fail(e,"arenaEntryX/Y","入口须离台角留足车身和对准余量");
  }
  // 尺寸为 mm；速度为 cm/s；加减速度为 cm/s²；角速度为 °/s。
  CHECK(wheelMm,20,500); CHECK(wheelbaseMm,30,1000); CHECK(trackMm,50,1000);
  CHECK(ppr,1,10000); CHECK(pulsesPerCm,0,10000); CHECK(turnFactor,0.1f,10);
  CHECK(maxSpeed,1,220); CHECK(defaultSpeed,0.1f,c.maxSpeed);
  CHECK(acceleration,1,500); CHECK(deceleration,1,1000); CHECK(maxYawRate,1,720);
  CHECK(turnAcceleration,1,3600); CHECK(turnDeceleration,1,3600);
  CHECK(fusionAlpha,0,1); CHECK(leftTrim,0.5f,1.5f); CHECK(rightTrim,0.5f,1.5f);
  CHECK(pwmDeadzone,0,800); CHECK(irThresholdCm,20,150);
  CHECK(irSlowdownCm,20,150); CHECK(irScale,0.5f,2); CHECK(impactThresholdG,1.2f,16);
  if(c.irSlowdownCm<=c.irThresholdCm) return fail(e,"irSlowdownCm","必须大于irThresholdCm");
  CHECK(batteryLowV,5,35); CHECK(batteryScale,1,50);
  CHECK(irCount,1,12); CHECK(batteryChannel,0,15); CHECK(gyroAxis,0,2);
  CHECK(digitalDebounceMs,0,500);
  if(c.gyroSign!=1 && c.gyroSign!=-1) return fail(e,"gyroSign","只能是1或-1");
  // gyroBias 是完成轴映射后的角速度零偏；脉宽端点用 µs 表示。
  CHECK(gyroBias,-30,30); CHECK(servoMinUs,500,2500); CHECK(servoMaxUs,500,2500);
  if(c.servoMinUs>=c.servoMaxUs) return fail(e,"servoMinUs","必须小于servoMaxUs");
  CHECK(servoCenterUs,c.servoMinUs,c.servoMaxUs); CHECK(servoDownDeg,0,180); CHECK(servoUpDeg,0,180);
  CHECK(heartbeatMs,200,5000);
  CHECK(stallTimeoutMs,300,5000); CHECK(telemetryHz,1,30);
  if(c.pulsesPerCm>0 && c.pulsesPerCm<0.01f) return fail(e,"pulsesPerCm","标定值过小");
  if(!gainsValid(c.speedPid,"speedPid",e) || !gainsValid(c.positionPid,"positionPid",e)
     || !gainsValid(c.headingPid,"headingPid",e)) return false;
#undef CHECK
  return true;
}
// ============================================================================
// 部分更新解析 · 只改变输入中出现的字段，拒绝未知键与隐式类型转换
// ============================================================================

/** @brief 合并一个 PID 增益对象；未出现的 kp/ki/kd 保留原值。 */
bool patchGains(JsonVariantConst v,Gains& g,const char* field,String& e) {
  if(!v.is<JsonObjectConst>()) return fail(e,field,"必须是kp/ki/kd对象");
  JsonObjectConst o=v.as<JsonObjectConst>();
  for(JsonPairConst kv:o) {
    const char* k=kv.key().c_str(); float value=0;
    if(!number(kv.value(),value,field,e)) return false;
    if(!strcmp(k,"kp")) g.kp=value;
    else if(!strcmp(k,"ki")) g.ki=value;
    else if(!strcmp(k,"kd")) g.kd=value;
    else return fail(e,field,"未知PID字段");
  }
  return true;
}
/**
 * @brief   将 JSON 对象合并至候选配置，并对合并结果执行完整校验。
 * @param[in,out] c 临时配置副本；失败时不得把它发布到运行态。
 * @details 布尔值、数值、整数与字符串分别解析，避免 "100" 或 1 代替正确类型。
 * @note hasApPass/hasStaPass 是导出元数据，允许回传但不参与配置更新。
 */
bool patch(JsonVariantConst input,Config& c,String& e) {
  if(!input.is<JsonObjectConst>()) return fail(e,"config","必须是JSON对象");
  const JsonObjectConst object=input.as<JsonObjectConst>();
  // A 旧导出由 PID、网络、舵机和 IR 字段指纹识别，并要求没有融合新增键。
  // 兼容这一旧格式时补齐渐进区间；仅更新阈值的普通请求仍须满足跨字段校验。
  // B 导出由外部显式转换处理，不能通过别名绕过设备端校验。
  bool legacyA=object["speedPid"].is<JsonObjectConst>() && object["hostname"].is<const char*>()
    && !object["servoMinUs"].isNull() && !object["irThresholdCm"].isNull();
  const char* addedFields[]={"edgeProtection","irProtection","tiltProtection","irFailSafeStop",
    "stallTimeoutMs","telemetryHz","irScale","impactThresholdG","irSlowdownCm",
    "turnAcceleration","turnDeceleration"};
  for(JsonPairConst kv:object) {
    for(const char* field:addedFields) if(!strcmp(kv.key().c_str(),field)) legacyA=false;
  }
  if(legacyA) {
    float oldThreshold=0;
    if(!number(object["irThresholdCm"],oldThreshold,"irThresholdCm",e)) return false;
    if(oldThreshold>=150) return fail(e,"irThresholdCm","旧版停车阈值须低于150cm，才能保留渐进限速区间");
    c.irSlowdownCm=fmaxf(60.0f,fminf(150.0f,oldThreshold+10.0f));
  }
  for(JsonPairConst kv:object) {
    const char* key=kv.key().c_str(); JsonVariantConst v=kv.value();
#define APPLY_FLOAT(field) if(!strcmp(key,#field)) { if(!number(v,c.field,#field,e)) return false; continue; }
    FLOAT_FIELDS(APPLY_FLOAT)
#undef APPLY_FLOAT
#define APPLY_BOOL(field) if(!strcmp(key,#field)) { if(!v.is<bool>()) return fail(e,#field,"必须是布尔值"); c.field=v.as<bool>(); continue; }
    BOOL_FIELDS(APPLY_BOOL)
#undef APPLY_BOOL
#define APPLY_INT(field) if(!strcmp(key,#field)) { float f=0; if(!number(v,f,#field,e)) return false; if(floorf(f)!=f || f<-1000000 || f>1000000) return fail(e,#field,"必须是整数"); c.field=(int)f; continue; }
    INT_FIELDS(APPLY_INT)
#undef APPLY_INT
#define APPLY_STRING(field) if(!strcmp(key,#field)) { if(!copyString(v,c.field,sizeof(c.field),#field,e)) return false; continue; }
    STRING_FIELDS(APPLY_STRING)
#undef APPLY_STRING
    if(!strcmp(key,"heartbeatMs") || !strcmp(key,"stallTimeoutMs")) {
      float f=0; if(!number(v,f,key,e)) return false;
      const float minimum=!strcmp(key,"heartbeatMs")?200.0f:300.0f;
      if(floorf(f)!=f || f<minimum || f>5000) return fail(e,key,"时间须为允许范围内的整数毫秒");
      if(!strcmp(key,"heartbeatMs")) c.heartbeatMs=(uint32_t)f;
      else c.stallTimeoutMs=(uint32_t)f;
      continue;
    }
    if(!strcmp(key,"invert")) {
      if(!v.is<JsonArrayConst>() || v.size()!=4) return fail(e,key,"须为四个布尔值");
      for(int i=0;i<4;++i) { if(!v[i].is<bool>()) return fail(e,key,"须为四个布尔值"); c.invert[i]=v[i].as<bool>(); }
      continue;
    }
    if(!strcmp(key,"speedPid")) { if(!patchGains(v,c.speedPid,key,e)) return false; continue; }
    if(!strcmp(key,"positionPid")) { if(!patchGains(v,c.positionPid,key,e)) return false; continue; }
    if(!strcmp(key,"headingPid")) { if(!patchGains(v,c.headingPid,key,e)) return false; continue; }
    if(!strcmp(key,"cloudCaPem")) {
      if(!v.is<const char*>()) return fail(e,key,"须为PEM字符串");
      JsonString pem=v.as<JsonString>();
      if(pem.size()>=sizeof(c.cloudCaPem) || strlen(pem.c_str())!=pem.size()) return fail(e,key,"长度超限或含零字符");
      for(size_t i=0;i<pem.size();++i) if(((unsigned char)pem.c_str()[i]<32 && pem.c_str()[i]!='\r' && pem.c_str()[i]!='\n') || pem.c_str()[i]==127) return fail(e,key,"含非法控制字符");
      memcpy(c.cloudCaPem,pem.c_str(),pem.size()+1); continue;
    }
    if(!strcmp(key,"hasApPass") || !strcmp(key,"hasStaPass") || !strcmp(key,"hasCloudDeviceKey")) continue;
    return fail(e,key,"未知配置字段");
  }
  return validate(c,e);
}
// ============================================================================
// 持久化提交 · 调用方持有 configMutex，写入成功后才发布新快照
// ============================================================================

/**
 * @brief   将完整配置序列化为一个 NVS 字符串，并在成功后替换 live。
 * @pre 调用方已持有 configMutex，本函数不会再次取得互斥锁。
 * @details 全部参数作为同一条配置记录保存；检查 putString 的返回字节数，
 *          失败时保留当前内存快照，并向调用方返回可展示的错误原因。
 */
bool saveLocked(const Config& c,String& e) {
  if(!validate(c,e)) return false;
  JsonDocument doc; configJson(c,doc.to<JsonObject>(),true);
  String text; serializeJson(doc,text);
  Preferences prefs;
  if(!prefs.begin("combatfusion",false)) return fail(e,"NVS","无法打开存储");
  size_t written=prefs.putString("config",text); prefs.end();
  if(written!=text.length()) return fail(e,"NVS","写入失败，当前配置保留");
  const uint32_t revision=live.revision+1;
  live=c; live.revision=revision; e=""; return true;
}
}

// ============================================================================
// 公共接口 · 初始化、快照、导出与受保护的配置更新
// ============================================================================

/**
 * @brief   从默认配置开始载入 NVS，存储缺失、损坏或非法时保留默认值。
 * @details 载入大小限制为 8192 字节；读取结果仍走正常的 JSON 合并与校验。
 * @note 失败记录不会在此自动覆盖，便于后续诊断；此函数应在任务启动前调用。
 */
void configBegin() {
  if(!configMutex) configMutex=xSemaphoreCreateMutex();
  if(!configMutex) { live=Config(); return; }
  xSemaphoreTake(configMutex,portMAX_DELAY);
  live=Config(); Preferences prefs;
  if(prefs.begin("combatfusion",true)) {
    String text=prefs.getString("config",""); prefs.end();
    if(text.length()>0 && text.length()<=8192) {
      JsonDocument doc; String e; Config loaded;
      if(!deserializeJson(doc,text) && patch(doc.as<JsonVariantConst>(),loaded,e)) live=loaded;
      else Serial.println("NVS配置损坏或非法，使用默认配置；原存储保留供诊断");
    }
  }
  xSemaphoreGive(configMutex);
}
/** @brief 在互斥锁保护下复制运行配置，返回后不继续持锁。 */
Config configSnapshot() {
  if(!configMutex) return live;
  xSemaphoreTake(configMutex,portMAX_DELAY); Config result=live; xSemaphoreGive(configMutex);
  return result;
}
/**
 * @brief   导出完整配置，包括四轮极性数组与三组 PID 参数对象。
 * @param[in] secrets 是否输出真实 AP/STA 密码；调用方负责选择使用场景。
 * @note secrets=false 会把密码字段写为空串；直接回传该对象会清空原密码，
 *       如需保留密码，部分更新请求应省略对应密码字段。
 */
void configJson(const Config& c,JsonObject o,bool secrets) {
#define WRITE(field) o[#field]=c.field;
  FLOAT_FIELDS(WRITE) BOOL_FIELDS(WRITE) INT_FIELDS(WRITE)
#undef WRITE
  o["apSsid"]=c.apSsid; o["staSsid"]=c.staSsid; o["hostname"]=c.hostname;
  o["apPass"]=secrets?c.apPass:""; o["staPass"]=secrets?c.staPass:"";
  o["hasApPass"]=bool(c.apPass[0]); o["hasStaPass"]=bool(c.staPass[0]);
  o["cloudHost"]=c.cloudHost; o["cloudPort"]=c.cloudPort; o["cloudPath"]=c.cloudPath;
  o["cloudDeviceId"]=c.cloudDeviceId; o["cloudDeviceKey"]=secrets?c.cloudDeviceKey:"";
  o["cloudCaPem"]=c.cloudCaPem; o["hasCloudDeviceKey"]=bool(c.cloudDeviceKey[0]);
  o["heartbeatMs"]=c.heartbeatMs;
  o["stallTimeoutMs"]=c.stallTimeoutMs;
  JsonArray a=o["invert"].to<JsonArray>(); for(bool v:c.invert) a.add(v);
  const Gains* gains[3]={&c.speedPid,&c.positionPid,&c.headingPid};
  const char* names[3]={"speedPid","positionPid","headingPid"};
  for(int i=0;i<3;++i) {
    JsonObject g=o[names[i]].to<JsonObject>();
    g["kp"]=gains[i]->kp; g["ki"]=gains[i]->ki; g["kd"]=gains[i]->kd;
  }
}
/**
 * @brief   对当前配置执行部分更新，校验与持久化全部成功后再发布。
 * @details 拿锁后复制 live，解析只修改候选副本，避免半份配置被控制任务读取。
 */
bool configApply(JsonVariantConst input,String& error) {
  if(!configMutex) return fail(error,"config","存储锁初始化失败");
  xSemaphoreTake(configMutex,portMAX_DELAY);
  Config next=live;
  // “未提交”与“显式清空”语义不同，密码也遵循相同规则。
  bool ok=patch(input,next,error) && saveLocked(next,error);
  xSemaphoreGive(configMutex); return ok;
}
/** @brief 保存完整快照；与部分更新共用相同的校验和提交顺序。 */
bool configSave(const Config& c,String& error) {
  if(!configMutex) return fail(error,"config","存储锁初始化失败");
  xSemaphoreTake(configMutex,portMAX_DELAY); bool ok=saveLocked(c,error); xSemaphoreGive(configMutex); return ok;
}
/** @brief 以默认 Config 快照替换全部配置；网络参数等待重启应用。 */
bool configDefaults(String& error) { return configSave(Config(),error); }

/**
 * @brief   仅重置网络相关配置，保留机械参数、标定结果和运动控制参数。
 * @details AP SSID/密码及主机名恢复默认；STA SSID/密码清空并关闭 STA。
 */
bool configClearWifi(String& error) {
  if(!configMutex) return fail(error,"config","存储锁初始化失败");
  xSemaphoreTake(configMutex,portMAX_DELAY); Config c=live, defaults;
  memcpy(c.apSsid,defaults.apSsid,sizeof(c.apSsid)); memcpy(c.apPass,defaults.apPass,sizeof(c.apPass));
  c.staSsid[0]=0; c.staPass[0]=0; c.staEnabled=false;
  c.cloudEnabled=false;
  memcpy(c.hostname,defaults.hostname,sizeof(c.hostname));
  bool ok=saveLocked(c,error); xSemaphoreGive(configMutex); return ok;
}
}
