#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include "freertos/semphr.h"
#include "freertos/task.h"

class String : public std::string {
public:
  using std::string::string;
  using std::string::operator=;
  String() = default;
  String(const std::string &value) : std::string(value) {}
  explicit String(unsigned long value) : std::string(std::to_string(value)) {}
  bool isEmpty() const { return empty(); }
  bool startsWith(const char *prefix) const { return rfind(prefix, 0) == 0; }
  bool endsWith(const char *suffix) const {
    const size_t length = strlen(suffix);
    return size() >= length && compare(size() - length, length, suffix) == 0;
  }
  int indexOf(char value) const { const auto at = find(value); return at == npos ? -1 : int(at); }
  int indexOf(const char *value) const { const auto at = find(value); return at == npos ? -1 : int(at); }
  void remove(size_t at, size_t count) { erase(at, count); }
  long toInt() const { return strtol(c_str(), nullptr, 10); }
};
#define PROGMEM
using byte = uint8_t;
constexpr int LOW = 0, HIGH = 1, OUTPUT = 1, INPUT_PULLUP = 2;
constexpr float PI = 3.14159265358979323846f;
inline int hostPins[64] = {};
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int value) { hostPins[pin] = value; }
inline int digitalRead(int pin) { return hostPins[pin]; }
inline void delay(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
extern std::atomic<uint32_t> clockOffset;
inline uint32_t millis() {
  static const auto start = std::chrono::steady_clock::now();
  return 1 + clockOffset.load() +
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start).count();
}
inline double radians(double v) { return v * 0.017453292519943295; }
inline double degrees(double v) { return v / 0.017453292519943295; }
inline double sq(double v) { return v * v; }
constexpr double TWO_PI = 6.283185307179586;
struct HostSerial {
  std::atomic<unsigned> messages{0};
  template <typename... Args> void printf(const char *, Args...) { ++messages; }
  void println(const char *) { ++messages; }
  int writable = 128;
  int availableForWrite() const { return writable; }
  size_t write(const uint8_t *, size_t length) { ++messages; return length; }
};
extern HostSerial Serial;
