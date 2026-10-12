#pragma once

#include <Arduino.h>

// ============================================================================
// 电源开关（电池开关 / PWR Key）管理
//
// 依据 Waveshare ESP32-S3-LCD-2.8 官方 PWR_Key 示例实现：
//   - GPIO6：电池开关按键输入（内部/外部上拉，按下为低电平）
//   - GPIO7：电源保持控制（拉高锁定供电；拉低切断供电实现关机）
//
// 功能：
//   1) 开机：上电时若检测到按键按下 → 拉高 GPIO7 保持供电（松开后系统继续运行）。
//   2) 关机：长按按键 PWR_LONG_PRESS_MS（默认 5 秒）→ 拉低 GPIO7 断电。
//
// 用法：
//   powerKey.begin();   // setup() 中调用（尽早调用以尽快锁定供电）
//   powerKey.tick();    // loop() 中周期调用（非阻塞）
// ============================================================================

class PowerKey {
public:
  // 初始化 GPIO 并检测开机条件。若按键按下则锁定供电。
  void begin();

  // 轮询按键状态，处理长按关机。非阻塞，需在 loop() 中频繁调用。
  void tick();

  // 立即关机（拉低 PWR_Control 断电）。
  void shutdown();

  bool shuttingDown() const { return _shuttingDown; }

private:
  bool _powerHeld = false;       // 是否已锁定供电
  bool _buttonActive = false;    // 当前按键是否处于按下状态
  bool _shuttingDown = false;    // 正在关机
  uint32_t _pressStartMs = 0;    // 本次按下起始时刻
};
