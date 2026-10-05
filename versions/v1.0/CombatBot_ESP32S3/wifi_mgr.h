/**
 ******************************************************************************
 * @file    wifi_mgr.h
 * @brief   网络状态机接口（AP 常驻 + STA 可选 + mDNS + 强制门户）
 *
 * @details 状态机（**AP 在任何状态下都不关闭**，这是本项目的硬性约束）：
 *          @verbatim
 *              BOOT ──▶ AP_ONLY ◀──────────────┐
 *                          │                  │ STA 断开 / 认证失败
 *                          │ 用户启用 STA      │
 *                          ▼                  │
 *                       AP_STA ───────────────┘
 *          @endverbatim
 *
 *          三重寻址兜底（Android 对 .local 解析不稳定是已知问题）：
 *            1. mDNS：http://combatbot.local
 *            2. 固定 IP：AP 模式 192.168.4.1 / STA 模式由路由器分配
 *            3. 二维码：网页右上角生成，手机扫码直达
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

#ifndef __WIFI_MGR_H
#define __WIFI_MGR_H

/* ==========================================================================
 *                              头文件引用
 * ========================================================================== */
#include <Arduino.h>

/**
 * @brief   网络工作模式
 */
enum NetMode : uint8_t {
    NET_BOOT    = 0,    /**< 启动中                                          */
    NET_AP_ONLY = 1,    /**< 仅 AP（STA 未启用/未连上/已掉线）               */
    NET_AP_STA  = 2     /**< AP + STA 并存                                   */
};

/** @brief 当前网络模式（由 wifiLoop 维护） */
extern NetMode gNetMode;

/**
 * @brief   初始化网络：启动 AP、mDNS、强制门户，并尝试连接 STA
 * @param   无
 * @return  无
 * @note    AP 密码不足 8 位时会自动降级为开放热点（WPA2 规范要求 ≥8 位）。
 */
void wifiInit();

/**
 * @brief   网络周期任务（主循环调用）
 * @param[in] now  当前毫秒时间戳
 * @return   无
 * @note     负责 STA 15s 重试与强制门户的 DNS 请求处理，均为非阻塞实现。
 */
void wifiLoop(uint32_t now);

/**
 * @brief   配置变更后立即重新尝试 STA 连接
 * @param   无
 * @return  无
 * @note    不会重启 AP，因此已连接的控制页面不会中断。
 */
void wifiApplySta();

/**
 * @brief   获取 AP 模式 IP
 * @param   无
 * @return  IP 字符串（默认 192.168.4.1）
 */
String wifiAPIP();

/**
 * @brief   获取 STA 模式 IP
 * @param   无
 * @return  IP 字符串；未连接时返回空串
 */
String wifiSTAIP();

/**
 * @brief   获取当前页面实际应该使用的 IP
 * @param   无
 * @return  STA 已连接返回 STA IP，否则返回 AP IP
 */
String wifiActiveIP();

/**
 * @brief   获取当前 STA 信号强度
 * @param   无
 * @return  RSSI，单位 dBm；未连接返回 0
 */
int8_t wifiRSSI();

/**
 * @brief   获取网络模式的可读字符串（用于网页顶栏徽章）
 * @param   无
 * @return  "AP" 或 "AP+STA"
 */
const char* wifiModeStr();

/**
 * @brief   查询强制门户是否启用
 * @param   无
 * @return  true 已启用（由配置 @ref Cfg::captive 决定）
 */
bool wifiCaptive();

#endif /* __WIFI_MGR_H */
