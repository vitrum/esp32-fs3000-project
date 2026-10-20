#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using String = std::string;
using byte = uint8_t;
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
  size_t write(const uint8_t *, size_t length) { ++messages; return length; }
};
extern HostSerial Serial;
