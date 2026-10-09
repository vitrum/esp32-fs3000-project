#include "fs3000.h"
#include <string.h>

// FS3000-1015 官方典型输出曲线（Renesas 数据手册 Figure 3, Rev.1.01）
// 风速 m/s → 输出计数
static const float    kMps[] = {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15};
static const uint16_t kCnt[] = {409, 1203, 1597, 1908, 2187, 2400, 2629,
                                2801, 3006, 3178, 3309, 3563, 3686};
static const int kTableN = sizeof(kMps) / sizeof(kMps[0]);

bool FS3000::begin(TwoWire &w, uint8_t addr) {
  _w = &w;
  _addr = addr;
  _present = probe();
  return _present;
}

bool FS3000::probe() {
  _w->beginTransmission(_addr);
  return _w->endTransmission() == 0;
}

bool FS3000::readMps(float &mps) {
  ++_readAttempts;
  if (!_present) {
    _lastError = ReadError::NotPresent;
    return false;
  }

  memset(_lastFrame, 0, sizeof(_lastFrame));
  const uint8_t received = _w->requestFrom(_addr, (uint8_t)sizeof(_lastFrame));
  _lastBytesReceived = 0;
  while (_w->available() > 0) {
    const int value = _w->read();
    if (value < 0) break;
    if (_lastBytesReceived < sizeof(_lastFrame))
      _lastFrame[_lastBytesReceived++] = (uint8_t)value;
  }
  if (received != sizeof(_lastFrame) || _lastBytesReceived != sizeof(_lastFrame)) {
    ++_shortReads;
    if (++_consecutiveFailures >= 8) _present = false;
    _lastError = ReadError::ShortRead;
    return false;
  }

  // 校验和：5 字节之和的低 8 位必须为 0
  uint8_t checksum = 0;
  for (uint8_t byte : _lastFrame) checksum += byte;
  if (checksum != 0) {
    ++_checksumErrors;
    if (++_consecutiveFailures >= 8) _present = false;
    _lastError = ReadError::Checksum;
    return false;
  }

  // 12 位流量数据，仅高字节低 4 位有效
  _raw = ((uint16_t)(_lastFrame[1] & 0x0F) << 8) | _lastFrame[2];
  mps  = countToMps(_raw);
  ++_readSuccesses;
  _consecutiveFailures = 0;
  _lastError = ReadError::None;
  return true;
}

const char *FS3000::lastErrorName() const {
  switch (_lastError) {
    case ReadError::None: return "OK";
    case ReadError::NotPresent: return "NOT_PRESENT";
    case ReadError::ShortRead: return "SHORT_READ";
    case ReadError::Checksum: return "CHECKSUM";
  }
  return "UNKNOWN";
}

float FS3000::countToMps(uint16_t count) {
  if (count <= kCnt[0]) return 0.0f;
  if (count >= kCnt[kTableN - 1]) return kMps[kTableN - 1];

  for (int i = 1; i < kTableN; i++) {
    if (count <= kCnt[i]) {
      float t = (float)(count - kCnt[i - 1]) / (float)(kCnt[i] - kCnt[i - 1]);
      return kMps[i - 1] + t * (kMps[i] - kMps[i - 1]);
    }
  }
  return 0.0f;
}
