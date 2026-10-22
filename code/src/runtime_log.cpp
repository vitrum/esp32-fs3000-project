#include "runtime_log.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#ifdef ARDUINO_ARCH_ESP32
#include <freertos/task.h>
#endif

RuntimeLog AppLog;

bool RuntimeLog::begin() {
  if (outputStarted_) return true;
#ifdef ARDUINO_ARCH_ESP32
  if (xTaskCreate([](void *context) {
        auto *log = static_cast<RuntimeLog *>(context);
        for (;;) {
          log->flushSerial();
          vTaskDelay(pdMS_TO_TICKS(10));
        }
      }, "logOutput", 3072, this, 1, nullptr) != pdPASS) return false;
#endif
  outputStarted_ = true;
  return true;
}

void RuntimeLog::flushSerial() {
  const int available = Serial.availableForWrite();
  if (available <= 0) return;
  uint8_t text[128];
  const size_t capacity = size_t(available) < sizeof(text) ? size_t(available) : sizeof(text);
  uint64_t next = serialCursor_;
  uint64_t lost = 0;
  portENTER_CRITICAL(&mux_);
  const size_t length = serialBuffer_.read(next, text, capacity, lost);
  portEXIT_CRITICAL(&mux_);
  serialCursor_ += lost;
  serialLostBytes_.fetch_add(uint32_t(lost));
  if (length) serialCursor_ += Serial.write(text, length);
}

size_t RuntimeLog::printf(const char *format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  const int result = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (result < 0) {
    truncated_.fetch_add(1);
    return println("[LOG] formatting failed");
  }
  size_t length = size_t(result);
  if (length >= sizeof(text)) {
    static const char marker[] = "...[TRUNCATED]\n";
    length = sizeof(text) - 1;
    memcpy(text + length - (sizeof(marker) - 1), marker, sizeof(marker) - 1);
    truncated_.fetch_add(1);
  }
  record(text, length);
  portENTER_CRITICAL(&mux_);
  serialBuffer_.append(reinterpret_cast<const uint8_t *>(text), length);
  portEXIT_CRITICAL(&mux_);
  return length;
}

void RuntimeLog::record(const char *text, size_t length) {
  char prefix[24];
  const int prefixLength = snprintf(prefix, sizeof(prefix), "[%lu] ", (unsigned long)millis());
  // Keep each log record contiguous across producers.
  portENTER_CRITICAL(&mux_);
  buffer_.append(reinterpret_cast<const uint8_t *>(prefix), size_t(prefixLength));
  buffer_.append(reinterpret_cast<const uint8_t *>(text), length);
  portEXIT_CRITICAL(&mux_);
}

size_t RuntimeLog::println(const char *text) {
  return printf("%s\n", text);
}

void RuntimeLog::packet(const char *profile, const uint8_t *data, size_t length) {
  if (!rawEnabled()) return;
  const uint32_t sequence = packetSequence_.fetch_add(1) + 1;
  if (!data || length == 0) {
    char line[128];
    const int size = snprintf(line, sizeof(line), "[RAW] seq=%lu %.24s len=%u EMPTY\n",
                              (unsigned long)sequence, profile, (unsigned)length);
    record(line, size_t(size));
    return;
  }
  const size_t captured = length > 512 ? 512 : length;
  for (size_t offset = 0; offset < captured; offset += 32) {
    char hex[65];
    const size_t count = captured - offset < 32 ? captured - offset : 32;
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < count; ++i) {
      hex[i * 2] = digits[data[offset + i] >> 4];
      hex[i * 2 + 1] = digits[data[offset + i] & 15];
    }
    hex[count * 2] = '\0';
    char line[160];
    const int size = snprintf(line, sizeof(line), "[RAW] seq=%lu %.24s len=%u off=%u hex=%s\n",
                              (unsigned long)sequence, profile, (unsigned)length, (unsigned)offset, hex);
    record(line, size_t(size));
  }
  if (captured < length) {
    truncated_.fetch_add(1);
    static const char marker[] = "[RAW] TRUNCATED at 512 bytes\n";
    record(marker, sizeof(marker) - 1);
  }
}

size_t RuntimeLog::read(uint64_t &cursor, uint8_t *output, size_t capacity, uint64_t &lost) {
  portENTER_CRITICAL(&mux_);
  const size_t length = buffer_.read(cursor, output, capacity, lost);
  portEXIT_CRITICAL(&mux_);
  return length;
}

uint64_t RuntimeLog::oldest() {
  portENTER_CRITICAL(&mux_);
  const uint64_t value = buffer_.oldest();
  portEXIT_CRITICAL(&mux_);
  return value;
}

uint64_t RuntimeLog::end() {
  portENTER_CRITICAL(&mux_);
  const uint64_t value = buffer_.end();
  portEXIT_CRITICAL(&mux_);
  return value;
}
