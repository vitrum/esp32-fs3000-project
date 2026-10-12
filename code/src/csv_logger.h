#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>

#include <FS.h>

bool beginSdCardMutex();
bool lockSdCard(TickType_t timeout = portMAX_DELAY);
void unlockSdCard();

// SD 已挂载的 FATFS 文件系统对象（走 ESP-IDF 原生 SDSPI+FATFS）。
// 路径以 "/" 开头即可（内部会拼接到挂载点）。音频库也用它。
fs::FS &sdFs();
bool sdCardReady();

// ---- SD 卡文件浏览辅助 ----
struct SdFileEntry {
  char name[64];   // 文件名（不含路径）
  uint32_t size;   // 字节
  bool isAudio;    // .mp3/.wav 等可播放音频
};

// 列出 SD 根目录（及 /system）文件，写入 out[]（最多 maxCount），返回数量。
// 音频文件排在前面。失败或无卡返回 0。
size_t listSdFiles(SdFileEntry *out, size_t maxCount);

// SD card CSV log (SDSPI mode)
// 列：t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc
//     powerSrc: 0=none, 1=BLE, 2=ANT+, 3=virtual power meter
class CsvLogger {
public:
  bool begin();                       // 挂载 SD 并创建新文件，写表头
  bool start();
  bool stop();
  bool ok() const { return _ok; }

  // 暂停写盘（保留已打开的文件与文件名），用于音频占用 SD 时避让冲突。
  // 期间 log() 调用会被丢弃，不写 SD。resume() 恢复写同一文件。
  void pause()  { _paused = true; }
  void resume() { _paused = false; _lastFlush = millis(); }
  bool paused() const { return _paused; }

  void log(uint32_t tMs, float powerW, float cadRpm, float vgMps,
           float vAirMps, float yawDeg, float postureDeg, float rho,
           uint8_t powerSrc);
  const char *fileName() const { return _name; }

private:
  bool   _ok = false;
  bool   _sdReady = false;
  bool   _paused = false;      // 音频播放期间暂停写盘，避免 SD 争用
  File   _f;
  char   _name[40];
  uint32_t _lastFlush = 0;
};
