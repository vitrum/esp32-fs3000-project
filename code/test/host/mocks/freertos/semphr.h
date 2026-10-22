#pragma once
#include "FreeRTOS.h"

inline SemaphoreHandle_t xSemaphoreCreateBinary() { return new HostSemaphore; }
inline void vSemaphoreDelete(SemaphoreHandle_t semaphore) { delete semaphore; }
inline int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks) {
  std::unique_lock<std::mutex> lock(semaphore->mutex);
  if (ticks == portMAX_DELAY)
    semaphore->ready.wait(lock, [&] { return semaphore->available; });
  else if (!semaphore->ready.wait_for(lock, std::chrono::milliseconds(ticks),
                                     [&] { return semaphore->available; }))
    return pdFALSE;
  semaphore->available = false;
  return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t semaphore) {
  std::lock_guard<std::mutex> lock(semaphore->mutex);
  semaphore->available = true;
  semaphore->ready.notify_all();
}
