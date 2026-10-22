#pragma once
#include "FreeRTOS.h"
#include <chrono>
#include <thread>
#include <map>
#include <memory>

struct HostNotification {
  std::mutex mutex;
  std::condition_variable ready;
  unsigned count = 0;
};
inline thread_local std::shared_ptr<HostNotification> hostNotification;
inline std::mutex hostNotificationsMutex;
inline auto &hostNotifications() {
  static auto *registry = new std::map<void *, std::shared_ptr<HostNotification>>;
  return *registry;
}
inline void xTaskNotifyGive(TaskHandle_t task) {
  std::shared_ptr<HostNotification> notification;
  {
    std::lock_guard<std::mutex> lock(hostNotificationsMutex);
    notification = hostNotifications().at(task);
  }
  std::lock_guard<std::mutex> lock(notification->mutex);
  ++notification->count;
  notification->ready.notify_one();
}
inline uint32_t ulTaskNotifyTake(int clear, TickType_t timeout) {
  auto notification = hostNotification;
  std::unique_lock<std::mutex> lock(notification->mutex);
  if (timeout == portMAX_DELAY)
    notification->ready.wait(lock, [&] { return notification->count > 0; });
  else if (!notification->ready.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return notification->count > 0; }))
    return 0;
  const unsigned count = notification->count;
  if (clear) notification->count = 0; else --notification->count;
  return count;
}

extern bool failTaskCreation;
inline TickType_t xTaskGetTickCount() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
}
inline int xTaskCreate(void (*entry)(void *), const char *, uint32_t, void *arg,
                       unsigned, TaskHandle_t *handle) {
  if (failTaskCreation) return pdFALSE;
  auto notification = std::make_shared<HostNotification>();
  {
    std::lock_guard<std::mutex> lock(hostNotificationsMutex);
    hostNotifications()[notification.get()] = notification;
  }
  if (handle) *handle = notification.get();
  std::thread([entry, arg, notification] {
    hostNotification = notification;
    entry(arg);
    std::lock_guard<std::mutex> lock(hostNotificationsMutex);
    hostNotifications().erase(notification.get());
  }).detach();
  return pdPASS;
}
inline void vTaskDelay(TickType_t ticks) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}
inline int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, uint32_t stack,
                                  void *arg, unsigned priority, TaskHandle_t *handle, int) {
  return xTaskCreate(entry, name, stack, arg, priority, handle);
}
inline void vTaskDelete(TaskHandle_t) {}
