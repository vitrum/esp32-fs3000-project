#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

using TickType_t = uint32_t;
using portMUX_TYPE = std::mutex;
using TaskHandle_t = void *;
using QueueHandle_t = void *;
struct HostSemaphore {
  std::mutex mutex;
  std::condition_variable ready;
  bool available = false;
};
using SemaphoreHandle_t = HostSemaphore *;
constexpr int pdTRUE = 1;
constexpr int pdFALSE = 0;
constexpr int pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define portMUX_INITIALIZER_UNLOCKED {}
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
