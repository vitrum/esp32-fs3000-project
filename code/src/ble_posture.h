#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

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
  bool tryConnect();               // 扫描并连接；未连接时可反复调用
  bool connected() const { return s_connected; }
  float pitchDeg() const { return s_pitch; }   // 俯仰角（度）
  uint32_t notificationCount() const { return s_notificationCount; }

private:
  static void witNotify(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t len, bool isNotify);
  static bool   s_connected;
  static float  s_pitch;
  static uint32_t s_notificationCount;
};
