#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

struct BlePowerDevice {
  char name[32];
  char address[18];
  int8_t rssi;
  bool cyclingPower;
  bool fitnessMachine;
};

// BLE GATT Central supporting Cycling Power (0x1818) and Fitness Machine
// Service Indoor Bike Data (0x1826) power trainers.
class BlePowerMeter {
public:
  void begin(const char *deviceName, float wheelCircM);
  bool scanDevices(BlePowerDevice *devices, size_t capacity, size_t &count);
  bool connectDevice(const char *address);
  void cancelCurrentOperation();
  void disconnect();
  bool connected() const { return s_connected; }
  const char *lastError() const { return s_lastError; }
  float power()   const { return s_power; }
  float cadence() const { return s_cadence; }   // rpm（需功率计发送曲柄数据）
  float speed()   const { return s_speed; }     // m/s（需功率计发送轮速数据）
  uint32_t notificationCount() const { return s_notificationCount; }
  uint32_t disconnectCount() const { return s_disconnectCount; }
  void setWheelCircumference(float m) { s_wheelCirc = m; }

private:
  friend class BlePowerClientCallbacks;
  static void cpmNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify);
  static void indoorBikeNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify);
  static void setError(const char *message);
  static bool   s_connected;
  static bool   s_hasCyclingPower;
  static bool   s_hasFitnessMachine;
  static float  s_power;
  static float  s_cadence;
  static float  s_speed;
  static float  s_wheelCirc;
  static uint32_t s_notificationCount;
  static uint32_t s_disconnectCount;
  static NimBLEClient *s_client;
  static char s_connectedAddress[18];
  static char s_lastError[192];
};
