#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>

class BleCancellation;

// BLE 客户端：维特智能（WitMotion）WT9011DCL-BT50 蓝牙姿态传感器
//
// 设备规格：
//   - BLE 5.0（内置 nRF52832），自供电（电池 ~30h），9g，可绑胸口/上背
//   - 输出：加速度/角速度/角度(Roll·Pitch·Yaw)/磁场/四元数（内置卡尔曼，0.2°）
//   - WitMotion 标准 0x55 协议帧，角度包 0x53，角度单位 0.01°
//
// 接入方式（BLE GATT 客户端）：
//   - 服务：0xFFE0，数据特征（notify）：0xFFE1
//   - 本模块只订阅 notify 并解析 Pitch（俯仰角），作为躯干姿态角
//   - 备注：Yaw 使用磁力计，骑行中受车架/路边金属干扰会漂；Pitch 由
//     重力+陀螺仪融合，不受磁场影响，本模块只取 Pitch。
class BlePosture {
public:
  void begin(const char *deviceName);
  // 扫描并连接姿态传感器。scanMs 由调用方（BLE 工作线程）指定。
  // cancelFlag 指向工作线程的取消标志：为 true 时尽快放弃本次扫描/连接。
  bool tryConnect(uint32_t scanMs = 1500,
                  const BleCancellation *cancelFlag = nullptr);
  void processData();
  uint32_t droppedPackets() const;
  void disconnect();               // 断开并释放客户端
  void cancelConnect();            // 取消未完成的连接/扫描
  void resetAfterStackShutdown() { disconnect(); }
  bool connected() const { return s_connected; }
  const char *lastError() const { return s_lastError; }
  float pitchDeg() const { return s_connected ? s_pitch : 0; }   // 俯仰角（度）
  uint32_t notificationCount() const { return s_notificationCount; }

private:
  static void witNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify);
  friend class BlePostureClientCallbacks;
  static void parsePacket(uint8_t *data, size_t length);
  static std::atomic<bool> s_connected;
  static float  s_pitch;
  static uint32_t s_notificationCount;
  static NimBLEClient *s_client;
  static char   s_lastError[160];
};

