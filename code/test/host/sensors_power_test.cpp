#include "fs3000.h"
#include "wind_probe.h"
#include "bme280_env.h"
#include "power_key.h"
#include "runtime_log.h"
#include "../../src/audio.h"
#include <driver/i2s.h>
#include <cassert>
#include <iostream>

std::atomic<uint32_t> clockOffset{0};
HostSerial Serial;
bool failTaskCreation = false;
bool sdCardReady() { return false; }
bool lockSdCard(TickType_t) { return false; }
void unlockSdCard() {}
fs::FS &sdFs() { static fs::FS filesystem; return filesystem; }

static void sensorTests() {
  TwoWire wire;
  FS3000 sensor;
  float speed = -1;
  assert(!sensor.readMps(speed) && speed == -1);
  wire.ack = false;
  assert(!sensor.begin(wire));
  wire.ack = true;
  assert(sensor.begin(wire));
  const uint16_t counts[] = {0, 409, 1203, 1597, 1908, 2187, 2400, 2629, 2801, 3006, 3178, 3309, 3563, 3686, 4095};
  const float expected[] = {0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 15};
  for (size_t index = 0; index < sizeof(counts) / sizeof(counts[0]); ++index) {
    wire.setCount(counts[index]);
    assert(sensor.readMps(speed) && std::abs(speed - expected[index]) < 0.0001);
    assert(sensor.lastRaw() == counts[index] && sensor.lastBytesReceived() == 5);
  }
  wire.setCount(806);
  assert(sensor.readMps(speed) && std::abs(speed - 1) < 0.0001);
  float previous = -1;
  const uint16_t calibration[] = {409, 1203, 1597, 1908, 2187, 2400, 2629, 2801, 3006, 3178, 3309, 3563, 3686};
  const float velocities[] = {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15};
  for (uint16_t raw = 0; raw < 4096; ++raw) {
    float expectedSpeed = raw >= 3686 ? 15 : 0;
    for (size_t interval = 1; interval < 13; ++interval) {
      if (raw > calibration[interval - 1] && raw <= calibration[interval]) {
        expectedSpeed = velocities[interval - 1] +
            float(raw - calibration[interval - 1]) * (velocities[interval] - velocities[interval - 1]) /
            float(calibration[interval] - calibration[interval - 1]);
        break;
      }
    }
    wire.setCount(raw);
    assert(sensor.readMps(speed));
    assert(std::isfinite(speed) && speed >= previous && speed >= 0 && speed <= 15);
    assert(std::abs(speed - expectedSpeed) < 0.00001);
    previous = speed;
  }
  wire.setCount(0xF000 | 1203);
  assert(sensor.readMps(speed) && sensor.lastRaw() == 1203 && speed == 2);
  speed = 42;
  wire.frame = {0};
  assert(!sensor.readMps(speed) && speed == 42 && sensor.lastError() == FS3000::ReadError::ShortRead);
  wire.setCount(1203);
  assert(sensor.readMps(speed));
  for (unsigned failure = 0; failure < 8; ++failure) {
    wire.setCount(1203);
    ++wire.frame[0];
    assert(!sensor.readMps(speed));
  }
  assert(!sensor.present() && sensor.checksumErrors() == 8 && sensor.shortReads() == 1);
  assert(!sensor.readMps(speed) && sensor.lastError() == FS3000::ReadError::NotPresent);
  assert(sensor.begin(wire));
  wire.setCount(1203);
  assert(sensor.readMps(speed));
  std::cout << "PASS FS3000: all 4096 raw codes, calibration/interpolation, monotonic range, 12-bit mask, checksum, retained value on short frame, offline threshold, reprobe\n";
}

