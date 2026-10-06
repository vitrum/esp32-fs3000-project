#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BME280.h>

// 可选环境传感器：BME280 温度/气压 → 空气密度
// ρ = P_atm / (287.05 · T_K)，单位 kg/m³
// 无 BME280 时返回标准海平面密度 1.225 kg/m³
class EnvSensor {
public:
  bool begin(TwoWire &w, uint8_t addr = BME280_ADDR);
  bool present() const { return _ok; }
  float rhoKgM3();                  // 每 10s 刷新一次
  float temperatureC();

private:
  Adafruit_BME280 _bme;
  bool   _ok = false;
  float  _rho = 1.225f;
  float  _temp = 20.0f;
  uint32_t _lastMs = 0;

  void update();
};
