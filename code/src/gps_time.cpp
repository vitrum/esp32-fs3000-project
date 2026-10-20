#include "gps_time.h"
#include "runtime_log.h"

#include <time.h>
#include <driver/uart.h>
#include <esp_err.h>

#include "config.h"

namespace {
constexpr uart_port_t kGpsUart = UART_NUM_2;
constexpr int kRxBuffer = 4096;
// 定位新鲜度阈值：超过该时长未见有效 location 视为失锁。
constexpr uint32_t kFixFreshMs = 3000;
// 模块存在性检测窗口：该时间内若一直收不到任何 NMEA 字节，判为不存在。
constexpr uint32_t kPresenceTimeoutMs = 3000;
// UTC 时间新鲜度阈值：超过该时长未更新 RMC 时间，视为不可信。
constexpr uint32_t kTimeFreshMs = 5000;
// 卫星数合理上限：超过即视为解析到脏数据（日志中出现过 sat=300）。
constexpr uint16_t kMaxPlausibleSatellites = 64;
}  // namespace

bool GpsTime::begin() {
#if GPS_ENABLE
  if (_enabled) return true;   // 已启动，幂等
  if (!_receiverStopped) _receiverStopped = xSemaphoreCreateBinary();
  if (!_receiverStopped) {
    AppLog.println("[GPS] could not allocate receiver stop semaphore");
    return false;
  }
  uart_config_t config = {};
  config.baud_rate = GPS_BAUD;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_APB;
  esp_err_t err = uart_param_config(kGpsUart, &config);
  if (err == ESP_OK)
    err = uart_set_pin(kGpsUart, PIN_GPS_TX, PIN_GPS_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (err == ESP_OK)
    err = uart_driver_install(kGpsUart, kRxBuffer, 0, 16, &_uartEvents, 0);
  if (err != ESP_OK) {
    AppLog.printf("[GPS] UART initialization failed: %s\n", esp_err_to_name(err));
    return false;
  }
  portENTER_CRITICAL(&_snapshotMux);
  _received = ParsedState();
  _stopRequested = false;
  portEXIT_CRITICAL(&_snapshotMux);
  if (xTaskCreate(receiverTaskEntry, "gpsReceiver", 4096, this, 2,
                  &_receiverTask) != pdPASS) {
    uart_driver_delete(kGpsUart);
    _uartEvents = nullptr;
    AppLog.println("[GPS] could not create receiver task");
    return false;
  }
  _enabled = true;
  // 探测窗口从主循环开始轮询时计算，不受后续 setup 初始化耗时影响。
  _presenceStarted = false;
  _presenceStartMs = 0;
  _presenceChecked = false;
  _modulePresent = false;
  _lastRxMs = 0;
  _rxBytesTotal = 0;
  AppLog.printf("[GPS] UART2 receiver ready: RX=%d TX=%d baud=%d buffer=%d\n",
                PIN_GPS_RX, PIN_GPS_TX, GPS_BAUD, kRxBuffer);
  return true;
#else
  AppLog.println("[GPS] disabled by config");
  _modulePresent = false;
  _presenceChecked = true;
  return false;
#endif
}

void GpsTime::stop() {
  if (_enabled) {
    portENTER_CRITICAL(&_snapshotMux);
    _stopRequested = true;
    portEXIT_CRITICAL(&_snapshotMux);
    xSemaphoreTake(_receiverStopped, portMAX_DELAY);
    _receiverTask = nullptr;
    const esp_err_t err = uart_driver_delete(kGpsUart);
    if (err != ESP_OK)
      AppLog.printf("[GPS] UART shutdown failed: %s\n", esp_err_to_name(err));
    _uartEvents = nullptr;
    AppLog.println("[GPS] receiver stopped");
  }
  _enabled = false;
}

bool GpsTime::restart() {
  AppLog.println("[GPS] manual restart requested");
  stop();
  _gps = ParsedState();
  _lastFixMs = 0;
  _sentencesSeen = 0;
  _lastSentence[0] = '\0';
  // 复位模块检测状态，重启后重新判定模块是否存在。
  _presenceStarted = false;
  _presenceStartMs = 0;
  _lastRxMs = 0;
  _rxBytesTotal = 0;
  _charsProcessed = _failedChecksum = _overflowCount = 0;
  _reportedOverflows = _reportedChecksumFailures = 0;
  return begin();
}

void GpsTime::receiverTaskEntry(void *argument) {
  static_cast<GpsTime *>(argument)->receiverTask();
}

void GpsTime::receiverTask() {
  TinyGPSPlus parser;
  ParsedState state;
  char line[sizeof(state.sentence)] = {};
  size_t lineSize = 0;
  uint8_t bytes[256];
  for (;;) {
    portENTER_CRITICAL(&_snapshotMux);
    const bool stopRequested = _stopRequested;
    portEXIT_CRITICAL(&_snapshotMux);
    if (stopRequested) break;

    uart_event_t event;
    while (xQueueReceive(_uartEvents, &event, 0) == pdTRUE) {
      if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL)
        ++state.overflows;
    }
    const int count = uart_read_bytes(kGpsUart, bytes, sizeof(bytes),
                                      pdMS_TO_TICKS(20));
    if (count < 0) {
      AppLog.println("[GPS] UART read failed");
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (count > 0) state.lastRxMs = millis();
    for (int i = 0; i < count; ++i) {
      const char c = (char)bytes[i];
      ++state.rxBytes;
      if (c == '$') lineSize = 0;
      if (lineSize < sizeof(line) - 1) line[lineSize++] = c;
      if (c == '\n') {
        line[lineSize] = '\0';
        memcpy(state.sentence, line, sizeof(line));
        lineSize = 0;
      }
      if (parser.encode(c)) {
        ++state.sentences;
        if (parser.location.isUpdated() && parser.location.isValid()) {
          state.lastFixMs = millis();
          parser.location.lat(); // Clear isUpdated so time-only sentences do not refresh a fix.
        }
      }
    }
    state.location = parser.location;
    state.date = parser.date;
    state.time = parser.time;
    state.speed = parser.speed;
    state.course = parser.course;
    state.altitude = parser.altitude;
    state.satellites = parser.satellites;
    state.hdop = parser.hdop;
    state.checksumFailures = parser.failedChecksum();
    portENTER_CRITICAL(&_snapshotMux);
    _received = state;
    portEXIT_CRITICAL(&_snapshotMux);
    vTaskDelay(1);
  }
  xSemaphoreGive(_receiverStopped);
  vTaskDelete(nullptr);
}

void GpsTime::tick() {
  if (!_enabled) return;
  portENTER_CRITICAL(&_snapshotMux);
  _gps = _received;
  portEXIT_CRITICAL(&_snapshotMux);
  _lastFixMs = _gps.lastFixMs;
  _lastRxMs = _gps.lastRxMs;
  _rxBytesTotal = _charsProcessed = _gps.rxBytes;
  _sentencesSeen = _gps.sentences;
  _failedChecksum = _gps.checksumFailures;
  _overflowCount = _gps.overflows;
  memcpy(_lastSentence, _gps.sentence, sizeof(_lastSentence));
  if (_rxBytesTotal > 0) _modulePresent = true;
  const uint32_t now = millis();
  if ((_overflowCount != _reportedOverflows ||
       _failedChecksum != _reportedChecksumFailures) &&
      now - _lastDiagnosticsMs >= 5000) {
    _lastDiagnosticsMs = now;
    _reportedOverflows = _overflowCount;
    _reportedChecksumFailures = _failedChecksum;
    AppLog.printf("[GPS] rx=%lu checksum_failures=%lu overflows=%lu\n",
                  (unsigned long)_rxBytesTotal, (unsigned long)_failedChecksum,
                  (unsigned long)_overflowCount);
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
    AppLog.println("[GPS] module detected (NMEA data received)");
    return;
  }

  // 超时仍未收到任何数据 → 判为不存在。
  if (now - _presenceStartMs >= kPresenceTimeoutMs) {
    _presenceChecked = true;
    _modulePresent = false;
    AppLog.printf("[GPS] no module detected within %lums (RX=%d)\n",
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
  if (year < 2020 || year > 2099) return false;   // 排除未定位时的默认/乱值

  // 时间必须新鲜：RMC 时间语句过期（例如模块输出陈旧/半帧数据）时不可信。
  // _gps.time.age() 单位是毫秒。
  if (_gps.time.age() > kTimeFreshMs) return false;

  // 卫星数必须落在合理区间。TinyGPSPlus 的 satellites 若被脏帧污染，
  // 会出现 300 这类荒谬值（日志中出现过 sat=300），此时数据不可信。
  const uint16_t sats = _gps.satellites.isValid()
                            ? (uint16_t)_gps.satellites.value()
                            : 0;
  if (sats > kMaxPlausibleSatellites) return false;

  // 时间/日期来自 RMC，其可靠性依赖有效定位。若从未定位过，宁可视作不可信，
  // 避免把「无定位时的空时间戳」当成可信时间用于 FIT 锚点。
  // 注意：这里只要求「曾经定位过」，不要求当前 fix 新鲜（记录中会短暂丢星）。
  if (_lastFixMs == 0) return false;

  return true;
}

uint8_t GpsTime::satellites() {
  if (!_gps.satellites.isValid()) return 0;
  const uint32_t sats = _gps.satellites.value();
  // 脏帧会给出 300 这类荒谬值，直接归零，避免 UI/日志显示误导信息。
  return sats <= kMaxPlausibleSatellites ? (uint8_t)sats : 0;
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
