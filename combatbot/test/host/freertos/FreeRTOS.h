/**
 * @file FreeRTOS.h
 * @brief 提供主机测试所需的 FreeRTOS 类型与成功状态常量。
 *
 * ============================================================================
 * 测试约定一个 tick 对应虚拟 1 ms，不代表目标硬件的实际 tick 配置。
 * ============================================================================
 */
#pragma once
#include <cstdint>
using TickType_t=uint32_t;
constexpr int pdTRUE=1,pdPASS=1;
inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
