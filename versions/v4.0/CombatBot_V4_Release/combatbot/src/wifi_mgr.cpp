/**
 * @file    wifi_mgr.cpp
 * @brief   AP + STA 联网、通配 DNS 与本地域名发布。
 *
 * ============================================================================
 * 模块职责：启动控制热点，按配置尝试接入路由器，并为本地页面提供地址信息。
 * 运行约定：网络字段取启动快照；配置修改在重启后应用，STA 每 15 s 尝试重连。
 * 设计边界：AP、DNS 和 mDNS 的实际可用性受无线栈及终端支持影响。
 *           固定备用热点参数只是失败后的重试措施，不能保证热点一定创建成功。
 * ============================================================================
 */
#include "wifi_mgr.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>

namespace bot {
namespace {
// ============================================================================
// 启动快照与 STA 重试状态
// ============================================================================
DNSServer dns;
Config bootConfig;
uint32_t lastStaAttempt=0;
bool begun=false;
/** @brief 配置启用且 SSID 非空时发起 STA 连接，并记录本次尝试时刻。 */
void trySta() {
  if(!bootConfig.staEnabled || !bootConfig.staSsid[0]) return;
  // 保持 WIFI_AP_STA 模式，只重试 STA；本函数不主动关闭 AP 接口。
  WiFi.begin(bootConfig.staSsid,bootConfig.staPass);
  lastStaAttempt=millis();
}
}
// ============================================================================
// 网络启动与周期维护
// ============================================================================
/**
 * @brief   按启动配置建立 AP + STA 模式，并启动 DNS / mDNS 服务。
 * @param cfg 网络名称、凭据与 STA 开关等启动配置。
 * @note AP 默认地址为 192.168.4.1；AP 密码为空时请求创建开放热点。
 * @details 自定义热点创建失败后用固定名称和密码再次尝试；备用调用结果未被
 *          检查，调用完成并不代表 AP、DNS 或 mDNS 已确认可用。
 */
void wifiBegin(const Config& cfg) {
  bootConfig=cfg;
  WiFi.persistent(false); WiFi.mode(WIFI_AP_STA); WiFi.setSleep(false);
  WiFi.setHostname(bootConfig.hostname); WiFi.setAutoReconnect(false);
  WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
  const char* password=bootConfig.apPass[0]?bootConfig.apPass:nullptr;
  bool apOk=WiFi.softAP(bootConfig.apSsid,password);
  if(!apOk) { // 使用固定备用参数再次尝试；无线栈故障时这次尝试仍可能失败。
    WiFi.softAP("CombatBot-AP","12345678");
  }
  dns.setErrorReplyCode(DNSReplyCode::NoError); dns.start(53,"*",WiFi.softAPIP());
  if(MDNS.begin(bootConfig.hostname)) MDNS.addService("http","tcp",80);
  // mDNS 为支持 .local 的终端提供发现入口；不支持时可尝试 AP IP 或通配 DNS。
  trySta(); begun=true;
}
/**
 * @brief   处理一个 DNS 请求，并在 STA 未连接时推进定时重试。
 * @note 使用启动配置快照；此接口保留 Config 参数，当前实现不应用热修改。
 */
void wifiTick(const Config&) {
  if(!begun) return;
  dns.processNextRequest();
  if(bootConfig.staEnabled && bootConfig.staSsid[0] && WiFi.status()!=WL_CONNECTED
     && uint32_t(millis()-lastStaAttempt)>=15000) trySta();
  // 网络配置重启生效，防止保存热点参数时立即中断当前控制连接。
}
// ============================================================================
// 状态查询：报告 STA 连接状态，不额外探测热点或服务是否可达
// ============================================================================
/** @brief STA 已连接时返回 AP_STA，否则返回 AP_ONLY；名称不保证 AP 实际已成功启动。 */
String networkMode() { return WiFi.status()==WL_CONNECTED?"AP_STA":"AP_ONLY"; }
/** @brief 返回无线栈报告的 AP IP 地址字符串。 */
String apIp() { return WiFi.softAPIP().toString(); }
/** @brief STA 已连接时返回其 IP 地址，未连接时返回空字符串。 */
String staIp() { return WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String(""); }
/** @brief STA 已连接时返回 RSSI，单位 dBm；未连接时返回约定值 0。 */
int networkRssi() { return WiFi.status()==WL_CONNECTED?WiFi.RSSI():0; }
}
