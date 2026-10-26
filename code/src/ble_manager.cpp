#include "ble_manager.h"
#include "ble_diagnostics.h"
#include "config.h"
#include "runtime_log.h"
#include "saved_connections.h"
#include <freertos/task.h>

void BleManager::releaseQueues() {
  if (commands_) vQueueDelete(commands_);
  if (outcomes_) vQueueDelete(outcomes_);
  if (stopped_) vSemaphoreDelete(stopped_);
  commands_ = outcomes_ = nullptr;
  stopped_ = nullptr;
}

bool BleManager::begin() {
  if (worker_) return !stopping_;
  power_.begin(BLE_DEVICE_NAME, BLE_WHEEL_CIRC_M);
  posture_.begin(BLE_DEVICE_NAME);
  if (!bleDiagnostics.begin()) AppLog.println("[BLE-LOG] diagnostic service unavailable");
  commands_ = xQueueCreate(BLE_OP_QUEUE_LEN, sizeof(BleCommand));
  outcomes_ = xQueueCreate(BLE_OP_QUEUE_LEN + 1, sizeof(BleOutcome));
  stopped_ = xSemaphoreCreateBinary();
  if (!commands_ || !outcomes_ || !stopped_ ||
      xTaskCreatePinnedToCore(workerEntry, "bleWorker", BLE_OP_TASK_STACK,
                              this, 2, &worker_, 1) != pdPASS) {
    worker_ = nullptr;
    releaseQueues();
    AppLog.println("[BLE] manager initialization failed");
    return false;
  }
  state_.store(BleManagerState::Idle);
  return true;
}

bool BleManager::enqueue(BleOpKind kind, const char *address, uint8_t type) {
  if (!worker_ || stopping_ || pending_.load() >= BLE_OP_QUEUE_LEN || kind == BleOpKind::Stop) return false;
  if (kind == BleOpKind::Posture) {
    const uint32_t backoff = postureFailures_ ? (1000u << (postureFailures_ > 5 ? 5 : postureFailures_)) : 0;
    if (busy() || ((!address || !address[0]) && backoff &&
             millis() - lastPostureFailureMs_ < backoff)) return false;
  }
  BleCommand command;
  command.kind = kind;
  command.addressType = type;
  command.generation = generation_.load();
  if (address) snprintf(command.address, sizeof(command.address), "%s", address);
  if (!busy() && !bleDiagnostics.setSensorBusy(true)) return false;
  pending_.fetch_add(1);
  if (xQueueSend(commands_, &command, 0) != pdTRUE) {
    if (pending_.fetch_sub(1) == 1) bleDiagnostics.setSensorBusy(false);
    return false;
  }
  return true;
}

bool BleManager::takeOutcome(BleOutcome &out) {
  if (!outcomes_ || xQueueReceive(outcomes_, &out, 0) != pdTRUE) return false;
  if (out.kind == BleOpKind::Posture) {
    if (out.success) postureFailures_ = 0;
    else if (!out.cancelled) {
      if (postureFailures_ < 5) ++postureFailures_;
      lastPostureFailureMs_ = millis();
    }
  }
  if (pending_.fetch_sub(1) == 1) bleDiagnostics.setSensorBusy(false);
  if (out.success && !out.cancelled && out.address[0] &&
      (out.kind == BleOpKind::Connect || out.kind == BleOpKind::Posture)) {
    SavedConnections saved;
    if (!saved.load() || !saved.rememberBle(out.address, out.addressType,
        out.kind == BleOpKind::Connect ? SavedConnections::Role::Power : SavedConnections::Role::Posture))
      AppLog.println("[BLE] connected, but the device profile could not be saved");
  }
  return true;
}

void BleManager::workerEntry(void *context) {
  static_cast<BleManager *>(context)->work();
  vTaskDelete(nullptr);
}

void BleManager::work() {
  for (;;) {
    BleCommand command;
    if (xQueueReceive(commands_, &command, portMAX_DELAY) != pdTRUE) continue;
    if (command.kind == BleOpKind::Stop) {
      xSemaphoreGive(stopped_);
      return;
    }
    const BleCancellation cancellation(generation_, command.generation);
    BleOutcome out;
    out.kind = command.kind;
    snprintf(out.address, sizeof(out.address), "%s", command.address);
    out.addressType = command.addressType == 0xFF ? 0 : command.addressType;
    bleScanner.setConnected(power_.connected() || posture_.connected() || bleDiagnostics.connected());
    if (!cancellation.cancelled()) {
      switch (command.kind) {
        case BleOpKind::Scan:
          state_.store(BleManagerState::Scanning);
          out.success = power_.scanDevices(out.devices, kBleDeviceCapacity, out.deviceCount,
                                            BLE_OP_SCAN_DURATION_MS, &cancellation);
          if (!out.success) snprintf(out.error, sizeof(out.error), "%s", power_.lastError());
          break;
        case BleOpKind::Connect:
          state_.store(BleManagerState::Connecting);
          out.success = power_.connectDevice(command.address, command.addressType);
          if (!out.success) snprintf(out.error, sizeof(out.error), "%s", power_.lastError());
          break;
        case BleOpKind::Posture:
          state_.store(BleManagerState::Posture);
          out.success = command.address[0]
              ? posture_.connectDevice(command.address, command.addressType, &cancellation)
              : posture_.tryConnect(BLE_OP_POSTURE_SCAN_MS, &cancellation);
          if (out.success) {
            snprintf(out.address, sizeof(out.address), "%s", posture_.address());
            out.addressType = posture_.addressType();
          }
          if (!out.success) snprintf(out.error, sizeof(out.error), "%s", posture_.lastError());
          break;
        case BleOpKind::Stop: break;
      }
    }
    out.cancelled = cancellation.cancelled();
    if (out.cancelled) {
      if (command.kind == BleOpKind::Connect) power_.disconnect();
      if (command.kind == BleOpKind::Posture) posture_.disconnect();
      out.success = false;
    }
    state_.store(BleManagerState::Idle);
    xQueueSend(outcomes_, &out, portMAX_DELAY);
  }
}

bool BleManager::stopWorker() {
  if (!worker_) return true;
  if (busy()) return false;
  BleCommand command;
  command.kind = BleOpKind::Stop;
  if (!stopping_) {
    if (xQueueSend(commands_, &command, 0) != pdTRUE) return false;
    stopping_ = true;
  }
  if (xSemaphoreTake(stopped_, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  worker_ = nullptr;
  stopping_ = false;
  releaseQueues();
  return true;
}

bool BleManager::suspend() {
  if (!stopWorker()) return false;
  bleDiagnostics.suspend();
  power_.disconnect();
  posture_.disconnect();
  if (NimBLEDevice::isInitialized() && !NimBLEDevice::deinit(true)) {
    begin();
    return false;
  }
  state_.store(BleManagerState::Suspended);
  return true;
}

void BleManager::tick() {
  power_.processData();
  posture_.processData();
  bleDiagnostics.setByteRate(power_.connected() || posture_.connected() ? 500 : 1500);
  bleDiagnostics.tick();
  const uint32_t now = millis();
  const uint32_t drops = power_.droppedPackets() + posture_.droppedPackets() + AppLog.serialLostBytes();
  if (drops != lastDropCount_ && now - lastStatsMs_ >= 5000) {
    lastStatsMs_ = now;
    lastDropCount_ = drops;
    AppLog.printf("[BLE-QUEUE] power_drop=%lu posture_drop=%lu serial_lost=%lu\n",
                  (unsigned long)power_.droppedPackets(), (unsigned long)posture_.droppedPackets(),
                  (unsigned long)AppLog.serialLostBytes());
  }
}