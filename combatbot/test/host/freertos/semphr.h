/**
 * @file semphr.h
 * @brief 使用 std::mutex 提供配置测试的互斥锁接口。
 *
 * ============================================================================
 * 保留加锁与解锁行为，但忽略等待时限，不验证 FreeRTOS 优先级继承。
 * ============================================================================
 */
#pragma once
#include "FreeRTOS.h"
#include <mutex>
using SemaphoreHandle_t=std::mutex*;
constexpr uint32_t portMAX_DELAY=0xffffffff;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return new std::mutex;}
inline int xSemaphoreTake(SemaphoreHandle_t lock,uint32_t){lock->lock();return pdTRUE;}
inline void xSemaphoreGive(SemaphoreHandle_t lock){lock->unlock();}
