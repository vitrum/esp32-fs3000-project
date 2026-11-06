#include "ble_posture.h"
char BlePosture::s_connectedAddress[18] = {};
uint8_t BlePosture::s_connectedAddressType = 0;

#include "config.h"
#include "runtime_log.h"
#include <NimBLEDevice.h>
#include "ble_inbox.h"
#include "ble_scan.h"

namespace {
BleInbox<8> postureInbox;
uint32_t processedPostureGeneration = 0;
uint8_t postureStream[40] = {};
size_t postureStreamLength = 0;
}

// ---- WitMotion WT9011DCL BLE 协议 ----
static const NimBLEUUID kSvcWitBle5("0000ffe5-0000-1000-8000-00805f9a34fb");
static const NimBLEUUID kChrDataBle5("0000ffe4-0000-1000-8000-00805f9a34fb");
static const NimBLEUUID kSvcWitBle5Standard(uint16_t(0xFFE5));
static const NimBLEUUID kChrDataBle5Standard(uint16_t(0xFFE4));
static const NimBLEUUID kSvcWitLegacy(uint16_t(0xFFE0));
static const NimBLEUUID kChrDataLegacy(uint16_t(0xFFE1));

std::atomic<bool> BlePosture::s_connected{false};
std::atomic<uint32_t> BlePosture::s_lastMeasurementMs{0};
float  BlePosture::s_pitch = 0;
float  BlePosture::s_roll = 0;
float  BlePosture::s_yaw = 0;
ImuSample BlePosture::s_imuSample;
uint32_t BlePosture::s_notificationCount = 0;
NimBLEClient *BlePosture::s_client = nullptr;
char   BlePosture::s_lastError[160] = {};

class BlePostureClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int) override {
    BlePosture::s_connected = false;
    postureInbox.reset();
  }
};

static BlePostureClientCallbacks postureCallbacks;

// ---------------------------------------------------------------------------
// BLE 5.0 frame layout/scaling follow WITMOTION's official BLE5.0 Python SDK:
// github.com/WITMOTION/WitBluetooth_BWT901BLE5_0.
// The 20-byte 0x61 frame has angles at bytes 14..19; legacy 0x53 is also accepted.
// ---------------------------------------------------------------------------
void BlePosture::witNotify(NimBLERemoteCharacteristic *chr, uint8_t *d, size_t n,
                           bool isNotify) {
  (void)chr; (void)isNotify;
  postureInbox.push(0, d, n, millis());
}

uint32_t BlePosture::droppedPackets() const { return postureInbox.dropped(); }

void BlePosture::processData() {
  const uint32_t generation = postureInbox.generation();
  if (processedPostureGeneration != generation) {
    processedPostureGeneration = generation;
    s_pitch = 0;
    s_roll = 0;
    s_yaw = 0;
    s_lastMeasurementMs = 0;
    s_notificationCount = 0;
    s_imuSample = ImuSample{};
    postureStreamLength = 0;
  }
  BleInbox<8>::Packet packet;
  for (size_t processed = 0; processed < 8 && postureInbox.pop(packet); ++processed) {
    if (packet.generation == postureInbox.generation())
      parsePacket(packet.data, packet.length, packet.receivedMs);
  }
}

void BlePosture::parsePacket(uint8_t *d, size_t n, uint32_t receivedMs) {
  AppLog.packet("POSTURE", d, n);
  for (size_t index = 0; index < n; ++index) {
    if (postureStreamLength == sizeof(postureStream)) {
      memmove(postureStream, postureStream + 1, --postureStreamLength);
    }
    postureStream[postureStreamLength++] = d[index];

    for (;;) {
      size_t header = 0;
      while (header < postureStreamLength && postureStream[header] != 0x55)
        ++header;
      if (header) {
        postureStreamLength -= header;
        memmove(postureStream, postureStream + header, postureStreamLength);
      }
      if (postureStreamLength < 2) break;

      const uint8_t type = postureStream[1];
      const size_t frameLength = type == 0x53 ? 11
          : (type == 0x61 || type == 0x71) ? 20 : 0;
      if (!frameLength) {
        postureStreamLength--;
        memmove(postureStream, postureStream + 1, postureStreamLength);
        continue;
      }
      if (postureStreamLength < frameLength) break;

      bool valid = true;
      if (type == 0x53) {
        uint8_t checksum = 0;
        for (size_t byte = 0; byte < 10; ++byte)
          checksum += postureStream[byte];
        valid = checksum == postureStream[10];
      }
      if (!valid) {
        postureStreamLength--;
        memmove(postureStream, postureStream + 1, postureStreamLength);
        continue;
      }

      parseAngleFrame(postureStream, frameLength, receivedMs);
      postureStreamLength -= frameLength;
      memmove(postureStream, postureStream + frameLength, postureStreamLength);
    }
  }
}

