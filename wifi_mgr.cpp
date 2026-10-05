/**
 ******************************************************************************
 * @file    wifi_mgr.cpp
 * @brief   网络状态机实现（AP 常驻 + STA 可选 + mDNS + Captive Portal）
 *
 * @details 关键设计：
 *            - 始终使用 @c WIFI_AP_STA 模式，即使 STA 连不上，AP 也照常工作；
 *            - STA 重连每 15s 一次，且重连过程不影响已建立的 WebSocket 连接；
 *            - mDNS 只需注册一次，AP 与 STA 两个网口都能解析到同一主机名。
 *
 * @author  CombatBot 电控组
 * @version V1.1.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>统一企业级注释规范
 *          </table>
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include "wifi_mgr.h"
#include "cfg.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>

/* ==========================================================================
 *                          私有常量与静态数据
 * ========================================================================== */
#define STA_RETRY_MS  15000     /**< STA 重连间隔 ms（设计文档 4.1 节）       */

static DNSServer s_dns;                 /**< 强制门户 DNS 服务器               */
NetMode          gNetMode = NET_BOOT;   /**< 当前网络模式                      */
static uint32_t  s_lastTry = 0;         /**< 上次尝试 STA 的时间戳             */
static bool      s_staRunning = false;  /**< STA 是否已发起连接                */

/* ==========================================================================
 *                          私有函数（文件内静态）
 * ========================================================================== */

/**
 * @brief   发起一次 STA 连接
 * @param   无
 * @return  无
 * @note    连接失败不会影响 AP：AP 由 softAP 常驻，与 STA 状态完全解耦。
 */
static void startSta() {
    CfgSnap cs;
    const Cfg& c = cs.c;

    if (!c.staEnable || strlen(c.staSsid) == 0) { s_staRunning = false; return; }

    WiFi.setHostname(c.mdns);
    WiFi.begin(c.staSsid, c.staPass);
    s_staRunning = true;
    Serial.printf("[wifi] STA 尝试连接 %s\n", c.staSsid);
}

/* ==========================================================================
 *                              对外函数实现
 * ========================================================================== */

/**
 * @brief   初始化网络子系统
 * @param   无
 * @return  无
 */
void wifiInit() {
    CfgSnap cs;
    const Cfg& c = cs.c;

    WiFi.mode(WIFI_AP_STA);         /* AP 常驻；STA 可选（即使不连也保持 AP） */
    WiFi.setHostname(c.mdns);

    /* ---- AP（永不关闭） ---- */
    bool ok = (strlen(c.apPass) >= 8)
            ? WiFi.softAP(c.apSsid, c.apPass)
            : WiFi.softAP(c.apSsid);        /* 密码 <8 位时开放热点 */
    WiFi.softAPsetHostname(c.mdns);
    Serial.printf("[wifi] AP %s %s  IP=%s\n", c.apSsid, ok ? "OK" : "FAIL",
                  WiFi.softAPIP().toString().c_str());

    /* ---- mDNS：AP / STA 两种模式下 combatbot.local 都要能解析 ---- */
    if (MDNS.begin(c.mdns)) {
        MDNS.addService("http", "tcp", 80);
        MDNS.setInstanceName("CombatBot Controller");
        Serial.printf("[wifi] mDNS http://%s.local\n", c.mdns);
    } else {
        Serial.println("[wifi] mDNS 启动失败（不影响 IP 直连）");
    }

    /* ---- 强制门户：手机连上 AP 后自动弹出控制页 ---- */
    if (c.captive) {
        s_dns.setErrorReplyCode(DNSReplyCode::NoError);
        if (s_dns.start(53, "*", WiFi.softAPIP())) Serial.println("[wifi] Captive Portal ON");
    }

    startSta();
    s_lastTry  = millis();
    gNetMode   = NET_AP_ONLY;
}

/**
 * @brief   配置变更后立即重新尝试 STA
 * @param   无
 * @return  无
 */
void wifiApplySta() {
    WiFi.disconnect(false);
    s_staRunning = false;
    s_lastTry    = 0;               /* 下一个 wifiLoop 立即重试 */
}

/**
 * @brief   网络周期任务
 * @param[in] now  当前毫秒时间戳
 * @return   无
 */
void wifiLoop(uint32_t now) {
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected) {
        gNetMode      = NET_AP_STA;
        s_staRunning  = true;
    } else {
        if (gNetMode == NET_AP_STA)
            Serial.println("[wifi] STA 掉线 → 回落 AP_ONLY（AP 仍在）");
        gNetMode = NET_AP_ONLY;
    }

    /* STA 每 15s 重试一次（重试期间 AP 不受影响） */
    CfgSnap cs;
    if (cs.c.staEnable && !connected && (now - s_lastTry > STA_RETRY_MS)) {
        s_lastTry = now;
        startSta();
    }

    /* 强制门户 DNS 请求处理（非阻塞） */
    if (cs.c.captive) s_dns.processNextRequest();
}

/**
 * @brief   获取 AP IP
 * @param   无
 * @return  IP 字符串
 */
String wifiAPIP()  { return WiFi.softAPIP().toString(); }

/**
 * @brief   获取 STA IP
 * @param   无
 * @return  IP 字符串（未连接返回空串）
 */
String wifiSTAIP() { return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(""); }

/**
 * @brief   获取当前生效 IP
 * @param   无
 * @return  IP 字符串
 */
String wifiActiveIP() {
    String s = wifiSTAIP();
    return s.length() ? s : wifiAPIP();
}

/**
 * @brief   获取 RSSI
 * @param   无
 * @return  dBm
 */
int8_t wifiRSSI() {
    return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
}

/**
 * @brief   获取网络模式字符串
 * @param   无
 * @return  "AP" / "AP+STA"
 */
const char* wifiModeStr() {
    return (gNetMode == NET_AP_STA) ? "AP+STA" : "AP";
}

/**
 * @brief   强制门户是否启用
 * @param   无
 * @return  true 启用
 */
bool wifiCaptive() { return true; }
