#pragma once

#include "Arduino.h"
#include "Wire.h"

class SensorQMI8658 {
public:
  enum { ACC_RANGE_16G, ACC_ODR_125Hz, LPF_OFF, GYR_RANGE_1024DPS, GYR_ODR_112_1Hz };
  bool init(TwoWire &, int sda, int scl, uint8_t address) {
    assert(sda == -1 && scl == -1 && address == 0x6B);
    return available;
  }
  int configAccelerometer(int, int, int) { return configError; }
  int configGyroscope(int, int, int) { return configError; }
  bool enableAccelerometer() { return available; }
  bool enableGyroscope() { return available; }
  bool getDataReady() { return dataReady; }
  bool getAccelerometer(float &x, float &y, float &z) { x = 0; y = 0; z = 1; return readOk; }
  bool getGyroscope(float &x, float &y, float &z) { x = 3; y = 4; z = 0; return readOk; }
  inline static bool available = true, dataReady = true, readOk = true;
  inline static int configError = 0;
};