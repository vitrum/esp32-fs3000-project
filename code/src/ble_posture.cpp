#include "ble_posture.h"
#include "config.h"
#include <NimBLEDevice.h>

// ---- WitMotion WT9011DCL BLE 协议 ----
static const NimBLEUUID kSvcWit ("0xFFE0");
static const NimBLEUUID kChrData("0xFFE1");

bool   BlePosture::s_connected = false;
float  BlePosture::s_pitch = 0;

// ---------------------------------------------------------------------------
// 0x55 协议帧解析（角度包 0x53）：
//   [0]=0x55 [1]=0x53 [2..3]=Roll [4..5]=Pitch [6..7]=Yaw [8..9]=温度
//   [10]=校验和（前 10 字节和低 8 位）；int16 小端，单位 0.01°
// ---------------------------------------------------------------------------
static void witNotify(NimBLERemoteCharacteristic *chr, uint8_t *d, size_t n,
                      bool isNotify) {
  (void)chr; (void)isNotify;
  if (n < 11) return;

  for (size_t i = 0; i + 11 <= n; i++) {
    if (d[i] != 0x55 || d[i + 1] != 0x53) continue;   // 找角度包

    uint8_t sum = 0;
    for (size_t j = i; j < i + 10; j++) sum += d[j];
    if (sum != d[i + 10]) break;                       // 校验失败，丢弃整包

    int16_t pitch = (int16_t)(d[i + 4] | (d[i + 5] << 8));
    s_pitch = (float)pitch / 100.0f;                   // 0.01°/LSB
    return;
  }
}

void BlePosture::begin(const char *deviceName) {
  if (!NimBLEDevice::isInitialized()) {
    NimBLEDevice::init(deviceName);
  }
}

bool BlePosture::tryConnect() {
  if (s_connected) return true;

  NimBLEDevice::getScan()->clearResults();
  NimBLEScanResults results = NimBLEDevice::getScan()->start(4, false);

  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice *ad = results.getDevice(i);

    // 过滤：广播服务 0xFFE0 或设备名以 BLE_POSTURE_NAME_PREFIX 开头
    std::string name = ad->getName();
    const char *prefix = BLE_POSTURE_NAME_PREFIX;
    bool nameHit = (name.length() >= strlen(prefix) &&
                    strncmp(name.c_str(), prefix, strlen(prefix)) == 0);
    if (!ad->isAdvertisingService(kSvcWit) && !nameHit) continue;

    NimBLEClient *cli = NimBLEDevice::createClient();
    if (!cli) return false;

    if (!cli->connect(ad)) {
      NimBLEDevice::deleteClient(cli);
      return false;
    }

    NimBLERemoteService *svc = cli->getService(kSvcWit);
    if (!svc) {
      cli->disconnect();
      NimBLEDevice::deleteClient(cli);
      return false;
    }

    NimBLERemoteCharacteristic *chr = svc->getCharacteristic(kChrData);
    if (!chr) {
      cli->disconnect();
      NimBLEDevice::deleteClient(cli);
      return false;
    }

    if (!chr->subscribe(true, witNotify, true)) {
      cli->disconnect();
      NimBLEDevice::deleteClient(cli);
      return false;
    }

    s_connected = true;
    Serial.printf("BLE posture connected: %s\n",
                  ad->getAddress().toString().c_str());
    return true;
  }
  return false;
}
