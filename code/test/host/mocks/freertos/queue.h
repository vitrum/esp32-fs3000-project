#pragma once
#include "FreeRTOS.h"
#include <chrono>
#include <cstring>
#include <deque>
#include <vector>

struct HostQueue {
  size_t capacity;
  size_t itemSize;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::vector<uint8_t>> items;
  HostQueue(size_t capacity, size_t itemSize) : capacity(capacity), itemSize(itemSize) {}
};

inline QueueHandle_t xQueueCreate(size_t capacity, size_t itemSize) {
  return new HostQueue(capacity, itemSize);
}
inline int xQueueSend(QueueHandle_t handle, const void *item, TickType_t ticks) {
  auto *queue = static_cast<HostQueue *>(handle);
  std::unique_lock<std::mutex> lock(queue->mutex);
  auto ready = [&] { return queue->items.size() < queue->capacity; };
  if (ticks == portMAX_DELAY) queue->changed.wait(lock, ready);
  else if (!queue->changed.wait_for(lock, std::chrono::milliseconds(ticks), ready)) return pdFALSE;
  const auto *bytes = static_cast<const uint8_t *>(item);
  queue->items.emplace_back(bytes, bytes + queue->itemSize);
  queue->changed.notify_all();
  return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t handle, void *item, TickType_t ticks) {
  auto *queue = static_cast<HostQueue *>(handle);
  std::unique_lock<std::mutex> lock(queue->mutex);
  auto ready = [&] { return !queue->items.empty(); };
  if (ticks == portMAX_DELAY) queue->changed.wait(lock, ready);
  else if (!queue->changed.wait_for(lock, std::chrono::milliseconds(ticks), ready)) return pdFALSE;
  memcpy(item, queue->items.front().data(), queue->itemSize);
  queue->items.pop_front();
  queue->changed.notify_all();
  return pdTRUE;
}
inline void vQueueDelete(QueueHandle_t handle) { delete static_cast<HostQueue *>(handle); }