#include "ble_power.h"
#include "config.h"
#include <NimBLEDevice.h>

namespace {
const NimBLEUUID kSvcCP("0x1818");
const NimBLEUUID kChrCPM("0x2A63");
const NimBLEUUID kSvcFTMS("0x1826");
const NimBLEUUID kChrIndoorBikeData("0x2AD2");

bool s_hasWheel = false;
bool s_hasCrank = false;
uint32_t s_lastWheelRevs = 0;
uint16_t s_lastWheelTime = 0;
uint32_t s_lastCrankRevs = 0;
uint16_t s_lastCrankTime = 0;

bool hasBytes(size_t length, size_t index, size_t count) {
  return index <= length && count <= length - index;
}

uint16_t readU16(const uint8_t *data, size_t index) {
  return (uint16_t)data[index] | ((uint16_t)data[index + 1] << 8);
}

void logBleError(const char *message) {
  Serial.printf("[BLE] %s\n", message);
}
}

bool BlePowerMeter::s_connected = false;
bool BlePowerMeter::s_hasCyclingPower = false;
bool BlePowerMeter::s_hasFitnessMachine = false;
float BlePowerMeter::s_power = 0;
float BlePowerMeter::s_cadence = 0;
float BlePowerMeter::s_speed = 0;
float BlePowerMeter::s_wheelCirc = BLE_WHEEL_CIRC_M;
uint32_t BlePowerMeter::s_notificationCount = 0;
uint32_t BlePowerMeter::s_disconnectCount = 0;
NimBLEClient *BlePowerMeter::s_client = nullptr;
char BlePowerMeter::s_connectedAddress[18] = {};
char BlePowerMeter::s_lastError[192] = {};

class BlePowerClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int reason) override {
    (void)reason;
    BlePowerMeter::s_connected = false;
    BlePowerMeter::s_connectedAddress[0] = '\0';
    BlePowerMeter::s_hasCyclingPower = false;
    BlePowerMeter::s_hasFitnessMachine = false;
    BlePowerMeter::s_power = 0;
    BlePowerMeter::s_cadence = 0;
    BlePowerMeter::s_speed = 0;
    BlePowerMeter::s_disconnectCount++;
  }
};

static BlePowerClientCallbacks s_clientCallbacks;

void BlePowerMeter::setError(const char *message) {
  snprintf(s_lastError, sizeof(s_lastError), "%s", message ? message : "Unknown BLE error");
  logBleError(s_lastError);
}

void BlePowerMeter::cpmNotify(NimBLERemoteCharacteristic *, uint8_t *data,
                              size_t length, bool) {
  if (!data || length < 4) return;

  const uint16_t flags = readU16(data, 0);
  size_t index = 2;
  s_power = (float)(int16_t)readU16(data, index);
  s_notificationCount++;
  index += 2;

  if (flags & 0x0001) index += 1;
  if (flags & 0x0004) index += 2;

  if ((flags & 0x0010) && hasBytes(length, index, 6)) {
    const uint32_t revolutions = (uint32_t)data[index] |
                                 ((uint32_t)data[index + 1] << 8) |
                                 ((uint32_t)data[index + 2] << 16) |
                                 ((uint32_t)data[index + 3] << 24);
    const uint16_t eventTime = readU16(data, index + 4);
    if (s_hasWheel) {
      const uint16_t deltaTime = (uint16_t)(eventTime - s_lastWheelTime);
      const uint32_t deltaRevs = revolutions - s_lastWheelRevs;
      if (deltaTime > 0) {
        s_speed = s_wheelCirc * (float)deltaRevs / ((float)deltaTime / 1024.0f);
      }
    }
    s_lastWheelRevs = revolutions;
    s_lastWheelTime = eventTime;
    s_hasWheel = true;
    index += 6;
  }

  if ((flags & 0x0020) && hasBytes(length, index, 4)) {
    const uint16_t revolutions = readU16(data, index);
    const uint16_t eventTime = readU16(data, index + 2);
    if (s_hasCrank) {
      const uint16_t deltaTime = (uint16_t)(eventTime - s_lastCrankTime);
      const uint16_t deltaRevs = (uint16_t)(revolutions - s_lastCrankRevs);
      if (deltaTime > 0) {
        s_cadence = 60.0f * (float)deltaRevs / ((float)deltaTime / 1024.0f);
      }
    }
    s_lastCrankRevs = revolutions;
    s_lastCrankTime = eventTime;
    s_hasCrank = true;
  }
}

