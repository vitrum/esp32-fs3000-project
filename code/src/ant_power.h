#pragma once

#include <Arduino.h>
#include "ant_node.h"      // ant_node_t / ant_node_rx_t / antplus_power_data_t 等

// ANT+ 功率计接收（esp32-ant 库，coexist 模式与 BLE 共用射频）
//
// 依赖：
//   - NimBLE 已初始化（由 ble_power/ble_posture 的 begin() 完成）
//   - 本模块 begin() 会把 NimBLE 扫描切到"被动常开"，ANT 借用这些
//     扫描窗口收包；因此 begin() 之后不要再调用 BLE 的 tryConnect()
//     （会打断被动扫描，BLE 扫描在 ANT 运行期间本就暂停）
//   - BLE 已建立的连接（姿态传感器等）不受影响
//
// 数据：Bike Power profile（标准 power-only page 0x10）
//   - power  : 瞬时功率 W
//   - cadence: 瞬时踏频 rpm（功率计未发曲柄数据时为 0）
//   - deviceNum: 已配对功率计的 ANT 设备号（首次搜索后自动记忆）
class AntPower {
public:
  void begin();                    // 启动 ANT（被动扫描 + coexist + 功率通道）
  void stop();                     // 停止 ANT 并释放射频供 BLE 扫描使用
  bool started()  const { return s_started; }
  bool tracking() const { return s_tracking; }   // 最近是否收到功率页
  float power()   const { return s_power; }      // W
  float cadence() const { return s_cadence; }    // rpm
  uint16_t deviceNum() const { return s_device; }

private:
  static void onData(ant_node_t *, const ant_node_rx_t *, const uint8_t page[8], void *);
  static void onEvent(ant_node_t *, uint8_t ch, uint8_t ev, void *);

  static ant_node_t s_node;
  static bool    s_started;
  static bool    s_tracking;
  static float   s_power;
  static float   s_cadence;
  static uint16_t s_device;
};
