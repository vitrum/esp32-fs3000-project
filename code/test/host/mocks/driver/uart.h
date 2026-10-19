#pragma once
#include <deque>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

using uart_port_t = int;
constexpr int UART_NUM_2 = 2;
constexpr int UART_DATA_8_BITS = 8;
constexpr int UART_PARITY_DISABLE = 0;
constexpr int UART_STOP_BITS_1 = 1;
constexpr int UART_HW_FLOWCTRL_DISABLE = 0;
constexpr int UART_SCLK_APB = 0;
constexpr int UART_PIN_NO_CHANGE = -1;
constexpr int UART_FIFO_OVF = 1;
constexpr int UART_BUFFER_FULL = 2;
struct uart_config_t {
  int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
};
struct uart_event_t { int type; };
extern std::mutex uartMutex;
extern std::deque<uint8_t> uartBytes;
extern std::deque<uart_event_t> uartEvents;
extern bool uartInstalled;
extern int uartCapacity;
extern int failUartInit;
inline int uart_param_config(int, const uart_config_t *) { return failUartInit; }
inline int uart_set_pin(int, int, int, int, int) { return ESP_OK; }
inline int uart_driver_install(int, int capacity, int, int, QueueHandle_t *queue, int) {
  uartInstalled = true;
  uartCapacity = capacity;
  *queue = &uartEvents;
  return ESP_OK;
}
inline int uart_driver_delete(int) {
  std::lock_guard<std::mutex> lock(uartMutex);
  uartInstalled = false;
  uartBytes.clear();
  uartEvents.clear();
  return ESP_OK;
}
inline int uart_read_bytes(int, uint8_t *out, size_t length, TickType_t) {
  std::lock_guard<std::mutex> lock(uartMutex);
  size_t count = 0;
  while (count < length && !uartBytes.empty()) {
    out[count++] = uartBytes.front();
    uartBytes.pop_front();
  }
  return (int)count;
}
inline int xQueueReceive(QueueHandle_t, uart_event_t *event, TickType_t) {
  std::lock_guard<std::mutex> lock(uartMutex);
  if (uartEvents.empty()) return pdFALSE;
  *event = uartEvents.front();
  uartEvents.pop_front();
  return pdTRUE;
}
