#include "ble_scan.h"
#include "runtime_log.h"
#include <freertos/semphr.h>

BleScanner bleScanner;

void BleScanner::onScanEnd(const NimBLEScanResults &, int reason) {
  reason_.store(reason);
  if (completed_) xSemaphoreGive(completed_);
}

bool BleScanner::fail(const char *message) {
  snprintf(error_, sizeof(error_), "%s", message);
  state_.store(BleScanState::Failed);
  failures_.fetch_add(1);
  AppLog.printf("[BLE-SCAN] FAILED %s\n", error_);
  return false;
}

bool BleScanner::run(uint32_t durationMs, bool manual, const BleCancellation *cancel) {
  error_[0] = '\0';
  elapsedMs_.store(0);
  if (!NimBLEDevice::isInitialized()) return fail("BLE stack unavailable");
  auto *scan = NimBLEDevice::getScan();
  if (!scan || scan->isScanning()) return fail("Scanner unavailable or already running");
  if (!completed_) completed_ = xSemaphoreCreateBinary();
  if (!completed_) return fail("Could not create scan completion signal");
  while (xSemaphoreTake(completed_, 0) == pdTRUE) {}
  scan->setScanCallbacks(this, false);
  const auto policy = BleScanPolicy::select(manual, connected_.load());
  scan->setActiveScan(policy.active);
  scan->setInterval(policy.intervalMs);
  scan->setWindow(policy.windowMs);
  const uint32_t duration = durationMs ? durationMs : 1500;
  const uint32_t started = millis();
  reason_.store(0);
  state_.store(BleScanState::Starting);
  if (cancel && cancel->cancelled()) {
    state_.store(BleScanState::Cancelled);
    return false;
  }
  if (!scan->start(duration, false)) return fail("Scan start rejected by controller");
  state_.store(BleScanState::Scanning);
  AppLog.printf("[BLE-SCAN] START duration=%lu interval=%u window=%u active=%u\n",
                (unsigned long)duration, policy.intervalMs, policy.windowMs, policy.active);
  for (;;) {
    const bool done = xSemaphoreTake(completed_, pdMS_TO_TICKS(25)) == pdTRUE;
    elapsedMs_.store(millis() - started);
    if (cancel && cancel->cancelled()) {
      state_.store(BleScanState::Stopping);
      if (!scan->stop()) return fail("Scan cancellation rejected");
      state_.store(BleScanState::Cancelled);
      return false;
    }
    if (done) {
      const int reason = reason_.load();
      if (reason != 0 && reason != BLE_HS_EDONE) {
        char message[100];
        snprintf(message, sizeof(message), "Scan ended with BLE error %d", reason);
        return fail(message);
      }
      state_.store(BleScanState::Complete);
      AppLog.printf("[BLE-SCAN] COMPLETE elapsed=%lu results=%d reason=%d\n",
                    (unsigned long)elapsedMs_.load(), scan->getResults().getCount(), reason);
      return true;
    }
    if (elapsedMs_.load() >= duration + 2000) {
      state_.store(BleScanState::Stopping);
      if (!scan->stop()) return fail("Scan timed out; controller refused stop");
      return fail("Scan completion event timed out");
    }
  }
}