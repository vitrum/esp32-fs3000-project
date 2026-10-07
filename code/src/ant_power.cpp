#include "ant_power.h"
#include "config.h"
#include <NimBLEDevice.h>
#include "ant_node.h"

// ---- esp32-ant 全局状态 ----
ant_node_t AntPower::s_node;
bool    AntPower::s_started   = false;
bool    AntPower::s_tracking  = false;
float   AntPower::s_power     = 0.0f;
float   AntPower::s_cadence   = 0.0f;
uint16_t AntPower::s_device   = 0;

// ---------------------------------------------------------------------------
// 回调运行在 ANT 任务上下文（持节点锁）：只拷贝数据，不阻塞、不打印大块
// ---------------------------------------------------------------------------
void AntPower::onData(ant_node_t *, const ant_node_rx_t *rx,
                      const uint8_t page[8], void *) {
  if (rx->device_type != ANTPLUS_DEVTYPE_BIKE_POWER) return;

  antplus_power_data_t pw;
  if (!antplus_power_decode(page, &pw)) return;   // 非 power-only page 0x10

  s_device   = rx->device_num;
  s_power    = (float)pw.instantaneous_power;
  s_cadence  = (pw.instantaneous_cadence == 0xFF)
                   ? 0.0f : (float)pw.instantaneous_cadence;
  s_tracking = true;
}

void AntPower::onEvent(ant_node_t *, uint8_t ch, uint8_t ev, void *) {
  // 事件：ANT_EVENT_RX_SEARCH_TIMEOUT / RX_FAIL / RX_FAIL_GO_TO_SEARCH 等
  // 简短打印便于现场排查；正常跟踪时几乎不出现
  Serial.printf("[ANT] ch%u event 0x%02X\n", ch, ev);
}

void AntPower::begin() {
  if (s_started) return;

  // 1) NimBLE 必须已初始化；把其扫描切为被动常开 —— ANT 的射频窗口载体
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(false);
  scan->start(0, false, true);          // perpetual passive scan

  // 2) 以 coexist 模式启动 ANT 节点（共享射频，BLE 连接保持）
  ant_node_config_t cfg = {};
  cfg.on_data   = onData;
  cfg.on_event  = onEvent;
  cfg.store     = &ant_node_store_nvs;  // 记住已配对功率计，重启自动重连
  cfg.coexist   = true;
#ifdef ANT_POWER_PROXIMITY_RSSI
  cfg.proximity_rssi = ANT_POWER_PROXIMITY_RSSI;
#endif

  ant_espphy_status_t st = ant_node_start(&s_node, &cfg);
  Serial.printf("[ANT] node start: %s\n", ant_espphy_status_str(st));
  if (st != ANT_ESPPHY_OK) return;

  // 3) 打开 ANT+ Bike Power 通道（设备号 0 = 记忆中的功率计，无则搜索第一个）
  uint8_t rc = ant_node_open_antplus_slave(
      &s_node, 0, ANTPLUS_DEVTYPE_BIKE_POWER, 0, ANTPLUS_PERIOD_BIKE_POWER);
  Serial.printf("[ANT] power channel open: rc=%u\n", rc);
  s_started = true;
}

void AntPower::stop() {
  if (!s_started) return;
  ant_node_stop(&s_node);
  NimBLEDevice::getScan()->stop();
  s_started = false;
  s_tracking = false;
  s_power = 0.0f;
  s_cadence = 0.0f;
}
