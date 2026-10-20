#include "ble_power.h"
#include "config.h"
#include "runtime_log.h"
#include <NimBLEDevice.h>

namespace {
const NimBLEUUID kSvcCP("0x1818");
const NimBLEUUID kSvcFTMS("0x1826");
const NimBLEUUID kSvcCSC("0x1816");

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
  AppLog.printf("[BLE] %s\n", message);
}

// NimBLEUtils::returnCodeToString() 对部分 HCI 层错误（如 13=ETIMEOUT）返回空串，
// 这里补一个可读描述，避免日志出现 "code 13: " 这种没用的输出。
const char *bleErrorText(int code) {
  const char *text = NimBLEUtils::returnCodeToString(code);
  if (text && text[0]) return text;
  switch (code) {
    case 13:  return "ETIMEOUT (no response from peer)";
    case 6:   return "ENOTCONN";
    case 0x3E: return "EREJECT";
    default:  return "unknown";
  }
}

const char *bleAddrTypeName(uint8_t type, char *buffer, size_t capacity) {
  static const char *const kNames[] = {
      "PUBLIC", "RANDOM_STATIC", "RANDOM_PRIVATE_RESOLVABLE",
      "RANDOM_PRIVATE_NON_RESOLVABLE"};
  if (type < 4) return kNames[type];
  snprintf(buffer, capacity, "UNKNOWN(%u)", (unsigned)type);
  return buffer;
}
}

bool BlePowerMeter::s_connected = false;
bool BlePowerMeter::s_hasCyclingPower = false;
bool BlePowerMeter::s_hasFitnessMachine = false;
bool BlePowerMeter::s_hasSpeedCadence = false;
float BlePowerMeter::s_power = 0;
float BlePowerMeter::s_cadence = 0;
float BlePowerMeter::s_speed = 0;
float BlePowerMeter::s_wheelCirc = BLE_WHEEL_CIRC_M;
uint32_t BlePowerMeter::s_notificationCount = 0;
uint32_t BlePowerMeter::s_fitnessMachineNotificationCount = 0;
uint16_t BlePowerMeter::s_lastFitnessMachineFlags = 0;
uint32_t BlePowerMeter::s_disconnectCount = 0;
NimBLEClient *BlePowerMeter::s_client = nullptr;
char BlePowerMeter::s_connectedAddress[18] = {};
char BlePowerMeter::s_lastError[320] = {};

class BlePowerClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int reason) override {
    AppLog.printf("[BLE] power link disconnected reason=%d\n", reason);
    BlePowerMeter::s_connected = false;
    BlePowerMeter::s_connectedAddress[0] = '\0';
    BlePowerMeter::s_hasCyclingPower = false;
    BlePowerMeter::s_hasFitnessMachine = false;
    BlePowerMeter::s_hasSpeedCadence = false;
    BlePowerMeter::s_power = 0;
    BlePowerMeter::s_cadence = 0;
    BlePowerMeter::s_speed = 0;
    BlePowerMeter::s_lastFitnessMachineFlags = 0;
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
  AppLog.packet("CPS/2A63", data, length);
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
  AppLog.packet("FTMS/2AD2", data, length);
  if (!data || length < 2) return;

  const uint16_t flags = readU16(data, 0);
  size_t index = 2;

  // FTMS bit 0 clear means instantaneous speed is present (0.01 km/h units).
  if ((flags & 0x0001) == 0) {
    if (!hasBytes(length, index, 2)) return;
    s_speed = (float)readU16(data, index) / 360.0f;
    index += 2;
  }

  if (flags & 0x0002) {
    if (!hasBytes(length, index, 2)) return;
    index += 2;  // Average Speed
  }
  if (flags & 0x0004) {
    if (!hasBytes(length, index, 2)) return;
    s_cadence = (float)readU16(data, index) * 0.5f;
    index += 2;
  }
  if (flags & 0x0008) {
    if (!hasBytes(length, index, 2)) return;
    index += 2;
  }
  if (flags & 0x0010) {
    if (!hasBytes(length, index, 3)) return;
    index += 3;
  }
  if (flags & 0x0020) {
    if (!hasBytes(length, index, 2)) return;
    index += 2;
  }
  bool hasPower = false;
  if (flags & 0x0040) {
    if (!hasBytes(length, index, 2)) return;
    s_power = (float)(int16_t)readU16(data, index);
    index += 2;
    hasPower = true;
  }
  if (flags & 0x0080) {
    if (!hasBytes(length, index, 2)) return;
    index += 2;
  }
  if (flags & 0x0100) {
    if (!hasBytes(length, index, 5)) return;
    index += 5;
  }
  if (flags & 0x0200) {
    if (!hasBytes(length, index, 1)) return;
    index += 1;
  }
  if (flags & 0x0400) {
    if (!hasBytes(length, index, 1)) return;
    index += 1;
  }
  if (flags & 0x0800) {
    if (!hasBytes(length, index, 2)) return;
    index += 2;
  }
  if (flags & 0x1000) {
    if (!hasBytes(length, index, 2)) return;
    index += 2;
  }
  s_lastFitnessMachineFlags = flags;
  s_fitnessMachineNotificationCount++;
  if (hasPower) s_notificationCount++;
}

