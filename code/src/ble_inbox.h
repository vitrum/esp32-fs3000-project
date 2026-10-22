#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>

template <size_t Slots, size_t MaxBytes = 244> class BleInbox {
public:
  struct Packet {
    uint8_t kind = 0;
    uint32_t generation = 0;
    size_t length = 0;
    uint8_t data[MaxBytes] = {};
  };

  bool push(uint8_t kind, const uint8_t *data, size_t length) {
    if (!data || !length || length > MaxBytes) {
      dropped_.fetch_add(1);
      return false;
    }
    portENTER_CRITICAL(&mux_);
    if (size_ == Slots) {
      portEXIT_CRITICAL(&mux_);
      dropped_.fetch_add(1);
      return false;
    }
    auto &packet = packets_[(head_ + size_) % Slots];
    packet.kind = kind;
    packet.generation = generation_.load();
    packet.length = length;
    memcpy(packet.data, data, length);
    ++size_;
    portEXIT_CRITICAL(&mux_);
    return true;
  }

  bool pop(Packet &packet) {
    portENTER_CRITICAL(&mux_);
    if (!size_) {
      portEXIT_CRITICAL(&mux_);
      return false;
    }
    packet = packets_[head_];
    head_ = (head_ + 1) % Slots;
    --size_;
    portEXIT_CRITICAL(&mux_);
    return true;
  }

  void reset() {
    portENTER_CRITICAL(&mux_);
    head_ = size_ = 0;
    generation_.fetch_add(1);
    portEXIT_CRITICAL(&mux_);
  }

  uint32_t generation() const { return generation_.load(); }
  uint32_t dropped() const { return dropped_.load(); }

private:
  Packet packets_[Slots];
  size_t head_ = 0;
  size_t size_ = 0;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> dropped_{0};
};