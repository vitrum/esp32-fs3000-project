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
inline void vTaskDelete(TaskHandle_t) {}
