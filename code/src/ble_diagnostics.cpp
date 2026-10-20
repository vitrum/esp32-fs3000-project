#include "ble_diagnostics.h"
#include "runtime_log.h"
#include "config.h"
#include <NimBLEDevice.h>
#include <cstring>

namespace {
constexpr char kService[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char kRx[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char kTx[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
static_assert(CONFIG_BT_NIMBLE_MAX_CONNECTIONS >= 3,
              "Diagnostics, power meter and posture require at least three BLE links");
}

BleDiagnostics bleDiagnostics;

class BleDiagnostics::ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
    uint16_t expected = UINT16_MAX;
    if (!bleDiagnostics.peer_.compare_exchange_strong(expected, info.getConnHandle())) {
      server->disconnect(info.getConnHandle());
      AppLog.println("[BLE-LOG] rejected additional diagnostic client");
      return;
    }
    AppLog.println("[BLE-LOG] phone connected; subscribe to TX for logs");
  }

  void onDisconnect(NimBLEServer *, NimBLEConnInfo &info, int reason) override {
    uint16_t expected = info.getConnHandle();
    if (!bleDiagnostics.peer_.compare_exchange_strong(expected, UINT16_MAX)) return;
    bleDiagnostics.subscribed_.store(false);
    bleDiagnostics.paused_.store(false);
    AppLog.setRaw(false);
    AppLog.printf("[BLE-LOG] phone disconnected reason=%d; raw capture off\n", reason);
  }
};

class BleDiagnostics::TxCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &info, uint16_t value) override {
    if (info.getConnHandle() != bleDiagnostics.peer_.load()) return;
    bleDiagnostics.subscribed_.store((value & 1) != 0);
    bleDiagnostics.paused_.store(false);
    bleDiagnostics.live_.store(false);
    bleDiagnostics.replay_.fetch_add(1);
    AppLog.printf("[BLE-LOG] notifications %s\n", value & 1 ? "on; replaying retained bytes" : "off");
  }

  void onRead(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
    char text[180];
    bleDiagnostics.status(text, sizeof(text));
    characteristic->setValue(text);
  }
};

class BleDiagnostics::CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &info) override {
    if (info.getConnHandle() != bleDiagnostics.peer_.load()) return;
    const auto value = characteristic->getValue();
    if (value.size() == 0 || value.size() > 32) {
      AppLog.println("[BLE-LOG] ERROR command must contain 1..32 ASCII bytes");
      return;
    }
    char command[33];
    memcpy(command, value.data(), value.size());
    size_t length = value.size();
    while (length && (command[length - 1] == '\r' || command[length - 1] == '\n' ||
                      command[length - 1] == ' ')) --length;
    command[length] = '\0';
    for (size_t i = 0; i < length; ++i) {
      if (command[i] < 32 || command[i] > 126) {
        AppLog.println("[BLE-LOG] ERROR command must be printable ASCII");
        return;
      }
    }
    if (!strcmp(command, "RAW ON")) AppLog.setRaw(true);
    else if (!strcmp(command, "RAW OFF")) AppLog.setRaw(false);
    else if (!strcmp(command, "PAUSE")) bleDiagnostics.paused_.store(true);
    else if (!strcmp(command, "RESUME")) bleDiagnostics.paused_.store(false);
    else if (!strcmp(command, "REPLAY") || !strcmp(command, "LIVE")) {
      bleDiagnostics.live_.store(!strcmp(command, "LIVE"));
      bleDiagnostics.replay_.fetch_add(1);
    } else if (!strcmp(command, "STATUS")) {
      char text[180];
      bleDiagnostics.status(text, sizeof(text));
      AppLog.printf("[BLE-LOG] %s\n", text);
    } else if (!strcmp(command, "HELP")) {
      AppLog.println("[BLE-LOG] RAW ON | RAW OFF | PAUSE | RESUME | REPLAY | LIVE | STATUS | HELP");
    } else {
      AppLog.println("[BLE-LOG] ERROR unknown command; write HELP for commands");
      return;
    }
    AppLog.printf("[BLE-LOG] OK %s\n", command);
  }
};

void BleDiagnostics::status(char *text, size_t capacity) const {
  snprintf(text, capacity, "BLE-LOG v1 raw=%u paused=%u subscribed=%u lost_bytes=%lu notify_fail=%lu truncated=%lu ring=%u",
           AppLog.rawEnabled(), paused_.load(), subscribed_.load(),
           (unsigned long)lostBytes_.load(), (unsigned long)notifyFailures_.load(),
           (unsigned long)AppLog.truncated(), (unsigned)RuntimeLog::kCapacity);
}

