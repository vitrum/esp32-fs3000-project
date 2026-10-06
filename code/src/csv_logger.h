#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// TF 卡 CSV 日志（SDSPI 模式）
// 列：t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc
//     powerSrc: 0=无功率源 1=BLE 功率计 2=ANT+ 功率计
class CsvLogger {
public:
  bool begin();                       // 挂载 SD 并创建新文件，写表头
  bool ok() const { return _ok; }
  void log(uint32_t tMs, float powerW, float cadRpm, float vgMps,
           float vAirMps, float yawDeg, float postureDeg, float rho,
           uint8_t powerSrc);
  const char *fileName() const { return _name; }

private:
  bool   _ok = false;
  File   _f;
  char   _name[40];
  uint32_t _lastFlush = 0;
};
