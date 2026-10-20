#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include "log_buffer.h"

class RuntimeLog {
public:
  static constexpr size_t kCapacity = 8192;
  size_t printf(const char *format, ...) __attribute__((format(printf, 2, 3)));
  size_t println(const char *text);
  void setRaw(bool enabled) { rawEnabled_.store(enabled); }
  bool rawEnabled() const { return rawEnabled_.load(); }
  void packet(const char *profile, const uint8_t *data, size_t length);
  size_t read(uint64_t &cursor, uint8_t *output, size_t capacity, uint64_t &lost);
  uint64_t oldest();
  uint64_t end();
  uint32_t truncated() const { return truncated_.load(); }

private:
  void record(const char *text, size_t length);
  LogBuffer<kCapacity> buffer_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  std::atomic<bool> rawEnabled_{false};
  std::atomic<uint32_t> packetSequence_{0};
  std::atomic<uint32_t> truncated_{0};
};

extern RuntimeLog AppLog;
