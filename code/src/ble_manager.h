#pragma once

#include "ble_power.h"
#include "ble_posture.h"
#include "ble_scan.h"
#include <freertos/queue.h>
#include <freertos/semphr.h>

enum class BleOpKind : uint8_t { Scan, Connect, Posture, Stop };
enum class BleManagerState : uint8_t { Idle, Scanning, Connecting, Posture, Suspended };

struct BleCommand {
  BleOpKind kind = BleOpKind::Scan;
  char address[18] = {};
  uint8_t addressType = 0xFF;
  uint32_t generation = 0;
};

struct BleOutcome {
  BleOpKind kind = BleOpKind::Scan;
  bool success = false;
  bool cancelled = false;
  char address[18] = {};
  uint8_t addressType = 0xFF;
  size_t deviceCount = 0;
  BlePowerDevice devices[kBleDeviceCapacity] = {};
  char error[320] = {};
};

class BleManager {
public:
  BleManager(BlePowerMeter &power, BlePosture &posture) : power_(power), posture_(posture) {}
  bool begin();
  bool enqueue(BleOpKind kind, const char *address = nullptr, uint8_t type = 0xFF);
  bool takeOutcome(BleOutcome &out);
  void cancel() { generation_.fetch_add(1); }
  bool busy() const { return pending_.load() != 0; }
  bool available() const { return worker_ != nullptr; }
  bool suspend();
  void tick();
  BleManagerState state() const { return state_.load(); }

private:
  static void workerEntry(void *context);
  void work();
  bool stopWorker();
  void releaseQueues();
  BlePowerMeter &power_;
  BlePosture &posture_;
  QueueHandle_t commands_ = nullptr;
  QueueHandle_t outcomes_ = nullptr;
  SemaphoreHandle_t stopped_ = nullptr;
  TaskHandle_t worker_ = nullptr;
  bool stopping_ = false;
  std::atomic<uint32_t> generation_{0};
  std::atomic<size_t> pending_{0};
  std::atomic<BleManagerState> state_{BleManagerState::Suspended};
  uint8_t postureFailures_ = 0;
  uint32_t lastPostureFailureMs_ = 0;
  uint32_t lastStatsMs_ = 0;
  uint32_t lastDropCount_ = 0;
};