void BlePowerMeter::speedCadenceNotify(NimBLERemoteCharacteristic *, uint8_t *data,
                                       size_t length, bool) {
  AppLog.packet("CSC/2A5B", data, length);
  if (!data || length < 1) return;

  const uint8_t flags = data[0];
  size_t index = 1;
  if (flags & 0x01) {
    if (!hasBytes(length, index, 6)) return;
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
  if (flags & 0x02) {
    if (!hasBytes(length, index, 4)) return;
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
  s_notificationCount++;
}

void BlePowerMeter::begin(const char *deviceName, float wheelCircM) {
  s_wheelCirc = wheelCircM;
  NimBLEDevice::init(deviceName);
  NimBLEDevice::getScan()->setMaxResults(20);
}

void BlePowerMeter::clearScanResults() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (scan && !scan->isScanning()) scan->clearResults();
}

bool BlePowerMeter::scanDevices(BlePowerDevice *devices, size_t capacity,
                                size_t &count, uint32_t durationMs) {
  count = 0;
  if (!devices || capacity == 0) {
    setError("BLE scan failed: invalid output buffer");
    return false;
  }

  if (!NimBLEDevice::isInitialized()) {
    setError("BLE scan failed: BLE stack is not initialized");
    return false;
  }
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (!scan) {
    setError("BLE scan failed: NimBLE scan is unavailable");
    return false;
  }
  scan->clearResults();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(30);
  NimBLEScanResults results =
      scan->getResults(durationMs ? durationMs : 6000, false);

  // 注意：NimBLEScanResults 持有的是扫描缓存里的指针。这里先把需要的字段
  // 全部拷进本地结构，再与上一次的「用户选择列表」做去重，最后才
  // clearResults()，避免任何 ad->getName() 落在缓存释放之后。
  struct Candidate {
    char name[32];
    char address[18];
    int8_t rssi;
    bool cyclingPower;
    bool fitnessMachine;
    bool speedCadence;
    bool supported;
    uint8_t addressType;
  };
  static Candidate candidates[32];
  size_t candidateCount = 0;

  for (int i = 0; i < results.getCount() && candidateCount < 32; ++i) {
    const NimBLEAdvertisedDevice *ad = results.getDevice(i);
    if (!ad || !ad->isConnectable()) continue;

    const bool cyclingPower = ad->isAdvertisingService(kSvcCP);
    const bool fitnessMachine = ad->isAdvertisingService(kSvcFTMS);
    const bool speedCadence = ad->isAdvertisingService(kSvcCSC);
    const bool supported = cyclingPower || fitnessMachine || speedCadence;

    const std::string name = ad->getName();
    if (name.empty() && !supported) continue;

    const std::string address = ad->getAddress().toString();
    bool alreadyAdded = false;
    for (size_t j = 0; j < candidateCount; ++j) {
      if (address == candidates[j].address) {
        alreadyAdded = true;
        break;
      }
    }
    if (alreadyAdded) continue;

    Candidate &c = candidates[candidateCount++];
    snprintf(c.name, sizeof(c.name), "%s",
             name.empty() ? "Unnamed BLE device" : name.c_str());
    snprintf(c.address, sizeof(c.address), "%s", address.c_str());
    c.rssi = ad->getRSSI();
    c.cyclingPower = cyclingPower;
    c.fitnessMachine = fitnessMachine;
    c.speedCadence = speedCadence;
    c.supported = supported;
    // 关键：记录真实地址类型，供后续 connect 使用（见 BlePowerDevice 注释）。
    c.addressType = (uint8_t)ad->getAddress().getType();
  }
  scan->clearResults();

  for (int pass = 0; pass < 2 && count < capacity; ++pass) {
    for (size_t i = 0; i < candidateCount && count < capacity; ++i) {
      const Candidate &c = candidates[i];
      if ((pass == 0 && !c.supported) || (pass == 1 && c.supported)) continue;

      BlePowerDevice &device = devices[count++];
      snprintf(device.name, sizeof(device.name), "%s", c.name);
      snprintf(device.address, sizeof(device.address), "%s", c.address);
      device.rssi = c.rssi;
      device.cyclingPower = c.cyclingPower;
      device.fitnessMachine = c.fitnessMachine;
      device.speedCadence = c.speedCadence;
      device.addressType = c.addressType;
    }
  }

  AppLog.printf("[BLE] scan complete: %u connectable device(s)\n",
                (unsigned)count);
  for (size_t i = 0; i < count; ++i) {
    char typeBuffer[24];
    const char *typeName =
        bleAddrTypeName(devices[i].addressType, typeBuffer, sizeof(typeBuffer));
    AppLog.printf("[BLE] %u: %s, %s [%s], RSSI %d dBm%s%s\n",
                  (unsigned)(i + 1), devices[i].name, devices[i].address,
                  typeName, devices[i].rssi,
                  devices[i].cyclingPower ? " CPS" : "",
                  devices[i].fitnessMachine ? " FTMS" :
                      (devices[i].speedCadence ? " CSC (cadence/speed only)" : ""));
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
  s_hasSpeedCadence = false;
  s_hasWheel = false;
  s_hasCrank = false;
  s_power = 0;
  s_cadence = 0;
  s_speed = 0;
}

void BlePowerMeter::cancelCurrentOperation() {
  if (!NimBLEDevice::isInitialized()) return;
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (scan && scan->isScanning()) scan->stop();
  if (s_client && !s_client->isConnected()) s_client->cancelConnect();
}

bool BlePowerMeter::connectDevice(const char *address, uint8_t addressType) {
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

  if (!NimBLEDevice::isInitialized()) {
    setError("Connection failed: BLE stack is not initialized");
    return false;
  }
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (!scan) {
    setError("Connection failed: NimBLE scan results are unavailable");
    return false;
  }

  // ---------------------------------------------------------------------
  // 不要依赖扫描缓存来连接。
  //
  // 旧实现要求「地址必须仍在本轮扫描缓存里」才允许连接，否则直接报
  // "device is no longer in scan results"。但扫描缓存的生命周期和用户
  // 点按时刻没有任何保证：扫描结束后的清缓存、下一次扫描开始时的
  // clearResults()（NimBLEScan::start() 内部就会清）、或者其他串行操作
  // 的复位，都会让刚扫到的设备在连接时「消失」。这正是日志里
  //   [BLE] 4: CYCPLUS DC1 52449, f6:96:e2:c1:2a:40   ← 扫到了
  //   [BLE] Connection failed ... no longer in scan results
  // 的直接原因。
  //
  // 连接只需要地址，NimBLEAddress 可以自解析地址类型。因此这里改成：
  //   1) 先在缓存里顺手取一下广播名（仅用于日志），取不到也无所谓；
  //   2) 无论如何都尝试直接用地址连接。
  // 若设备真的关机/走远，connect() 会超时并给出真实错误码，比一个
  // 误导性的「不在扫描结果里」更有用。
  // ---------------------------------------------------------------------
  char advertisedName[40] = {};
  if (!scan->isScanning()) {
    NimBLEScanResults cached = scan->getResults();
    for (int i = 0; i < cached.getCount(); ++i) {
      const NimBLEAdvertisedDevice *candidate = cached.getDevice(i);
      if (candidate && candidate->getAddress().toString() == address) {
        snprintf(advertisedName, sizeof(advertisedName), "%s",
                 candidate->getName().c_str());
        break;
      }
    }
  }

  // ---------------------------------------------------------------------
  // 地址类型：必须用扫描阶段记录的真实类型。
  //
  // 日志中 f6:96:e2:c1:2a:40 的最高字节为 0xF6（最高两位 = 11），是 BLE
  // 随机地址（Random）。旧代码一律按 BLE_ADDR_PUBLIC 连接，会把连接请求
  // 发到错误的地址解析路径，对端不应答 → 8s 后 ETIMEOUT(code 13)。
  // ---------------------------------------------------------------------
  const bool typeKnown = addressType < 4;
  // NimBLEAddress 的构造参数是 uint8_t（BLE_ADDR_PUBLIC=0 / RANDOM=1..3）。
  const uint8_t primaryType = typeKnown ? addressType : (uint8_t)BLE_ADDR_PUBLIC;
  char typeBuffer[24];
  const char *typeName =
      typeKnown ? bleAddrTypeName(addressType, typeBuffer, sizeof(typeBuffer))
                : "PUBLIC (assumed, unknown)";

  AppLog.printf("[BLE] connecting %s%s type=%s\n",
                address, advertisedName[0] ? advertisedName : "",
                typeName);
  if (!advertisedName[0])
    AppLog.printf("[BLE] %s not in scan cache; connecting by address anyway\n",
                  address);

  NimBLEClient *client = NimBLEDevice::createClient();
  if (!client) {
    setError("Connection failed: could not allocate a BLE GATT client");
    return false;
  }
  client->setClientCallbacks(&s_clientCallbacks, false);
  const uint32_t connectTimeoutMs = 8000;
  client->setConnectTimeout(connectTimeoutMs);
  s_client = client;

  // 直接用地址 + 真实地址类型连接，不依赖扫描缓存。
  const std::string addressString(address);
  bool connected = client->connect(NimBLEAddress(addressString, primaryType));
  if (connected) {
    AppLog.printf("[BLE] link established with type=%s in < %u ms\n", typeName,
                  (unsigned)connectTimeoutMs);
  } else {
    const int primaryError = client->getLastError();
    AppLog.printf("[BLE] connect failed type=%s code=%d (%s)\n", typeName,
                  primaryError, bleErrorText(primaryError));

    // 二次尝试：真实类型失败后回退 PUBLIC。仅当类型已知且不是 PUBLIC 时进行，
    // 用于覆盖「设备广播为 RANDOM、但实际接受 PUBLIC 连接」的少数厂商实现。
    if (typeKnown && primaryType != BLE_ADDR_PUBLIC) {
      AppLog.println("[BLE] retrying with PUBLIC address type");
      connected = client->connect(NimBLEAddress(addressString, BLE_ADDR_PUBLIC));
      if (connected) {
        AppLog.println("[BLE] link established only with PUBLIC (address type mismatch)");
        typeName = "PUBLIC (fallback)";
      } else {
        AppLog.printf("[BLE] PUBLIC retry failed code=%d (%s)\n",
                      client->getLastError(),
                      bleErrorText(client->getLastError()));
      }
    }
  }

  if (!connected) {
    const int code = client->getLastError();
    const char *reason = "unknown error";
    switch (code) {
      case 13:  // BLE_HS_ETIMEOUT
        reason = "no response from device; it may be connected to a phone app, "
                 "in pairing mode, or out of range";
        break;
      case 0x06:  // BLE_HS_ENOTCONN
        reason = "device rejected/cancelled the link";
        break;
      case 0x3E:  // BLE_HS_EREJECT
        reason = "device rejected the connection request";
        break;
      default:
        break;
    }
    char error[256];
    snprintf(error, sizeof(error),
             "BLE connection to %s failed (code %d: %s; type=%s). %s.",
             address, code, bleErrorText(code), typeName, reason);
    setError(error);
    disconnect();
    return false;
  }
  // 连接成功后释放扫描缓存（连接用的地址是副本，不依赖缓存）。
  if (!scan->isScanning()) scan->clearResults();

  const std::vector<NimBLERemoteService *> &services = client->getServices(true);
  NimBLERemoteCharacteristic *cpMeasurement = nullptr;
  NimBLERemoteCharacteristic *indoorBikeData = nullptr;
  NimBLERemoteCharacteristic *cscMeasurement = nullptr;

  AppLog.printf("[BLE-GATT] %s: discovered %u service(s)\n",
                address, (unsigned)services.size());
  for (NimBLERemoteService *service : services) {
    const std::string serviceUuid = service->getUUID().toString();
    const bool isCpService = serviceUuid == "0x1818";
    const bool isFtmsService = serviceUuid == "0x1826";
    const bool isCscService = serviceUuid == "0x1816";
    AppLog.printf("[BLE-GATT] service %s\n", serviceUuid.c_str());
    const std::vector<NimBLERemoteCharacteristic *> &characteristics =
        service->getCharacteristics(true);
    for (NimBLERemoteCharacteristic *characteristic : characteristics) {
      const std::string characteristicUuid = characteristic->getUUID().toString();
      const bool isCpMeasurement =
          isCpService && characteristicUuid == "0x2a63";
      const bool isIndoorBikeData =
          isFtmsService && characteristicUuid == "0x2ad2";
      const bool isCscMeasurement =
          isCscService && characteristicUuid == "0x2a5b";
      AppLog.printf("[BLE-GATT]   characteristic %s%s%s\n",
                    characteristicUuid.c_str(),
                    characteristic->canNotify() ? " notify" : "",
                    characteristic->canIndicate() ? " indicate" : "");
      if (isCpMeasurement) {
        cpMeasurement = characteristic;
        AppLog.println("[BLE-GATT]   matched Cycling Power Measurement");
      } else if (isIndoorBikeData) {
        indoorBikeData = characteristic;
        AppLog.println("[BLE-GATT]   matched FTMS Indoor Bike Data");
      } else if (isCscMeasurement) {
        cscMeasurement = characteristic;
        AppLog.println("[BLE-GATT]   matched CSC Measurement");
      }
    }
  }

  if (cpMeasurement) {
    s_hasCyclingPower = cpMeasurement->subscribe(true, cpmNotify, true);
    AppLog.printf("[BLE-GATT] subscribe CPS 0x2A63: %s\n",
                  s_hasCyclingPower ? "OK" : "FAILED");
  }
  if (indoorBikeData) {
    s_hasFitnessMachine = indoorBikeData->subscribe(true, indoorBikeNotify, true);
    AppLog.printf("[BLE-GATT] subscribe FTMS Indoor Bike Data 0x2AD2: %s\n",
                  s_hasFitnessMachine ? "OK" : "FAILED");
  }
  if (cscMeasurement) {
    s_hasSpeedCadence = cscMeasurement->subscribe(true, speedCadenceNotify, true);
    AppLog.printf("[BLE-GATT] subscribe CSC 0x2A5B: %s\n",
                  s_hasSpeedCadence ? "OK" : "FAILED");
  }

  if (!s_hasCyclingPower && !s_hasFitnessMachine && !s_hasSpeedCadence) {
    char error[320];
    if (services.empty()) {
      const int discoveryError = client->getLastError();
      if (discoveryError != 0) {
        snprintf(error, sizeof(error),
                 "Connected to %s but GATT service discovery failed (BLE error %d: %s). Check pairing and whether another app is using the trainer; see [BLE-GATT] monitor logs.",
                 address, discoveryError,
                 NimBLEUtils::returnCodeToString(discoveryError));
      } else {
        snprintf(error, sizeof(error),
                 "Connected to %s; GATT discovery completed but returned zero services. The device may require pairing, be claimed by another app, or use a vendor protocol; see [BLE-GATT] monitor logs.",
                 address);
      }
    } else {
      if (cpMeasurement || indoorBikeData || cscMeasurement) {
        snprintf(error, sizeof(error),
                 "Connected to %s and found data characteristics, but enabling notifications failed (CPS 0x2A63=%s, FTMS Indoor Bike Data 0x2AD2=%s, CSC 0x2A5B=%s; BLE error %d: %s).",
                 address,
                 cpMeasurement ? (s_hasCyclingPower ? "OK" : "FAILED") : "absent",
                 indoorBikeData ? (s_hasFitnessMachine ? "OK" : "FAILED") : "absent",
                 cscMeasurement ? (s_hasSpeedCadence ? "OK" : "FAILED") : "absent",
                 client->getLastError(),
                 NimBLEUtils::returnCodeToString(client->getLastError()));
      } else {
        snprintf(error, sizeof(error),
                 "Connected to %s. GATT services were found but no usable measurement characteristic: need CPS 0x1818/0x2A63, FTMS Indoor Bike Data 0x1826/0x2AD2, or cadence-only CSC 0x1816/0x2A5B. See [BLE-GATT] service list in monitor.",
                 address);
      }
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
  s_fitnessMachineNotificationCount = 0;
  s_lastFitnessMachineFlags = 0;
  s_lastError[0] = '\0';
  AppLog.printf("[BLE] connected: %s (%s), profiles:%s%s%s%s\n",
                advertisedName[0] ? advertisedName : "unnamed", address,
                s_hasCyclingPower ? " CPS" : "",
                s_hasFitnessMachine ? " FTMS" : "",
                s_hasSpeedCadence ? " CSC cadence/speed" : "",
                s_hasSpeedCadence && !s_hasCyclingPower && !s_hasFitnessMachine
                    ? " (no power)" : "");
  return true;
}
