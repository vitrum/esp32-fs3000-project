#include "fs3000.h"

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
  if (!_present) return false;

  _w->requestFrom(_addr, (uint8_t)5);
  if (_w->available() < 5) return false;

  uint8_t b[5];
  for (int i = 0; i < 5; i++) b[i] = _w->read();

  // 校验和：5 字节之和的低 8 位必须为 0
  if (((b[0] + b[1] + b[2] + b[3] + b[4]) & 0xFF) != 0) return false;

  // 12 位流量数据，仅高字节低 4 位有效
  _raw = ((b[1] & 0x0F) << 8) | b[2];
  mps  = countToMps(_raw);
  return true;
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
