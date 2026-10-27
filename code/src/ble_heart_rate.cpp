#include "ble_heart_rate.h"
#include "ble_inbox.h"
#include "runtime_log.h"

namespace {
BleInbox<8> heartInbox;
}
std::atomic<bool> BleHeartRate::connected_{false};
HeartRateMeasurement BleHeartRate::measurement_;
NimBLEClient *BleHeartRate::client_ = nullptr;
char BleHeartRate::error_[128]{};

class HeartRateCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int) override {
    BleHeartRate::connected_.store(false);
    BleHeartRate::measurement_.invalidate();
    heartInbox.reset();
  }
};
static HeartRateCallbacks callbacks;

void BleHeartRate::begin(const char *name) {
  if (!NimBLEDevice::isInitialized()) NimBLEDevice::init(name);
}

void BleHeartRate::notify(NimBLERemoteCharacteristic *, uint8_t *data, size_t size, bool) {
  if (!data || size > 240) return;
  uint8_t packet[244];
  const uint32_t received = millis();
  memcpy(packet, &received, sizeof(received));
  memcpy(packet + sizeof(received), data, size);
  heartInbox.push(0, packet, size + sizeof(received));
}

void BleHeartRate::processData() {
  BleInbox<8>::Packet packet;
  for (unsigned processed = 0; processed < 8 && heartInbox.pop(packet); ++processed) {
    if (!connected() || packet.generation != heartInbox.generation() || packet.length < 6) continue;
    uint32_t received;
    memcpy(&received, packet.data, sizeof(received));
    measurement_.accept(packet.data + sizeof(received), packet.length - sizeof(received), received);
  }
}

void BleHeartRate::disconnect() {
  connected_.store(false);
  measurement_.invalidate();
  heartInbox.reset();
  if (client_) {
    if (client_->isConnected()) client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }
}

bool BleHeartRate::connectDevice(const char *address, uint8_t type) {
  if (!address || strlen(address) != 17 || type > 3) {
    snprintf(error_, sizeof(error_), "Invalid heart sensor address/type");
    return false;
  }
  disconnect();
  client_ = NimBLEDevice::createClient();
  if (!client_) {
    snprintf(error_, sizeof(error_), "No BLE client available for heart sensor");
    return false;
  }
  client_->setClientCallbacks(&callbacks, false);
  client_->setConnectTimeout(8000);
  if (!client_->connect(NimBLEAddress(std::string(address), type))) {
    snprintf(error_, sizeof(error_), "Heart sensor connection failed: %d", client_->getLastError());
    disconnect();
    return false;
  }
  auto *service = client_->getService(NimBLEUUID("0x180d"));
  auto *characteristic = service ? service->getCharacteristic(NimBLEUUID("0x2a37")) : nullptr;
  if (!characteristic || !characteristic->subscribe(true, notify, true)) {
    snprintf(error_, sizeof(error_), "Heart Rate service/notification unavailable");
    disconnect();
    return false;
  }
  error_[0] = 0;
  connected_.store(true);
  AppLog.printf("[BLE-HR] connected: %s\n", address);
  return true;
}