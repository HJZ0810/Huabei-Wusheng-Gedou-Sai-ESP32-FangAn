/**
 * @file queue.h
 * @brief 使用 STL 容器模拟定长 FIFO 命令队列。
 *
 * ============================================================================
 * 条目按字节复制；不提供阻塞等待、线程同步或真实 FreeRTOS 调度语义。
 * ============================================================================
 */
#pragma once
#include "FreeRTOS.h"
#include <cstddef>
#include <cstring>
#include <deque>
#include <vector>
struct HostQueue { size_t limit,itemSize; std::deque<std::vector<unsigned char>> items; };
using QueueHandle_t=HostQueue*;
inline QueueHandle_t xQueueCreate(size_t length,size_t itemSize) { return new HostQueue{length,itemSize,{}}; }
inline int xQueueSend(QueueHandle_t q,const void* item,TickType_t) {
  if(q->items.size()>=q->limit) return 0;
  q->items.emplace_back(q->itemSize); std::memcpy(q->items.back().data(),item,q->itemSize); return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t q,void* item,TickType_t) {
  if(q->items.empty()) return 0;
  std::memcpy(item,q->items.front().data(),q->itemSize); q->items.pop_front(); return pdTRUE;
}
