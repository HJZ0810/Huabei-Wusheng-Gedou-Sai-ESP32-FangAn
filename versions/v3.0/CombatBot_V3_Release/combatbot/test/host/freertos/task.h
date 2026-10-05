/**
 * @file task.h
 * @brief 记录真实控制任务入口，并在单个周期结束时交回测试场景。
 *
 * ============================================================================
 * 仅执行测试主动推进的任务调用，不创建线程，也不模拟抢占与调度延迟。
 * ============================================================================
 */
#pragma once
#include "FreeRTOS.h"
#include "Arduino.h"
struct HostTickComplete {};
extern void (*hostTask)(void*);
inline TickType_t xTaskGetTickCount() { return hostMillis; }
inline int xTaskCreatePinnedToCore(void (*fn)(void*),const char*,uint32_t,void*,int,void*,int) { hostTask=fn; return pdPASS; }
// 发布一个周期后结束本次调用；生产模块的静态状态继续保留，供下一场景推进。
inline void vTaskDelayUntil(TickType_t*,TickType_t) { throw HostTickComplete{}; }