bool BleDiagnostics::begin() {
  if (running_) return true;
  if (!NimBLEDevice::isInitialized() || server_) {
    AppLog.println("[BLE-LOG] cannot start: shared stack unavailable or service initialization incomplete");
    return false;
  }
  static ServerCallbacks serverCallbacks;
  static TxCallbacks txCallbacks;
  static CommandCallbacks commandCallbacks;
  server_ = NimBLEDevice::createServer();
  if (!server_) {
    AppLog.println("[BLE-LOG] could not create GATT server");
    return false;
  }
  server_->setCallbacks(&serverCallbacks, false);
  server_->advertiseOnDisconnect(false);
  auto *service = server_->getServiceByUUID(kService);
  if (!service) service = server_->createService(kService);
  if (!service) {
    AppLog.println("[BLE-LOG] could not create UART service");
    return false;
  }
  tx_ = service->getCharacteristic(kTx);
  if (!tx_) tx_ = service->createCharacteristic(kTx, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, sizeof(pending_));
  auto *rx = service->getCharacteristic(kRx);
  if (!rx) rx = service->createCharacteristic(kRx, NIMBLE_PROPERTY::WRITE, 32);
  if (!tx_ || !rx) {
    AppLog.println("[BLE-LOG] could not create UART characteristics");
    return false;
  }
  tx_->setCallbacks(&txCallbacks);
  rx->setCallbacks(&commandCallbacks);
  if (!server_->start()) {
    AppLog.println("[BLE-LOG] could not start GATT server");
    return false;
  }
  auto *advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->setMinInterval(320); // 200 ms, reduce diagnostic radio duty.
  advertising->setMaxInterval(480);
  if (!advertising->setName(BLE_DEVICE_NAME) || !advertising->addServiceUUID(kService)) {
    AppLog.println("[BLE-LOG] could not configure advertising payload");
    return false;
  }
  running_ = true;
  lastAdvertiseMs_ = millis();
  if (!advertising->start())
    AppLog.println("[BLE-LOG] advertising start failed; will retry");
  AppLog.println("[BLE-LOG] Nordic UART service ready; one phone, RAW OFF by default");
  return true;
}

void BleDiagnostics::suspend() {
  running_ = false;
  if (server_ && NimBLEDevice::isInitialized()) {
    if (!server_->stopAdvertising()) AppLog.println("[BLE-LOG] could not stop advertising");
    const uint16_t peer = peer_.load();
    if (peer != UINT16_MAX && !server_->disconnect(peer))
      AppLog.println("[BLE-LOG] phone disconnect request failed");
  }
  peer_.store(UINT16_MAX);
  subscribed_.store(false);
  paused_.store(false);
  AppLog.setRaw(false);
  pendingLength_ = 0;
  // NimBLE owns these objects and deletes them in deinit(true).
  server_ = nullptr;
  tx_ = nullptr;
}

void BleDiagnostics::tick() {
  if (!running_) return;
  const uint32_t now = millis();
  if (peer_.load() == UINT16_MAX) {
    pendingLength_ = 0;
    if (now - lastAdvertiseMs_ >= 3000) {
      lastAdvertiseMs_ = now;
      auto *advertising = NimBLEDevice::getAdvertising();
      if (!advertising->isAdvertising() && !advertising->start())
        AppLog.println("[BLE-LOG] advertising retry failed");
    }
    return;
  }
  if (!subscribed_.load() || paused_.load()) return;
  const uint32_t replay = replay_.load();
  if (replay != lastReplay_) {
    cursor_ = live_.load() ? AppLog.end() : AppLog.oldest();
    pendingLength_ = 0;
    lastReplay_ = replay;
  }
  if (now - lastSendMs_ < 20) return;
  lastSendMs_ = now;
  const uint16_t peer = peer_.load();
  const uint16_t mtu = server_->getPeerMTU(peer);
  if (mtu < 23) return;
  const size_t capacity = mtu - 3 < sizeof(pending_) ? mtu - 3 : sizeof(pending_);
  if (pendingLength_ == 0) {
    pendingCursor_ = cursor_;
    uint64_t lost = 0;
    pendingLength_ = AppLog.read(pendingCursor_, pending_, capacity, lost);
    if (lost) {
      cursor_ += lost;
      pendingCursor_ = cursor_;
      lostBytes_.fetch_add(uint32_t(lost));
      memcpy(pending_, "!GAP\n", 5);
      pendingLength_ = 5;
    }
  }
  if (!pendingLength_) return;
  if (tx_->notify(pending_, pendingLength_, peer)) {
    cursor_ = pendingCursor_;
    pendingLength_ = 0;
  } else {
    notifyFailures_.fetch_add(1);
    if (now - lastErrorMs_ >= 5000) {
      lastErrorMs_ = now;
      AppLog.println("[BLE-LOG] notification enqueue failed; retained chunk will retry");
    }
  }
}
