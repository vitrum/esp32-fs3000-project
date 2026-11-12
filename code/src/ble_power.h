#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>

class BleCancellation;
static constexpr size_t kBleDevicePageSize = 4;

class BleDevicePages {
public:
  void reset() { page_ = 0; }
  void move(bool next, size_t count) {
    if (next && page_ + 1 < pages(count)) ++page_;
    else if (!next && page_ > 0) --page_;
  }
  size_t page() const { return page_; }
  size_t offset() const { return page_ * kBleDevicePageSize; }
  static size_t pages(size_t count) {
    return count / kBleDevicePageSize + (count % kBleDevicePageSize != 0);
  }
private:
  size_t page_ = 0;
};

struct BlePowerDevice {
  char name[32];
  char address[18];
  int8_t rssi;
  bool cyclingPower;
  bool fitnessMachine;
  bool speedCadence;
  // 扫描阶段记录的真实地址类型（0=PUBLIC, 1=RANDOM_STATIC, 2=RANDOM_PRIVATE_RESOLVABLE,
  // 3=RANDOM_PRIVATE_NON_RESOLVABLE，见 BLE_ADDR_*）。连接时必须用真实类型，
  // 否则「用 PUBLIC 去连 RANDOM 地址」会导致对端不应答、8s 后 ETIMEOUT(code 13)。
  uint8_t addressType;
  bool postureSensor;
  bool favorite;
  bool heartRate;
  bool supported;
  // Assioma 双脚踏识别：0=非Assioma, 1=左脚踏(L), 2=右脚踏(R)
  // 支持 AssiomaPRO\d+L/R 等任意数字的型号
  uint8_t assiomaLR;  // 0=none, 1=left, 2=right
};

// BLE GATT Central supporting Cycling Power, FTMS trainers, and CSC sensors.
class BlePowerMeter {
public:
  void begin(const char *deviceName, float wheelCircM);
  // 成功时 devices 由调用方通过 free() 释放。
  bool scanDevices(BlePowerDevice *&devices, size_t &count,
                   uint32_t durationMs, const BleCancellation *cancel = nullptr);
  void processData();
  uint32_t droppedPackets() const;
  // addressType 来自扫描结果（BlePowerDevice::addressType）。0xFF 表示未知，
  // 此时按 PUBLIC 连接，失败后不再重试其它类型。
  bool connectDevice(const char *address, uint8_t addressType = 0xFF);
  void cancelCurrentOperation();
  void clearScanResults();
  void disconnect();
  bool connected() const { return s_connected; }
  bool hasPowerService() const { return s_connected && (s_hasCyclingPower || s_hasFitnessMachine); }
  const char *lastError() const { return s_lastError; }
  float power()   const { return s_connected ? s_power.load() : 0; }
  float cadence() const { return s_connected ? s_cadence.load() : 0; }   // rpm（需功率计发送曲柄数据）
  float speed()   const { return s_connected ? s_speed.load() : 0; }     // m/s（需功率计发送轮速数据）
  uint32_t notificationCount() const { return s_notificationCount; }
  uint32_t fitnessMachineNotificationCount() const { return s_fitnessMachineNotificationCount; }
  uint16_t lastFitnessMachineFlags() const { return s_lastFitnessMachineFlags; }
  uint32_t disconnectCount() const { return s_disconnectCount; }
  uint32_t measurementMs() const { return s_powerReceivedMs.load(); }
  bool measurementValid(uint32_t now, uint32_t maxAgeMs = 3000) const {
    return s_connected && s_powerValid && now - s_powerReceivedMs.load() <= maxAgeMs;
  }
  void setWheelCircumference(float m) { s_wheelCirc = m; }

private:
  friend class BlePowerClientCallbacks;
  static void cpmNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify);
  static void indoorBikeNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify);
  static void speedCadenceNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify);
  static void parseCyclingPower(uint8_t *data, size_t length);
  static void parseIndoorBike(uint8_t *data, size_t length);
  static void parseSpeedCadence(uint8_t *data, size_t length);
  static void setError(const char *message);
  static std::atomic<bool> s_connected;
  static bool   s_hasCyclingPower;
  static bool   s_hasFitnessMachine;
  static std::atomic<float> s_power;
  static std::atomic<bool> s_powerValid;
  static std::atomic<uint32_t> s_powerReceivedMs;
  static std::atomic<float> s_cadence;
  static std::atomic<float> s_speed;
  static float  s_wheelCirc;
  static uint32_t s_notificationCount;
  static uint32_t s_fitnessMachineNotificationCount;
  static uint16_t s_lastFitnessMachineFlags;
  static std::atomic<uint32_t> s_disconnectCount;
  static NimBLEClient *s_client;
  static char s_connectedAddress[18];
  static char s_lastError[320];
  static bool s_hasSpeedCadence;
};

