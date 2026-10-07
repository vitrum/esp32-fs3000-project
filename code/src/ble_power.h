#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

struct BlePowerDevice {
  char name[32];
  char address[18];
  int8_t rssi;
};

// BLE 功率计客户端（GATT Central，NimBLE）
// 服务：Cycling Power Service  0x1818
// 特征：Cycling Power Measurement  0x2A63（notify）
//
// 解析协议字段（Bluetooth SIG CPS 规范）：
//   Flags(2B) + Instantaneous Power(2B, W) [+ 可选字段]
//   - bit4 置位：Wheel Revolution Data（4B 累计轮转 + 2B 事件时间 @1/1024s）
//   - bit5 置位：Crank Revolution Data（2B 累计曲柄转数 + 2B 事件时间）
// 车速/踏频由事件时间差分计算，需要轮周长参数。
class BlePowerMeter {
public:
  void begin(const char *deviceName, float wheelCircM);
  bool scanDevices(BlePowerDevice *devices, size_t capacity, size_t &count);
  bool connectDevice(const char *address);
  bool connected() const { return s_connected; }
  float power()   const { return s_power; }
  float cadence() const { return s_cadence; }   // rpm（需功率计发送曲柄数据）
  float speed()   const { return s_speed; }     // m/s（需功率计发送轮速数据）
  uint32_t notificationCount() const { return s_notificationCount; }
  uint32_t disconnectCount() const { return s_disconnectCount; }
  void setWheelCircumference(float m) { s_wheelCirc = m; }

private:
  friend class BlePowerClientCallbacks;
  static void cpmNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify);
  static bool   s_connected;
  static float  s_power;
  static float  s_cadence;
  static float  s_speed;
  static float  s_wheelCirc;
  static uint32_t s_notificationCount;
  static uint32_t s_disconnectCount;
  static NimBLEClient *s_client;
  static char s_connectedAddress[18];
};
