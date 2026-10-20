#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>

#include <FS.h>
#include <atomic>

bool beginSdCardMutex();
bool lockSdCard(TickType_t timeout = pdMS_TO_TICKS(1000));
void unlockSdCard();

// SD 已挂载的 FATFS 文件系统对象（走 ESP-IDF 原生 SDSPI+FATFS）。
// 路径以 "/" 开头即可（内部会拼接到挂载点）。音频库也用它。
fs::FS &sdFs();
bool sdCardReady();
// SD 卡 FATFS 挂载点（POSIX 路径，用于 opendir/stat 等）
const char *sdMountPoint();

// ---- SD 卡文件浏览辅助 ----
struct SdFileEntry {
  char name[256];  // FAT long filename, 不含路径
  uint32_t size;   // 字节
  bool isAudio;    // .mp3/.wav 等可播放音频
  bool isDirectory;
};

// 列出指定目录的直接子项（不递归），目录优先、音频文件次之。
// directory 使用以 "/" 开始的卡内路径；失败或无卡返回 0。
size_t listSdFiles(const char *directory, SdFileEntry *out, size_t maxCount);

// SD card CSV log (SDSPI mode)
// 列：t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc
//     powerSrc: 0=none, 1=BLE, 2=reserved (historical ANT+), 3=virtual power meter
// 存储策略（降低 SD 磨损、避免每秒写盘争用）：
//   - log() 每秒调用一次，样本先缓存到内存（默认 60 条 = 1 分钟）。
//   - 缓冲满（或 stop()）时，把整块数据一次性写入 CSV 文件。
//   - 文件位于 /LOG/ 目录；若有本地日期（GPS 定位），则放在 /LOG/<YYYY-MM-DD>/
//     子目录，当天的数据都写入该日期目录。
class CsvLogger {
public:
  bool begin();                       // 挂载 SD，准备 /LOG 目录（不创建文件）
  bool start();                       // 开始一次新的记录（选定目录/文件名，缓存待写）
  bool stop();                        // 结束记录：把剩余缓存写入并关闭文件
  bool ok() const { return _ok; }

  // 由主循环在有本地日期时设置（"YYYY-MM-DD"）。空字符串表示暂无日期。
  // 若日期发生变化（跨天），记录会自动切换到新日期目录下的新文件。
  void setLocalDate(const char *dateYmd);

  // 暂停/恢复提交新批次（保留缓存与文件名）；已交给写任务的批次仍会完成。
  // 暂停期间 log() 仍会缓存样本；缓冲满时丢弃新样本以免无限增长。
  void pause()  { _paused = true; }
  void resume() { _paused = false; _lastFlush = millis(); }
  bool paused() const { return _paused; }

  void log(uint32_t tMs, float powerW, float cadRpm, float vgMps,
           float vAirMps, float yawDeg, float postureDeg, float rho,
           uint8_t powerSrc);
  const char *fileName() const { return _name; }
  uint32_t sampleCount() const { return _sampleCount; }
  uint32_t droppedCount() const { return _sessionDropped; }
  void setUtcReference(uint32_t utc, uint32_t milliseconds);
  bool utcReference(uint32_t &utc, uint32_t &milliseconds) const;
  static constexpr size_t kSessionFiles = 8;
  size_t sessionFileCount() const { return _sessionFileCount; }
  const char *sessionFile(size_t index) const { return _sessionFiles[index]; }
  bool sessionComplete() const { return !_sessionOverflow; }

private:
  struct Sample {
    uint32_t tMs;
    float    powerW;
    float    cadRpm;
    float    vgMps;
    float    vAirMps;
    float    yawDeg;
    float    postureDeg;
    float    rho;
    uint8_t  powerSrc;
  };

  bool ensureDir(const char *path);
  bool openFileForDate(const char *date); // 打开新文件后再切换当前文件
  bool writeHeader(int fd);
  static void writerTaskEntry(void *argument);
  void writerTask();
  bool ensureWriterTask();
  bool queueBuffer();
  bool flushPending(TickType_t timeout);
  bool flushBuffer();                 // 把内存缓冲一次性写入文件并 flush
  static constexpr size_t kMaxSamples = 60;   // 1 分钟（1Hz）
  static constexpr size_t kMaxCsvLineBytes = 160;

  std::atomic<bool> _ok{false};
  bool   _sdReady = false;
  bool   _paused = false;      // 音频播放期间暂停写盘，避免 SD 争用
  TaskHandle_t _writerTaskHandle = nullptr;
  SemaphoreHandle_t _flushDone = nullptr;
  portMUX_TYPE _bufferMux = portMUX_INITIALIZER_UNLOCKED;
  int    _fd = -1;
  char   _name[80] = {};
  char   _dir[48] = {};        // 当前日志目录（/LOG 或 /LOG/<date>）
  char   _date[12] = {};       // 当前本地日期 "YYYY-MM-DD"，空=未知
  uint32_t _lastFlush = 0;
  size_t _flushDataSize = 0;
  size_t _flushOffset = 0;
  bool _syncPending = false;
  char _flushData[kMaxSamples * kMaxCsvLineBytes] = {};

  Sample _buf[kMaxSamples];
  size_t _bufCount = 0;
  uint32_t _droppedSamples = 0;
  uint32_t _sampleCount = 0, _sessionDropped = 0;
  uint32_t _referenceUtc = 0, _referenceMs = 0;
  char _sessionFiles[kSessionFiles][80] = {};
  size_t _sessionFileCount = 0;
  bool _sessionOverflow = false;
  Sample _writeBuf[kMaxSamples];
  size_t _writeCount = 0;
};
