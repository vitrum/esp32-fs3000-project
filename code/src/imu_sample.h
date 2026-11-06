#pragma once

#include <stdint.h>

struct ImuSample {
  float accelerationG[3] = {};
  float angularVelocityDps[3] = {};
  uint32_t receivedMs = 0;
  uint32_t sequence = 0;
  bool valid = false;
};