void BlePowerMeter::indoorBikeNotify(NimBLERemoteCharacteristic *, uint8_t *data,
                                     size_t length, bool) {
  if (!data || length < 2) return;

  const uint16_t flags = readU16(data, 0);
  size_t index = 2;

  // FTMS bit 0 clear means instantaneous speed is present (0.01 km/h units).
  if ((flags & 0x0001) == 0) {
    if (!hasBytes(length, index, 2)) return;
    s_speed = (float)readU16(data, index) / 360.0f;
    index += 2;
  }

  if (flags & 0x0002) index += 2;  // Average Speed
  if (flags & 0x0004) {
    if (!hasBytes(length, index, 2)) return;
    s_cadence = (float)readU16(data, index) * 0.5f;
    index += 2;
  }
  if (flags & 0x0008) index += 2;  // Average Cadence
  if (flags & 0x0010) index += 3;  // Total Distance
  if (flags & 0x0020) index += 2;  // Resistance Level
  if (flags & 0x0040) {
    if (!hasBytes(length, index, 2)) return;
    s_power = (float)(int16_t)readU16(data, index);
    index += 2;
    s_notificationCount++;
  }
  if (flags & 0x0080) index += 2;  // Average Power
  if (flags & 0x0100) index += 5;  // Expended Energy
  if (flags & 0x0200) index += 1;  // Heart Rate
  if (flags & 0x0400) index += 1;  // Metabolic Equivalent
  if (flags & 0x0800) index += 2;  // Elapsed Time
  if (flags & 0x1000) index += 2;  // Remaining Time
}

void BlePowerMeter::begin(const char *deviceName, float wheelCircM) {
  s_wheelCirc = wheelCircM;
  NimBLEDevice::init(deviceName);
}

bool BlePowerMeter::scanDevices(BlePowerDevice *devices, size_t capacity,
                                size_t &count) {
  count = 0;
  if (!devices || capacity == 0) {
    setError("BLE scan failed: invalid output buffer");
    return false;
  }

  NimBLEScan *scan = NimBLEDevice::getScan();
  if (!scan) {
    setError("BLE scan failed: NimBLE scan is unavailable");
    return false;
  }
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(6000, false);

  for (int pass = 0; pass < 2 && count < capacity; ++pass) {
    for (int i = 0; i < results.getCount() && count < capacity; ++i) {
      const NimBLEAdvertisedDevice *ad = results.getDevice(i);
      if (!ad->isConnectable()) continue;

      const bool cyclingPower = ad->isAdvertisingService(kSvcCP);
      const bool fitnessMachine = ad->isAdvertisingService(kSvcFTMS);
      const bool supported = cyclingPower || fitnessMachine;
      if ((pass == 0 && !supported) || (pass == 1 && supported)) continue;

      const std::string name = ad->getName();
      if (name.empty() && !supported) continue;

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
      device.cyclingPower = cyclingPower;
      device.fitnessMachine = fitnessMachine;
    }
  }

  Serial.printf("[BLE] scan complete: %u connectable device(s)\n",
                (unsigned)count);
  for (size_t i = 0; i < count; ++i) {
    Serial.printf("[BLE] %u: %s, %s, RSSI %d dBm%s%s\n",
                  (unsigned)(i + 1), devices[i].name, devices[i].address,
                  devices[i].rssi,
                  devices[i].cyclingPower ? " CPS" : "",
                  devices[i].fitnessMachine ? " FTMS" : "");
  }
  s_lastError[0] = '\0';
  return true;
}

void BlePowerMeter::disconnect() {
  if (s_client && s_client->isConnected()) s_client->disconnect();
  if (s_client) {
    NimBLEDevice::deleteClient(s_client);
    s_client = nullptr;
  }
  s_connected = false;
  s_connectedAddress[0] = '\0';
  s_hasCyclingPower = false;
  s_hasFitnessMachine = false;
  s_power = 0;
  s_cadence = 0;
  s_speed = 0;
}

