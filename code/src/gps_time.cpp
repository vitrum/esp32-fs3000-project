#include "gps_time.h"

#include <time.h>

#include "config.h"

namespace {
// 串口接收缓冲区：NMEA 单行最长约 82 字符，给足余量。
constexpr int kRxBuffer = 512;
// 定位新鲜度阈值：超过该时长未见有效 location 视为失锁。
constexpr uint32_t kFixFreshMs = 3000;
// 每次 tick 最多从串口读取的字节数：限制单次耗时，保证主循环不被卡住。
// 9600 波特约 960 字节/秒；每次读 64 字节足以跟上，且几乎瞬间返回。
constexpr int kMaxBytesPerTick = 64;
// 模块存在性检测窗口：该时间内若一直收不到任何 NMEA 字节，判为不存在。
constexpr uint32_t kPresenceTimeoutMs = 3000;
}  // namespace

bool GpsTime::begin() {
#if GPS_ENABLE
  if (_enabled) return true;   // 已启动，幂等
  // ESP32-S3：UART2 映射到指定引脚（RX<-GPS TX，TX->GPS RX）
  // 注意：setRxBufferSize 需在 begin() 之前调用才生效。
  Serial2.setRxBufferSize(kRxBuffer);
  Serial2.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  _enabled = true;
  // 探测窗口从主循环开始轮询时计算，不受后续 setup 初始化耗时影响。
  _presenceStarted = false;
  _presenceStartMs = 0;
  _presenceChecked = false;
  _modulePresent = false;
  _lastRxMs = 0;
  _rxBytesTotal = 0;
  Serial.printf("[GPS] UART ready: RX=%d TX=%d baud=%d (ATGM336H)\n",
                PIN_GPS_RX, PIN_GPS_TX, GPS_BAUD);
  return true;
#else
  Serial.println("[GPS] disabled by config");
  _modulePresent = false;
  _presenceChecked = true;
  return false;
#endif
}

void GpsTime::stop() {
  if (_enabled) {
    Serial2.end();
    Serial.println("[GPS] UART stopped (module not detected)");
  }
  _enabled = false;
}

bool GpsTime::restart() {
  Serial.println("[GPS] manual restart requested");
  // 重新初始化串口。ATGM336H 断电/复位后会自动重新输出 NMEA；
  // 这里通过重启 UART 并复位解析器状态实现"软重启"GPS。
  if (_enabled) {
    Serial2.end();
    delay(20);
  }
  _gps = TinyGPSPlus();      // 复位解析器
  _lastFixMs = 0;
  _sentencesSeen = 0;
  _lastSentence[0] = '\0';
  // 复位模块检测状态，重启后重新判定模块是否存在。
  _presenceStarted = false;
  _presenceStartMs = 0;
  _lastRxMs = 0;
  _rxBytesTotal = 0;
  return begin();
}

void GpsTime::tick() {
  if (!_enabled) return;

  // 把串口缓冲中的可读字节喂给解析器，但每次最多读 kMaxBytesPerTick 字节，
  // 避免 GPS 持续吐数据时 while 循环霸占 CPU 导致界面卡死。
  // TinyGPSPlus 是逐字符状态机，调用本身非阻塞。
  int budget = kMaxBytesPerTick;
  while (budget-- > 0 && Serial2.available() > 0) {
    const char c = (char)Serial2.read();
    ++_rxBytesTotal;
    _lastRxMs = millis();
    if (!_modulePresent) _modulePresent = true;   // 收到任何字节即确认存在
    if (_gps.encode(c)) {
      ++_sentencesSeen;
    }
  }

  // 记录最近一次有效定位的时间戳（hasFix() 据此判断是否新鲜）。
  if (_gps.location.isUpdated() && _gps.location.isValid()) {
    _lastFixMs = millis();
  }
}

