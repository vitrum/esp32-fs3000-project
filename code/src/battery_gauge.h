#pragma once

#include <cstdint>

// 电池电量表：把电压映射成 0..4 格。
//
// 硬件说明：Waveshare ESP32-S3-Touch-LCD-2.8 没有把电池电压引到 ADC，
// 板上只有充电管理芯片（充电中/充满用 LED 指示）。因此本模块把
// 「电压」当作输入，由调用方决定来源：
//   - 模拟源（默认）：BatteryGauge::Simulated
//   - 外部 ADC 分压：填好 PIN_BATTERY_ADC 后改用 AdcReading
//
// 3.7V 单节锂电的典型放电曲线（空载）：
//   4.20V 满 -> 3.30V 空。这里用保守的分段，避免显示虚电。
class BatteryGauge {
public:
  static constexpr uint8_t Cells = 4;

  // 电压 -> 格数（0..4）。< 3.30V 视为 0 格，>= 4.05V 视为满格。
  static uint8_t cellsForVoltage(float voltage) {
    if (!(voltage > 0.0f)) return 0;          // 含 NaN
    if (voltage >= 4.05f) return 4;
    if (voltage >= 3.85f) return 3;
    if (voltage >= 3.65f) return 2;
    if (voltage >= 3.45f) return 1;
    return 0;
  }

  // 格数 -> 图标是否整体告警（0 格时用红色）。
  static bool critical(uint8_t cells) { return cells == 0; }

  // 模拟电压：以「已用时长」模拟缓慢放电，用于没有 ADC 的演示固件。
  // 从 4.10V 起，按每 10 分钟下降 0.08V 估算，永不低于 3.30V。
  static float simulatedVoltage(uint32_t uptimeMs) {
    const float hours = static_cast<float>(uptimeMs) / 3600000.0f;
    float voltage = 4.10f - hours * 0.48f;    // 0.08V / 10min => 0.48V/h
    if (voltage < 3.30f) voltage = 3.30f;
    return voltage;
  }
};
