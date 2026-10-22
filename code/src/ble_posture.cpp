#include "ble_posture.h"
#include "config.h"
#include "runtime_log.h"
#include <NimBLEDevice.h>
#include "ble_inbox.h"
#include "ble_scan.h"

namespace {
BleInbox<8> postureInbox;
uint32_t processedPostureGeneration = 0;
}

// ---- WitMotion WT9011DCL BLE 协议 ----
static const NimBLEUUID kSvcWit ("0xFFE0");
static const NimBLEUUID kChrData("0xFFE1");

std::atomic<bool> BlePosture::s_connected{false};
float  BlePosture::s_pitch = 0;
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
// 0x55 协议帧解析（角度包 0x53）：
//   [0]=0x55 [1]=0x53 [2..3]=Roll [4..5]=Pitch [6..7]=Yaw [8..9]=温度
//   [10]=校验和（前 10 字节和低 8 位）；int16 小端，单位 0.01°
// ---------------------------------------------------------------------------
void BlePosture::witNotify(NimBLERemoteCharacteristic *chr, uint8_t *d, size_t n,
                           bool isNotify) {
  (void)chr; (void)isNotify;
  postureInbox.push(0, d, n);
}

uint32_t BlePosture::droppedPackets() const { return postureInbox.dropped(); }

void BlePosture::processData() {
  const uint32_t generation = postureInbox.generation();
  if (processedPostureGeneration != generation) {
    processedPostureGeneration = generation;
    s_pitch = 0;
    s_notificationCount = 0;
  }
  BleInbox<8>::Packet packet;
  for (size_t processed = 0; processed < 8 && postureInbox.pop(packet); ++processed) {
    if (packet.generation == postureInbox.generation()) parsePacket(packet.data, packet.length);
  }
}

void BlePosture::parsePacket(uint8_t *d, size_t n) {
  AppLog.packet("POSTURE/FFE1", d, n);
  if (n < 11) return;

  for (size_t i = 0; i + 11 <= n; i++) {
    if (d[i] != 0x55 || d[i + 1] != 0x53) continue;   // 找角度包

    uint8_t sum = 0;
    for (size_t j = i; j < i + 10; j++) sum += d[j];
    if (sum != d[i + 10]) break;                       // 校验失败，丢弃整包

    int16_t pitch = (int16_t)(d[i + 4] | (d[i + 5] << 8));
    s_pitch = (float)pitch / 100.0f;                   // 0.01°/LSB
    s_notificationCount++;
    return;
  }
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

    // 过滤：广播服务 0xFFE0 或设备名以 BLE_POSTURE_NAME_PREFIX 开头
    const std::string candidateName = ad->getName();
    const char *prefix = BLE_POSTURE_NAME_PREFIX;
    const bool nameHit = candidateName.length() >= strlen(prefix) &&
                         strncmp(candidateName.c_str(), prefix, strlen(prefix)) == 0;
    if (!ad->isAdvertisingService(kSvcWit) && !nameHit) continue;

    snprintf(address, sizeof(address), "%s",
             ad->getAddress().toString().c_str());
    snprintf(name, sizeof(name), "%s", candidateName.c_str());
    addressType = ad->getAddress().getType();
    break;
  }
  scan->clearResults();

  if (!address[0]) {
    snprintf(s_lastError, sizeof(s_lastError),
             "posture sensor '%s*' not seen in last scan", BLE_POSTURE_NAME_PREFIX);
    return false;
  }
  AppLog.printf("[BLE-POSTURE] candidate %s (%s)\n",
                name[0] ? name : "unnamed", address);

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

  NimBLERemoteService *svc = cli->getService(kSvcWit);
  if (!svc) {
    snprintf(s_lastError, sizeof(s_lastError),
             "%s: service 0xFFE0 not found", address);
    disconnect();
    return false;
  }

  NimBLERemoteCharacteristic *chr = svc->getCharacteristic(kChrData);
  if (!chr) {
    snprintf(s_lastError, sizeof(s_lastError),
             "%s: characteristic 0xFFE1 not found", address);
    disconnect();
    return false;
  }

  if (!chr->subscribe(true, witNotify, true)) {
    snprintf(s_lastError, sizeof(s_lastError),
             "%s: subscribing 0xFFE1 failed", address);
    disconnect();
    return false;
  }

  s_connected = true;
  AppLog.printf("[BLE-POSTURE] connected: %s\n", address);
  return true;
}
