#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "config.h"

// FS3000 数字风速传感器驱动（Renesas）
// - I2C 地址固定 0x28，连续测量，无需初始化命令
// - 读取 5 字节：校验和 + 数据高字节[3:0] + 数据低字节 + 2 字节通用校验
// - 数据校验：5 字节和低 8 位应为 0（数据手册 5.3）
// - 量程与换算表：FS3000-1015（0~15 m/s，输出计数 409~3686）
class FS3000 {
public:
  bool begin(TwoWire &w, uint8_t addr = FS3000_ADDR);
  bool readMps(float &mps);        // 读取一次风速(m/s)，校验通过返回 true
  uint16_t lastRaw() const { return _raw; }
  bool present() const { return _present; }

private:
  TwoWire  *_w = nullptr;
  uint8_t   _addr = FS3000_ADDR;
  uint16_t  _raw = 0;
  bool      _present = false;

  bool probe();                    // 探测器件是否在线（ACK）
  float countToMps(uint16_t count); // 官方典型曲线查表 + 线性插值
};