void BlePowerMeter::cancelCurrentOperation() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (scan && scan->isScanning()) scan->stop();
  if (s_client && !s_client->isConnected()) s_client->cancelConnect();
}

bool BlePowerMeter::connectDevice(const char *address) {
  if (!address || !address[0]) {
    setError("Connection failed: selected device address is empty");
    return false;
  }
  if (s_client && s_client->isConnected() &&
      strcmp(s_connectedAddress, address) == 0) {
    s_lastError[0] = '\0';
    return true;
  }

  disconnect();

  NimBLEScan *scan = NimBLEDevice::getScan();
  if (!scan) {
    setError("Connection failed: NimBLE scan results are unavailable");
    return false;
  }
  NimBLEScanResults results = scan->getResults();
  const NimBLEAdvertisedDevice *advertised = nullptr;
  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice *candidate = results.getDevice(i);
    if (candidate->getAddress().toString() == address) {
      advertised = candidate;
      break;
    }
  }
  if (!advertised) {
    char error[192];
    snprintf(error, sizeof(error),
             "Connection failed for %s: device is no longer in scan results. Scan again and select it nearby.",
             address);
    setError(error);
    return false;
  }

  NimBLEClient *client = NimBLEDevice::createClient();
  if (!client) {
    setError("Connection failed: could not allocate a BLE GATT client");
    return false;
  }
  client->setClientCallbacks(&s_clientCallbacks, false);
  client->setConnectTimeout(8000);
  s_client = client;

  if (!client->connect(advertised)) {
    char error[192];
    snprintf(error, sizeof(error),
             "BLE connection to %s failed (code %d: %s). Wake the trainer and retry.",
             address, client->getLastError(),
             NimBLEUtils::returnCodeToString(client->getLastError()));
    setError(error);
    disconnect();
    return false;
  }

  NimBLERemoteService *cpService = client->getService(kSvcCP);
  NimBLERemoteService *ftmsService = client->getService(kSvcFTMS);
  NimBLERemoteCharacteristic *cpMeasurement =
      cpService ? cpService->getCharacteristic(kChrCPM) : nullptr;
  NimBLERemoteCharacteristic *indoorBikeData =
      ftmsService ? ftmsService->getCharacteristic(kChrIndoorBikeData) : nullptr;

  if (cpMeasurement) {
    s_hasCyclingPower = cpMeasurement->subscribe(true, cpmNotify, true);
  }
  if (indoorBikeData) {
    s_hasFitnessMachine = indoorBikeData->subscribe(true, indoorBikeNotify, true);
  }

  if (!s_hasCyclingPower && !s_hasFitnessMachine) {
    char error[192];
    if (!cpService && !ftmsService) {
      snprintf(error, sizeof(error),
               "Connection to %s succeeded, but neither Cycling Power (0x1818) nor Fitness Machine (0x1826) service is present.",
               address);
    } else if ((cpService && !cpMeasurement) || (ftmsService && !indoorBikeData)) {
      snprintf(error, sizeof(error),
               "Connection to %s succeeded, but no supported measurement characteristic was found (CPS 0x2A63 / FTMS 0x2AD2).",
               address);
    } else {
      snprintf(error, sizeof(error),
               "Connection to %s succeeded, but subscribing to power data failed (CPS 0x2A63 / FTMS 0x2AD2).",
               address);
    }
    setError(error);
    disconnect();
    return false;
  }

  s_connected = true;
  snprintf(s_connectedAddress, sizeof(s_connectedAddress), "%s", address);
  s_power = 0;
  s_cadence = 0;
  s_speed = 0;
  s_hasWheel = false;
  s_hasCrank = false;
  s_notificationCount = 0;
  s_lastError[0] = '\0';
  Serial.printf("[BLE] connected: %s (%s), profiles:%s%s\n",
                advertised->getName().c_str(), address,
                s_hasCyclingPower ? " CPS" : "",
                s_hasFitnessMachine ? " FTMS" : "");
  return true;
}