static void windTests() {
  TwoWire left, right;
  WindProbe probe;
  WindVector vector;
  assert(!probe.read(vector));
  probe.begin(left, right, 45);
  left.setCount(1203);
  right.setCount(1203);
  assert(probe.read(vector));
  assert(std::abs(vector.vAir - 2 * std::sqrt(2.0)) < 0.0001 && std::abs(vector.yawDeg) < 0.0001);
  left.setCount(1908);
  assert(probe.read(vector) && vector.yawDeg > 0);
  const float yaw = vector.yawDeg;
  left.setCount(1203);
  right.setCount(1908);
  assert(probe.read(vector) && std::abs(vector.yawDeg + yaw) < 0.0001);
  right.frame.clear();
  assert(!probe.read(vector));
  std::cout << "PASS wind: initialization, symmetric flow, signed yaw, missing frame\n";
}

static void environmentTests() {
  TwoWire wire;
  EnvSensor sensor;
  Adafruit_BME280::available = false;
  assert(!sensor.begin(wire) && std::abs(sensor.rhoKgM3() - 1.225f) < 0.0001);
  Adafruit_BME280::available = true;
  assert(sensor.begin(wire));
  const float expected = 101325 / (287.05f * 293.15f);
  assert(std::abs(sensor.rhoKgM3() - expected) < 0.0001);
  const unsigned reads = Adafruit_BME280::reads;
  for (unsigned index = 0; index < 20; ++index) sensor.rhoKgM3();
  assert(Adafruit_BME280::reads == reads);
  Adafruit_BME280::pressure = -1;
  clockOffset += 10001;
  assert(std::abs(sensor.rhoKgM3() - expected) < 0.0001);
  Adafruit_BME280::pressure = 100000;
  Adafruit_BME280::temperature = 60;
  clockOffset += 10001;
  assert(std::abs(sensor.rhoKgM3() - expected) < 0.0001);
  Adafruit_BME280::temperature = 25;
  clockOffset += 10001;
  assert(std::abs(sensor.rhoKgM3() - 100000 / (287.05f * 298.15f)) < 0.0001);
  std::cout << "PASS environment: absent fallback, density formula, refresh limit, invalid reading retention\n";
}

static void powerTests() {
  PowerKey key;
  hostPins[PIN_PWR_KEY] = HIGH;
  key.begin();
  assert(hostPins[PIN_PWR_CONTROL] == LOW);
  hostPins[PIN_PWR_KEY] = LOW;
  key.tick();
  assert(hostPins[PIN_PWR_CONTROL] == HIGH);
  hostPins[PIN_PWR_KEY] = HIGH;
  key.tick();
  clockOffset += PWR_LONG_PRESS_MS + 1;
  key.tick();
  assert(!key.takeShutdownRequest());
  hostPins[PIN_PWR_KEY] = LOW;
  key.tick();
  clockOffset += PWR_LONG_PRESS_MS + 1;
  key.tick();
  assert(key.takeShutdownRequest() && !key.takeShutdownRequest());
  key.shutdown();
  assert(key.shuttingDown() && hostPins[PIN_PWR_CONTROL] == LOW && hostPins[PIN_LCD_BL] == LOW);
  key.tick();
  assert(!key.takeShutdownRequest());
  PowerKey boot;
  boot.begin();
  assert(hostPins[PIN_PWR_CONTROL] == HIGH);
  std::cout << "PASS power key: boot latch, short press, long press, one-shot request, shutdown outputs\n";
}

int main() {
  sensorTests();
  windTests();
  environmentTests();
  powerTests();
  assert(!audio.begin() && !audio.enabled());
  assert(!audio.playFile("/song.mp3") && !audio.playFile(nullptr));
  audio.playStartupChime();
  audio.playShutdownChime();
  audio.playBeepPreview();
  audio.pauseResume();
  audio.tick();
  audio.setVolume(255);
  assert(audio.volume() == 100 && !audio.playing());
  audio.setTitle("disabled track");
  assert(std::string(audio.currentTitle()) == "disabled track");
  assert(audio.stop() && audio.releasePlayback());
  assert(std::string(audio.currentTitle()).empty() && hostI2sInstalls == 0);
  std::cout << "PASS audio disabled: no decoder/I2S, rejected playback, bounded volume, title reset\n";
}