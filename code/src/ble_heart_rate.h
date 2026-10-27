#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>

class HeartRateMeasurement {
public:
  bool accept(const uint8_t *data, size_t length, uint32_t received) {
    if (!data || length < 2 || (data[0] & 0xe0)) return false;
    const uint8_t flags = data[0];
    size_t position = flags & 1 ? 3 : 2;
    if (length < position) return false;
    const uint16_t bpm = data[1] | ((flags & 1) ? uint16_t(data[2]) << 8 : 0);
    if (flags & 8) position += 2;
    if (length < position) return false;
    if (flags & 16) {
      if (length - position < 2 || ((length - position) & 1)) return false;
    } else if (length != position) return false;
    bpm_.store(bpm);
    received_.store(received);
    valid_.store(bpm != 0 && (!(flags & 4) || (flags & 2)));
    return true;
  }
  bool valid(uint32_t now) const { return valid_.load() && now - received_.load() <= 5000; }
  uint16_t bpm() const { return bpm_.load(); }
  void invalidate() { valid_.store(false); }
private:
  std::atomic<uint16_t> bpm_{0};
  std::atomic<uint32_t> received_{0};
  std::atomic<bool> valid_{false};
};

class BleHeartRate {
public:
  void begin(const char *name);
  bool connectDevice(const char *address, uint8_t type);
  void disconnect();
  void processData();
  bool connected() const { return connected_.load(); }
  bool valid(uint32_t now) const { return connected() && measurement_.valid(now); }
  uint16_t bpm() const { return measurement_.bpm(); }
  const char *lastError() const { return error_; }
private:
  friend class HeartRateCallbacks;
  static void notify(NimBLERemoteCharacteristic *, uint8_t *, size_t, bool);
  static std::atomic<bool> connected_;
  static HeartRateMeasurement measurement_;
  static NimBLEClient *client_;
  static char error_[128];
};