/**
 * @file Arduino.h
 * @brief 主机测试所需的 Arduino 类型、虚拟时间与临界区替身。
 *
 * ============================================================================
 * String 使用 std::string；临界区宏为空操作，不提供真实跨线程保护。
 * ============================================================================
 */
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
using String=std::string;
extern uint32_t hostMillis;
inline uint32_t millis() { return hostMillis; }
struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
