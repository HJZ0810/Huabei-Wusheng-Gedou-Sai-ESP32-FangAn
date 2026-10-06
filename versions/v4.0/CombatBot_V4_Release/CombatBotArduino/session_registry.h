/**
 * @file    session_registry.h
 * @brief   维护 WebSocket 连接与随机会话凭据的有限映射。
 *
 * 设计约定：凭据由网络模块生成，仅通过当前连接的 hello 消息单播。
 * 同步边界：本类不加锁；固件调用方须在同一临界区中访问全部方法。
 * 安全边界：凭据证明请求与连接的关联，不提供用户身份或账户认证。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace bot {
class SessionRegistry {
 public:
  static constexpr size_t Capacity = 4;
  static constexpr size_t TokenLength = 32;

  /** @brief 注册新连接；重复编号、凭据碰撞或容量不足均拒绝。 */
  bool add(uint32_t client, const char* token) {
    if (!client || !validToken(token) || find(token) != 0) return false;
    for (const auto& slot : slots_) if (slot.client == client) return false;
    for (auto& slot : slots_) {
      if (slot.client) continue;
      slot.client = client;
      memcpy(slot.token, token, TokenLength + 1);
      return true;
    }
    return false;
  }

  /** @brief 按凭据查询连接编号；未注册或格式无效时返回 0。 */
  uint32_t find(const char* token) const {
    if (!validToken(token)) return 0;
    for (const auto& slot : slots_) {
      // 固定长度比较，避免凭据内容决定单个槽位比较的提前结束位置。
      unsigned difference = 0;
      for (size_t i = 0; i < TokenLength; ++i)
        difference |= static_cast<unsigned char>(slot.token[i]) ^ static_cast<unsigned char>(token[i]);
      if (slot.client && !difference) return slot.client;
    }
    return 0;
  }

  /** @brief 检查连接是否仍有有效会话。 */
  bool contains(uint32_t client) const {
    for (const auto& slot : slots_) if (slot.client == client && client) return true;
    return false;
  }

  /** @brief 撤销断开连接的凭据；重复撤销允许且不会影响其他会话。 */
  void remove(uint32_t client) {
    for (auto& slot : slots_) if (slot.client == client) slot = Slot{};
  }

 private:
  struct Slot { uint32_t client = 0; char token[TokenLength + 1] = {}; };
  Slot slots_[Capacity];
  static bool validToken(const char* token) {
    if (!token || strlen(token) != TokenLength) return false;
    for (size_t i = 0; i < TokenLength; ++i)
      if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) return false;
    return true;
  }
};
}
