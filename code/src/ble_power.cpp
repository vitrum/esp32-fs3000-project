#include "ble_power.h"
#include "config.h"
#include <NimBLEDevice.h>

// ---- 蓝牙 SIG Cycling Power Service ----
static const NimBLEUUID kSvcCP ("0x1818");
static const NimBLEUUID kChrCPM("0x2A63");

bool   BlePowerMeter::s_connected = false;
float  BlePowerMeter::s_power  = 0;
float  BlePowerMeter::s_cadence = 0;
float  BlePowerMeter::s_speed  = 0;
float  BlePowerMeter::s_wheelCirc = BLE_WHEEL_CIRC_M;
uint32_t BlePowerMeter::s_notificationCount = 0;
uint32_t BlePowerMeter::s_disconnectCount = 0;
NimBLEClient *BlePowerMeter::s_client = nullptr;
char BlePowerMeter::s_connectedAddress[18] = {};

class BlePowerClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int reason) override {
    (void)reason;
    BlePowerMeter::s_connected = false;
    BlePowerMeter::s_connectedAddress[0] = '\0';
    BlePowerMeter::s_disconnectCount++;
  }
};

static BlePowerClientCallbacks s_clientCallbacks;

static bool      s_hasWheel = false;
static bool      s_hasCrank = false;
static uint32_t  s_lastWheelRevs = 0;
static uint16_t  s_lastWheelTime = 0;
static uint32_t  s_lastCrankRevs = 0;
static uint16_t  s_lastCrankTime = 0;

// ---------------------------------------------------------------------------
// 0x2A63 notify 回调：按 CPS 规范解析 Flags 之后按位序取字段
// ---------------------------------------------------------------------------
void BlePowerMeter::cpmNotify(NimBLERemoteCharacteristic *chr, uint8_t *d, size_t n,
                             bool isNotify) {
  (void)chr; (void)isNotify;
  if (n < 4) return;

  uint16_t flags = d[0] | (d[1] << 8);
  size_t   idx   = 2;

  // Instantaneous Power (W)
  s_power = (float)(d[idx] | (d[idx + 1] << 8));
  s_notificationCount++;
  idx += 2;

  if (flags & 0x0001) idx += 1;                 // Pedal Power Balance
  if (flags & 0x0004) idx += 2;                 // Accumulated Torque

  if ((flags & 0x0010) && (n >= idx + 6)) {     // Wheel Revolution Data
    uint32_t revs = (uint32_t)d[idx] | ((uint32_t)d[idx + 1] << 8) |
                    ((uint32_t)d[idx + 2] << 16) | ((uint32_t)d[idx + 3] << 24);
    uint16_t t = d[idx + 4] | (d[idx + 5] << 8);   // 1/1024 s
    if (s_hasWheel) {
      uint16_t dt  = (uint16_t)(t - s_lastWheelTime);  // 处理 64s 回绕
      uint32_t drev = revs - s_lastWheelRevs;
      if (dt > 0) {
        s_speed = s_wheelCirc * (float)drev / ((float)dt / 1024.0f);
      }
    }
    s_lastWheelRevs = revs;
    s_lastWheelTime = t;
    s_hasWheel = true;
    idx += 6;
  }

  if ((flags & 0x0020) && (n >= idx + 4)) {     // Crank Revolution Data
    uint16_t revs = d[idx] | (d[idx + 1] << 8);
    uint16_t t    = d[idx + 2] | (d[idx + 3] << 8);  // 1/1024 s
    if (s_hasCrank) {
      uint16_t dt  = (uint16_t)(t - s_lastCrankTime);
      uint16_t drev = (uint16_t)(revs - s_lastCrankRevs);
      if (dt > 0) {
        s_cadence = 60.0f * (float)drev / ((float)dt / 1024.0f);
      }
    }
    s_lastCrankRevs = revs;
    s_lastCrankTime = t;
    s_hasCrank = true;
  }
  // 其余字段（极值/上死点/累计能量等）本项目不需要，忽略
}

// ---------------------------------------------------------------------------
void BlePowerMeter::begin(const char *deviceName, float wheelCircM) {
  s_wheelCirc = wheelCircM;
  NimBLEDevice::init(deviceName);
}

