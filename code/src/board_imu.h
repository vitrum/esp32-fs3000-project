#pragma once

#include <SensorQMI8658.hpp>
#include <cmath>
#include "imu_sample.h"

class BoardImu {
public:
  bool begin(TwoWire &wire) {
    ready_ = sensor_.init(wire, -1, -1, 0x6B) &&
        sensor_.configAccelerometer(SensorQMI8658::ACC_RANGE_16G,
            SensorQMI8658::ACC_ODR_125Hz, SensorQMI8658::LPF_OFF) == 0 &&
        sensor_.configGyroscope(SensorQMI8658::GYR_RANGE_1024DPS,
            SensorQMI8658::GYR_ODR_112_1Hz, SensorQMI8658::LPF_OFF) == 0 &&
        sensor_.enableAccelerometer() && sensor_.enableGyroscope();
    sample_ = ImuSample{};
    return ready_;
  }

  void poll(uint32_t now) {
    if (!ready_ || uint32_t(now - lastPollMs_) < 10) return;
    lastPollMs_ = now;
    if (!sensor_.getDataReady()) return;
    ImuSample next;
    if (!sensor_.getAccelerometer(next.accelerationG[0], next.accelerationG[1], next.accelerationG[2]) ||
        !sensor_.getGyroscope(next.angularVelocityDps[0], next.angularVelocityDps[1], next.angularVelocityDps[2])) {
      sample_.valid = false;
      return;
    }
    for (unsigned axis = 0; axis < 3; ++axis)
      if (!std::isfinite(next.accelerationG[axis]) || !std::isfinite(next.angularVelocityDps[axis])) {
        sample_.valid = false;
        return;
      }
    next.receivedMs = millis();
    next.sequence = sample_.sequence + 1;
    next.valid = true;
    sample_ = next;
  }

  ImuSample sample(uint32_t now) const {
    ImuSample result = sample_;
    result.valid = result.valid && uint32_t(now - result.receivedMs) <= 250;
    return result;
  }
  bool ready() const { return ready_; }

private:
  SensorQMI8658 sensor_;
  ImuSample sample_;
  uint32_t lastPollMs_ = 0;
  bool ready_ = false;
};