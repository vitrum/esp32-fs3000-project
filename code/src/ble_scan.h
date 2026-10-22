#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>
#include <freertos/semphr.h>

enum class BleScanState : uint8_t { Idle, Starting, Scanning, Stopping, Complete, Cancelled, Failed };

class BleCancellation {
public:
  BleCancellation(const std::atomic<uint32_t> &generation, uint32_t expected)
      : generation_(generation), expected_(expected) {}
  bool cancelled() const { return generation_.load() != expected_; }
private:
  const std::atomic<uint32_t> &generation_;
  uint32_t expected_;
};

struct BleScanPolicy {
  uint16_t intervalMs;
  uint16_t windowMs;
  bool active;

  static BleScanPolicy select(bool manual, bool connected) {
    return {100, uint16_t(connected ? 30 : (manual ? 60 : 20)), manual};
  }
};

class BleScanner : public NimBLEScanCallbacks {
public:
  bool run(uint32_t durationMs, bool manual, const BleCancellation *cancel = nullptr);
  void setConnected(bool connected) { connected_.store(connected); }
  BleScanState state() const { return state_.load(); }
  const char *error() const { return error_; }
  uint32_t failures() const { return failures_.load(); }
  uint32_t elapsedMs() const { return elapsedMs_.load(); }
  void onScanEnd(const NimBLEScanResults &, int reason) override;

private:
  bool fail(const char *message);
  SemaphoreHandle_t completed_ = nullptr;
  std::atomic<BleScanState> state_{BleScanState::Idle};
  std::atomic<bool> connected_{false};
  std::atomic<int> reason_{0};
  std::atomic<uint32_t> failures_{0};
  std::atomic<uint32_t> elapsedMs_{0};
  char error_[120] = {};
};

extern BleScanner bleScanner;