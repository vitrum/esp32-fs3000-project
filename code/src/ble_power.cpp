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

bool BlePowerMeter::tryConnect() {
  if (s_connected) return true;

  NimBLEDevice::getScan()->clearResults();
  NimBLEScanResults results = NimBLEDevice::getScan()->getResults(4, false);

  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice *ad = results.getDevice(i);
    if (!ad->isAdvertisingService(kSvcCP)) continue;   // 只找功率计

    NimBLEClient *cli = NimBLEDevice::createClient();
    if (!cli) return false;

    if (!cli->connect(ad)) {
      NimBLEDevice::deleteClient(cli);
      return false;
    }

    NimBLERemoteService *svc = cli->getService(kSvcCP);
    if (!svc) {
      cli->disconnect();
      NimBLEDevice::deleteClient(cli);
      return false;
    }

    NimBLERemoteCharacteristic *chr = svc->getCharacteristic(kChrCPM);
    if (!chr) {
      cli->disconnect();
      NimBLEDevice::deleteClient(cli);
      return false;
    }

    // 订阅 notify：第二个参数为回调，第三个为写 CCCD 时是否等待应答
    if (!chr->subscribe(true, cpmNotify, true)) {
      cli->disconnect();
      NimBLEDevice::deleteClient(cli);
      return false;
    }

    s_connected = true;
    Serial.printf("BLE power meter connected: %s\n", ad->getAddress().toString().c_str());
    return true;
  }
  return false;
}
