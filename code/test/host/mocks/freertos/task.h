#pragma once
#include "FreeRTOS.h"
#include <chrono>
#include <thread>

extern bool failTaskCreation;
inline int xTaskCreate(void (*entry)(void *), const char *, uint32_t, void *arg,
                       unsigned, TaskHandle_t *handle) {
  if (failTaskCreation) return pdFALSE;
  if (handle) *handle = arg;
  std::thread(entry, arg).detach();
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
