#include "bme280_env.h"
#include "config.h"

bool EnvSensor::begin(TwoWire &w, uint8_t addr) {
  _ok = _bme.begin(addr, &w);
  if (_ok) update();
  return _ok;
}

void EnvSensor::update() {
  float p = _bme.readPressure();       // Pa
  float t = _bme.readTemperature();    // °C
  if (p > 0 && t > -40.0f && t < 60.0f) {
    _temp = t;
    _rho  = p / (287.05f * (t + 273.15f));
  }
}

float EnvSensor::rhoKgM3() {
  if (!_ok) return 1.225f;
  if (millis() - _lastMs >= 10000) {
    _lastMs = millis();
    update();
  }
  return _rho;
}