void BlePosture::parseAngleFrame(const uint8_t *data, size_t length, uint32_t receivedMs) {
  const auto readI16 = [data](size_t index) {
    return static_cast<int16_t>(
        static_cast<uint16_t>(data[index]) |
        (static_cast<uint16_t>(data[index + 1]) << 8));
  };
  if (length == 20 && data[1] == 0x61) {
    for (size_t axis = 0; axis < 3; ++axis) {
      s_imuSample.accelerationG[axis] = float(readI16(2 + axis * 2)) * (16.0f / 32768.0f);
      s_imuSample.angularVelocityDps[axis] = float(readI16(8 + axis * 2)) * (2000.0f / 32768.0f);
    }
    s_imuSample.receivedMs = receivedMs;
    ++s_imuSample.sequence;
    s_imuSample.valid = true;
    // WitMotion BLE 5.0 SDK: signed little-endian angle values, ±180° / 32768.
    s_roll = static_cast<float>(readI16(14)) * (180.0f / 32768.0f);
    s_pitch = static_cast<float>(readI16(16)) * (180.0f / 32768.0f);
    s_yaw = static_cast<float>(readI16(18)) * (180.0f / 32768.0f);
  } else if (length == 11 && data[1] == 0x53) {
    s_roll = static_cast<float>(readI16(2)) / 100.0f;
    s_pitch = static_cast<float>(readI16(4)) / 100.0f;
    s_yaw = static_cast<float>(readI16(6)) / 100.0f;
  } else {
    return;
  }
  s_lastMeasurementMs = receivedMs;
  s_notificationCount++;
  if (s_notificationCount == 1)
    AppLog.printf("[BLE-POSTURE] first angles roll=%.2f pitch=%.2f yaw=%.2f\n",
                  s_roll, s_pitch, s_yaw);
}

void BlePosture::begin(const char *deviceName) {
  if (!NimBLEDevice::isInitialized()) {
    NimBLEDevice::init(deviceName);
  }
}

void BlePosture::disconnect() {
  if (s_client) {
    if (s_client->isConnected()) s_client->disconnect();
    NimBLEDevice::deleteClient(s_client);
    s_client = nullptr;
  }
  s_connected = false;
  s_lastMeasurementMs = 0;
  s_pitch = s_roll = s_yaw = 0;
  s_connectedAddress[0] = '\0';
  postureInbox.reset();
}

void BlePosture::cancelConnect() {
  if (s_client && !s_client->isConnected()) s_client->cancelConnect();
}

bool BlePosture::tryConnect(uint32_t scanMs, const BleCancellation *cancelFlag) {
  if (s_connected) return true;

  s_lastError[0] = '\0';

  if (!NimBLEDevice::isInitialized()) {
    snprintf(s_lastError, sizeof(s_lastError), "BLE stack is not initialized");
    return false;
  }

  NimBLEScan *scan = NimBLEDevice::getScan();
  if (!scan) {
    snprintf(s_lastError, sizeof(s_lastError), "BLE scan is unavailable");
    return false;
  }
  if (!bleScanner.run(scanMs ? scanMs : 1500, false, cancelFlag)) {
    snprintf(s_lastError, sizeof(s_lastError), "%s",
             bleScanner.state() == BleScanState::Cancelled ? "cancelled" : bleScanner.error());
    return false;
  }
  NimBLEScanResults results = scan->getResults();

  // 先把地址/名称拷出来再 clearResults()：扫描结果对象持有的是扫描缓存里的
  // 指针，一旦 clearResults() 调用（或缓存被回收），原来取到的
  // NimBLEAdvertisedDevice* 就会失效，再调 ad->getName()/getAddress() 会
  // 解引用已释放内存而崩溃。
  char address[18] = {};
  char name[40] = {};
  uint8_t addressType = BLE_ADDR_PUBLIC;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice *ad = results.getDevice(i);
    if (!ad || !ad->isConnectable()) continue;

    // Match either BLE 5.0/legacy service UUID or the vendor's WT name token.
    const std::string candidateName = ad->getName();
    const char *prefix = BLE_POSTURE_NAME_PREFIX;
    const bool nameHit = candidateName.find(prefix) != std::string::npos;
    if (!ad->isAdvertisingService(kSvcWitBle5) &&
      !ad->isAdvertisingService(kSvcWitBle5Standard) &&
        !ad->isAdvertisingService(kSvcWitLegacy) && !nameHit) continue;

    snprintf(address, sizeof(address), "%s",
             ad->getAddress().toString().c_str());
    snprintf(name, sizeof(name), "%s", candidateName.c_str());
    addressType = ad->getAddress().getType();
    break;
  }
  scan->clearResults();

  if (!address[0]) {
    snprintf(s_lastError, sizeof(s_lastError),
             "posture sensor matching '%s' not seen in last scan",
             BLE_POSTURE_NAME_PREFIX);
    return false;
  }
  AppLog.printf("[BLE-POSTURE] candidate %s (%s)\n",
                name[0] ? name : "unnamed", address);
  return connectDevice(address, addressType, cancelFlag);
}