bool BlePowerMeter::scanDevices(BlePowerDevice *devices, size_t capacity,
                                size_t &count) {
  count = 0;
  if (!devices || capacity == 0) return false;

  NimBLEScan *scan = NimBLEDevice::getScan();
  if (!scan) return false;
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(6000, false);

  for (int pass = 0; pass < 2 && count < capacity; ++pass) {
    for (int i = 0; i < results.getCount() && count < capacity; ++i) {
      const NimBLEAdvertisedDevice *ad = results.getDevice(i);
      if (!ad->isConnectable()) continue;

      const bool isPowerMeter = ad->isAdvertisingService(kSvcCP);
      if ((pass == 0 && !isPowerMeter) || (pass == 1 && isPowerMeter)) continue;

      std::string name = ad->getName();
      if (name.empty() && !isPowerMeter) continue;

      const std::string address = ad->getAddress().toString();
      bool alreadyAdded = false;
      for (size_t j = 0; j < count; ++j) {
        if (address == devices[j].address) {
          alreadyAdded = true;
          break;
        }
      }
      if (alreadyAdded) continue;

      BlePowerDevice &device = devices[count++];
      snprintf(device.name, sizeof(device.name), "%s",
               name.empty() ? "Unnamed BLE device" : name.c_str());
      snprintf(device.address, sizeof(device.address), "%s", address.c_str());
      device.rssi = ad->getRSSI();
    }
  }

  Serial.printf("[BLE] scan complete: %u connectable device(s)\n",
                (unsigned)count);
  for (size_t i = 0; i < count; ++i) {
    Serial.printf("[BLE] %u: %s, %s, RSSI %d dBm\n",
                  (unsigned)(i + 1), devices[i].name, devices[i].address,
                  devices[i].rssi);
  }
  return true;
}

bool BlePowerMeter::connectDevice(const char *address) {
  if (!address || !address[0]) return false;
  if (s_client && s_client->isConnected() &&
      strcmp(s_connectedAddress, address) == 0) {
    return true;
  }

  if (s_client) {
    if (s_client->isConnected()) s_client->disconnect();
    NimBLEDevice::deleteClient(s_client);
    s_client = nullptr;
    s_connected = false;
    s_connectedAddress[0] = '\0';
  }

  NimBLEScan *scan = NimBLEDevice::getScan();
  if (!scan) return false;
  NimBLEScanResults results = scan->getResults();
  const NimBLEAdvertisedDevice *ad = nullptr;
  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice *candidate = results.getDevice(i);
    if (candidate->getAddress().toString() == address) {
      ad = candidate;
      break;
    }
  }
  if (!ad) {
    Serial.printf("[BLE] selected device %s is no longer in scan results\n", address);
    return false;
  }

  NimBLEClient *cli = NimBLEDevice::createClient();
  if (!cli) {
    Serial.println("[BLE] failed to create GATT client");
    return false;
  }
  cli->setClientCallbacks(&s_clientCallbacks, false);
  s_client = cli;

  if (!cli->connect(ad)) {
    Serial.printf("[BLE] connection failed: %s\n", address);
    NimBLEDevice::deleteClient(cli);
    s_client = nullptr;
    return false;
  }

  NimBLERemoteService *svc = cli->getService(kSvcCP);
  if (!svc) {
    Serial.printf("[BLE] %s has no Cycling Power service (0x1818)\n", address);
    cli->disconnect();
    NimBLEDevice::deleteClient(cli);
    s_client = nullptr;
    return false;
  }

  NimBLERemoteCharacteristic *chr = svc->getCharacteristic(kChrCPM);
  if (!chr) {
    Serial.printf("[BLE] %s has no power measurement characteristic (0x2A63)\n", address);
    cli->disconnect();
    NimBLEDevice::deleteClient(cli);
    s_client = nullptr;
    return false;
  }

  if (!chr->subscribe(true, cpmNotify, true)) {
    Serial.printf("[BLE] failed to subscribe to power data: %s\n", address);
    cli->disconnect();
    NimBLEDevice::deleteClient(cli);
    s_client = nullptr;
    return false;
  }

  s_connected = true;
  snprintf(s_connectedAddress, sizeof(s_connectedAddress), "%s", address);
  Serial.printf("[BLE] power meter connected: %s (%s)\n",
                ad->getName().c_str(), address);
  return true;
}
