#pragma once

#include <stdint.h>
#include <stddef.h>
#include <cstdio>

class RecentSensorReading {
public:
  void capture(const char *text, uint32_t receivedMs) {
    snprintf(text_, sizeof(text_), "%s", text);
    receivedMs_ = receivedMs;
    present_ = true;
  }
  void clear() { present_ = false; }
  void format(uint32_t now, char *out, size_t capacity) const {
    if (!present_) snprintf(out, capacity, "--");
    else snprintf(out, capacity, "%s\n%lu ms", text_, (unsigned long)(now - receivedMs_));
  }
private:
  char text_[128] = {};
  uint32_t receivedMs_ = 0;
  bool present_ = false;
};