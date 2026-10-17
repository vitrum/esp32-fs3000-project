#pragma once

#include <Arduino.h>
#include <TinyGPSPlus.h>

#include "config.h"

// ============================================================================
// GPS / 北斗双模模块（ATGM336H 等）——UART NMEA 解析
//
// 职责：
//   - 从串口喂入 NMEA 语句（TinyGPSPlus 解析）
//   - 提供：定位有效性、经纬度、UTC 时间、地速、海拔、卫星数、HDOP
//   - 地速可替代/补充功率计轮速（CdA 公式的 v_g）
//
// 本版本不使用 PPS 秒脉冲；PPS 预留接口见 config.h（GPS_USE_PPS）。
// 用法：
//   gps.begin();      // setup()
//   gps.tick();       // loop()，非阻塞
//   if (gps.hasFix()) vg = gps.speedMps();
// ============================================================================
class GpsTime {
public:
  // 初始化串口（使用 config.h 的 PIN_GPS_RX/PIN_GPS_TX/GPS_BAUD）。
  // 仅打开 UART，不阻塞、不主动搜星。返回是否打开成功。
  bool begin();

  // 非阻塞：把串口可读字节喂给解析器。需在 loop() 中频繁调用。
  // 每次调用最多读取有限字节，保证不阻塞主循环（界面不卡死）。
  void tick();

  // 手动重启 GPS：重新初始化串口并复位解析器状态。
  // 返回是否成功（重新 begin 成功即视为成功）。
  bool restart();

  bool enabled() const { return _enabled; }

  // ---- 模块检测 ----
  // 在 loop() 中周期性调用；统计 UART 上是否持续有 NMEA 字节流入，
  // 以此判断"GPS 模块是否存在"。非阻塞。
  void pollPresence();
  // 模块检测结果：true=确认存在（曾收到过 NMEA 数据）。
  bool modulePresent() const { return _modulePresent; }
  // 是否已完成一次存在性判定（超时仍未收到数据即判为不存在）。
  bool presenceChecked() const { return _presenceChecked; }

  // ---- 状态 ----
  bool  hasFix() const;          // 是否已定位（且数据新鲜）
  bool  timeValid();             // UTC 时间是否可信
  uint8_t satellites();          // 卫星数
  uint32_t hdop();               // HDOP 值（TinyGPSPlus 内部为整数）

  // ---- 数据（无效时返回 0 / 上次值）----
  // 注：TinyGPSPlus 的访问器为非 const，故这些接口也非 const。
  double latitude()  { return _gps.location.isValid() ? _gps.location.lat() : 0.0; }
  double longitude() { return _gps.location.isValid() ? _gps.location.lng() : 0.0; }
  float  speedMps();             // 地速 m/s
  float  altitudeM() { return _gps.altitude.isValid() ? (float)_gps.altitude.meters() : 0.0f; }
  float  courseDeg() { return _gps.course.isValid() ? (float)_gps.course.deg() : 0.0f; }

  // ---- 绝对 UTC 时间 ----
  // unixSec：自 1970-01-01 UTC 的秒数；无效返回 0。
  uint32_t unixSec();
  // fixAgeMs：距最近一次有效定位更新的毫秒数（用于判断数据是否新鲜）。
  uint32_t fixAgeMs() const;

  // UTC 日期时间各字段（无效时返回 0）。
  int year()   { return _gps.date.isValid() ? _gps.date.year()   : 0; }
  int month()  { return _gps.date.isValid() ? _gps.date.month()  : 0; }
  int day()    { return _gps.date.isValid() ? _gps.date.day()    : 0; }
  int hour()   { return _gps.time.isValid() ? _gps.time.hour()   : 0; }
  int minute() { return _gps.time.isValid() ? _gps.time.minute() : 0; }
  int second() { return _gps.time.isValid() ? _gps.time.second() : 0; }
  // 年龄：距最近一次有效 UTC 时间更新的秒数（TinyGPSPlus 提供）。
  uint32_t timeAgeMs() { return _gps.time.age(); }

  // 格式化为 "YYYY-MM-DD HH:MM:SS"（UTC）。时间无效时写入 "--"。
  void formatDateTime(char *buf, size_t len);
  // 格式化为 "YYYY-MM-DD HH:MM:SS"（本地时间，按 tzOffsetMinutes 偏移）。
  void formatLocalDateTime(char *buf, size_t len, int tzOffsetMinutes);

  // 最近一次收到的原始 NMEA 行（调试用）。
  const char *lastSentence() const { return _lastSentence; }
  // 解析统计
  uint32_t sentencesSeen() const { return _sentencesSeen; }
  uint32_t charsProcessed() const { return (uint32_t)_gps.charsProcessed(); }

private:
  TinyGPSPlus _gps;
  bool     _enabled = false;
  uint32_t _lastFixMs = 0;        // 最近一次 location 有效的时间戳
  uint32_t _sentencesSeen = 0;
  char     _lastSentence[96] = {};

  // ---- 模块存在性检测 ----
  bool     _modulePresent = false;    // 是否确认收到过 NMEA 字节
  bool     _presenceChecked = false;  // 是否已下结论（存在或超时不存在）
  bool     _presenceStarted = false;  // 检测计时是否已启动
  uint32_t _presenceStartMs = 0;      // 检测起始时间
  uint32_t _lastRxMs = 0;             // 最近一次收到字节的时间
  uint32_t _rxBytesTotal = 0;         // 累计收到的字节数（用于判断是否有数据流入）
};