void GpsTime::pollPresence() {
  if (!_enabled) return;
  if (_presenceChecked) return;   // 已下结论，不再重复判定

  const uint32_t now = millis();
  if (!_presenceStarted) {
    _presenceStarted = true;
    _presenceStartMs = now;
    return;
  }

  // 一旦收到过数据，立即确认存在。
  if (_modulePresent) {
    _presenceChecked = true;
    Serial.println("[GPS] module detected (NMEA data received)");
    return;
  }

  // 超时仍未收到任何数据 → 判为不存在。
  if (now - _presenceStartMs >= kPresenceTimeoutMs) {
    _presenceChecked = true;
    _modulePresent = false;
    Serial.printf("[GPS] no module detected within %lums (RX=%d)\n",
                  (unsigned long)kPresenceTimeoutMs, PIN_GPS_RX);
  }
}

void GpsTime::formatDateTime(char *buf, size_t len) {
  if (!buf || len == 0) return;
  if (!timeValid()) {
    snprintf(buf, len, "--");
    return;
  }
  snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
           _gps.date.year(), _gps.date.month(), _gps.date.day(),
           _gps.time.hour(), _gps.time.minute(), _gps.time.second());
}

void GpsTime::formatLocalDateTime(char *buf, size_t len, int tzOffsetMinutes) {
  if (!buf || len == 0) return;
  if (!timeValid()) {
    snprintf(buf, len, "--");
    return;
  }
  // 用 unixSec + 偏移再反算，处理跨天/跨月/跨年。
  time_t t = (time_t)unixSec() + (time_t)tzOffsetMinutes * 60;
  struct tm *lt = gmtime(&t);
  if (!lt) {
    snprintf(buf, len, "--");
    return;
  }
  snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
           lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
           lt->tm_hour, lt->tm_min, lt->tm_sec);
}

bool GpsTime::hasFix() const {
  if (!_enabled) return false;
  if (!_gps.location.isValid()) return false;
  if (_lastFixMs == 0) return false;
  return (millis() - _lastFixMs) <= kFixFreshMs;
}

bool GpsTime::timeValid() {
  if (!_enabled) return false;
  if (!_gps.date.isValid() || !_gps.time.isValid()) return false;
  const int year = _gps.date.year();
  return year >= 2020 && year <= 2099;   // 排除未定位时的默认/乱值
}

uint8_t GpsTime::satellites() {
  return _gps.satellites.isValid() ? (uint8_t)_gps.satellites.value() : 0;
}

uint32_t GpsTime::hdop() {
  return _gps.hdop.isValid() ? (uint32_t)_gps.hdop.value() : 0;
}

float GpsTime::speedMps() {
  return _gps.speed.isValid() ? (float)_gps.speed.mps() : 0.0f;
}

uint32_t GpsTime::unixSec() {
  if (!timeValid()) return 0;

  // 手动计算 Unix 秒，避免 timegm() 的可移植性问题。
  // 采用 Howard Hinnant 的 days_from_civil 算法（公历，UTC）。
  const int y = _gps.date.year();
  const int m = _gps.date.month();
  const int d = _gps.date.day();

  const int a = (14 - m) / 12;
  const long y2 = (long)y + 4800 - a;
  const long m2 = (long)m + 12 * a - 3;
  long jdn = d + (153 * m2 + 2) / 5 + 365 * y2 + y2 / 4 - y2 / 100 + y2 / 400 - 32045;
  const long daysSinceEpoch = jdn - 2440588L;   // 1970-01-01 的儒略日

  const long secsOfDay = (long)_gps.time.hour() * 3600L +
                         (long)_gps.time.minute() * 60L +
                         (long)_gps.time.second();
  const long long unixSec = (long long)daysSinceEpoch * 86400LL + secsOfDay;
  if (unixSec <= 0) return 0;
  return (uint32_t)unixSec;
}

uint32_t GpsTime::fixAgeMs() const {
  if (_lastFixMs == 0) return UINT32_MAX;
  return millis() - _lastFixMs;
}
