#pragma once
#include <cstdint>
#include <deque>
#include <vector>

class TwoWire {
public:
  void beginTransmission(uint8_t address) { lastAddress = address; }
  uint8_t endTransmission() const { return ack ? 0 : 2; }
  uint8_t requestFrom(uint8_t address, uint8_t) {
    lastAddress = address;
    availableBytes.assign(frame.begin(), frame.end());
    return frame.size();
  }
  int available() const { return availableBytes.size(); }
  int read() { if (availableBytes.empty()) return -1; const int value = availableBytes.front(); availableBytes.pop_front(); return value; }
  void setCount(uint16_t count) {
    frame = {0, uint8_t(count >> 8), uint8_t(count), 0, 0};
    frame[0] = uint8_t(0 - frame[1] - frame[2]);
  }
  bool ack = true;
  uint8_t lastAddress = 0;
  std::vector<uint8_t> frame;
  std::deque<uint8_t> availableBytes;
};