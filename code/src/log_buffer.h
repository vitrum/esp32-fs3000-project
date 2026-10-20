#pragma once

#include <stddef.h>
#include <stdint.h>

// Callers serialize access; offsets remain valid across ring wrap.
template <size_t Capacity> class LogBuffer {
public:
  void append(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; ++i) bytes_[end_++ % Capacity] = data[i];
  }

  size_t read(uint64_t &cursor, uint8_t *output, size_t capacity,
              uint64_t &lost) const {
    lost = cursor < oldest() ? oldest() - cursor : 0;
    if (lost) cursor = oldest();
    if (cursor > end_) cursor = end_;
    size_t length = end_ - cursor < capacity ? size_t(end_ - cursor) : capacity;
    for (size_t i = 0; i < length; ++i) output[i] = bytes_[cursor++ % Capacity];
    return length;
  }

  uint64_t oldest() const { return end_ > Capacity ? end_ - Capacity : 0; }
  uint64_t end() const { return end_; }

private:
  static_assert(Capacity > 0, "Log buffer must not be empty");
  uint8_t bytes_[Capacity] = {};
  uint64_t end_ = 0;
};
