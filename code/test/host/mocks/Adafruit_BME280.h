#pragma once
#include "Wire.h"
class Adafruit_BME280 {
public:
  bool begin(uint8_t, TwoWire *) { return available; }
  float readPressure() const { ++reads; return pressure; }
  float readTemperature() const { return temperature; }
  inline static bool available = true;
  inline static float pressure = 101325, temperature = 20;
  inline static unsigned reads = 0;
};