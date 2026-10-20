#pragma once

#include <stddef.h>
#include <stdint.h>

struct FitSample {
  uint32_t milliseconds;
  float power, cadence, speed, wind, yaw, posture, density;
  unsigned source;
};

bool parseFitCsv(const char *line, FitSample &sample);

class FitActivity {
public:
  using Sink = bool (*)(void *, const uint8_t *, size_t);
  static constexpr uint32_t kEpoch = 631065600;
  FitActivity(Sink sink, void *context) : sink_(sink), context_(context) {}
  bool begin(uint32_t count, uint32_t startUtc, uint32_t endUtc,
             uint32_t elapsedMs, uint32_t distanceCm);
  bool record(uint32_t utc, const FitSample &sample, uint32_t distanceCm);
  bool finish();

private:
  struct Field { uint8_t number, size, type; };
  bool bytes(const uint8_t *data, size_t length);
  bool number(uint32_t value, size_t size);
  bool definition(uint8_t local, uint16_t global, const Field *fields,
                  size_t count, bool developer = false);
  bool event(uint32_t utc, uint8_t type);
  bool summary(uint8_t local, bool lap);
  bool ok_ = true;
  Sink sink_;
  void *context_;
  uint16_t crc_ = 0;
  uint32_t remaining_ = 0, start_ = 0, end_ = 0, elapsed_ = 0, distance_ = 0;
};
