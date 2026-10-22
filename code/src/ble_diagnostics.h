#pragma once

#include <Arduino.h>
#include <atomic>
#include "ble_inbox.h"

class NimBLEServer;
class NimBLECharacteristic;

class BleDiagnostics {
public:
  bool begin();
  // Must be called on the main loop before destroying the shared NimBLE stack.
  void suspend();
  bool setSensorBusy(bool busy);
  bool connected() const { return peer_.load() != UINT16_MAX; }
  void setByteRate(uint16_t rate) { byteRate_ = rate; }
  void tick();

private:
  class ServerCallbacks;
  class TxCallbacks;
  class CommandCallbacks;
  void status(char *text, size_t capacity) const;
  void processCommand(const uint8_t *data, size_t length);
  BleInbox<8, 32> commands_;
  uint32_t reportedCommandDrops_ = 0;
  NimBLEServer *server_ = nullptr;
  NimBLECharacteristic *tx_ = nullptr;
  std::atomic<uint16_t> peer_{UINT16_MAX};
  std::atomic<bool> subscribed_{false};
  std::atomic<bool> paused_{false};
  std::atomic<uint32_t> replay_{0};
  std::atomic<bool> live_{false};
  std::atomic<uint32_t> notifyFailures_{0};
  std::atomic<uint32_t> lostBytes_{0};
  uint32_t lastReplay_ = 0;
  uint32_t lastSendMs_ = 0;
  uint32_t lastAdvertiseMs_ = 0;
  uint32_t lastErrorMs_ = 0;
  uint64_t cursor_ = 0;
  uint64_t pendingCursor_ = 0;
  uint8_t pending_[180] = {};
  size_t pendingLength_ = 0;
  bool sensorBusy_ = false;
  uint16_t byteRate_ = 1500;
  uint32_t creditMilliBytes_ = 180000;
  uint32_t lastBudgetMs_ = 0;
  uint32_t retryIntervalMs_ = 20;
  bool running_ = false;
};

extern BleDiagnostics bleDiagnostics;