bool BlePosture::connectDevice(const char *address, uint8_t addressType,
                               const BleCancellation *cancelFlag) {
  if (!address || strlen(address) != 17 || addressType > 3 || !NimBLEDevice::isInitialized()) {
    snprintf(s_lastError, sizeof(s_lastError), "Invalid saved posture device or BLE unavailable");
    return false;
  }

  if (cancelFlag && cancelFlag->cancelled()) {
    snprintf(s_lastError, sizeof(s_lastError), "cancelled");
    return false;
  }

  disconnect();

  NimBLEClient *cli = NimBLEDevice::createClient();
  if (!cli) {
    snprintf(s_lastError, sizeof(s_lastError),
             "could not allocate a BLE client for %s", address);
    return false;
  }
  s_client = cli;
  cli->setClientCallbacks(&postureCallbacks, false);
  cli->setConnectTimeout(8000);

  // 用地址连接（不依赖已清空的扫描缓存），地址类型由 NimBLE 自动解析。
  // 工作线程串行执行，这里阻塞的时间不会与手动扫描/连接重叠。
  if (!cli->connect(NimBLEAddress(std::string(address), addressType))) {
    const int code = cli->getLastError();
    snprintf(s_lastError, sizeof(s_lastError),
             "connect to %s failed (BLE error %d: %s)", address, code,
             NimBLEUtils::returnCodeToString(code));
    AppLog.printf("[BLE-POSTURE] %s\n", s_lastError);
    disconnect();
    return false;
  }

  AppLog.printf("[BLE-POSTURE] link connected %s type=%u; discovering GATT\n",
                address, unsigned(addressType));
  for (auto *service : cli->getServices(true)) {
    AppLog.printf("[BLE-POSTURE] service %s\n", service->getUUID().toString().c_str());
    for (auto *characteristic : service->getCharacteristics(true))
      AppLog.printf("[BLE-POSTURE] characteristic %s notify=%u indicate=%u\n",
                    characteristic->getUUID().toString().c_str(),
                    unsigned(characteristic->canNotify()), unsigned(characteristic->canIndicate()));
  }
  NimBLERemoteService *svc = cli->getService(kSvcWitBle5);
  const NimBLEUUID *characteristicUuid = &kChrDataBle5;
  if (!svc) {
    svc = cli->getService(kSvcWitBle5Standard);
    characteristicUuid = &kChrDataBle5Standard;
  }
  if (!svc) {
    svc = cli->getService(kSvcWitLegacy);
    characteristicUuid = &kChrDataLegacy;
  }
  if (!svc) {
    snprintf(s_lastError, sizeof(s_lastError),
             "%s: service 0xFFE5/0xFFE0 not found", address);
    disconnect();
    return false;
  }

  NimBLERemoteCharacteristic *chr = svc->getCharacteristic(*characteristicUuid);
  if (!chr) {
    snprintf(s_lastError, sizeof(s_lastError),
             "%s: data characteristic not found", address);
    disconnect();
    return false;
  }

  if (!chr->subscribe(true, witNotify, true)) {
    snprintf(s_lastError, sizeof(s_lastError),
             "%s: subscribing to posture data failed", address);
    disconnect();
    return false;
  }

  s_connected = true;
  snprintf(s_connectedAddress, sizeof(s_connectedAddress), "%s", address);
  s_connectedAddressType = addressType;
  AppLog.printf("[BLE-POSTURE] connected: %s\n", address);
  return true;
}
