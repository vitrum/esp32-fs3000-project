#include "fit_activity.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

static bool writeVector(void *context, const uint8_t *data, size_t length) {
  auto &bytes = *static_cast<std::vector<uint8_t> *>(context);
  bytes.insert(bytes.end(), data, data + length);
  return true;
}

static uint16_t crc(const uint8_t *data, size_t length) {
  uint16_t value = 0;
  for (size_t i = 0; i < length; ++i) {
    value ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      value = value & 1 ? (value >> 1) ^ 0xA001 : value >> 1;
  }
  return value;
}

int main(int argc, char **argv) {
  FitSample sample;
  assert(parseFitCsv("1000,250.0,90.0,10.000,11.200,-1.0,-15.0,1.2250,1\n", sample));
  assert(!parseFitCsv("1000,250,90,nan,11,0,0,1.2,1\n", sample));
  assert(!parseFitCsv("1000,250,90,10,11,0,0,1.2,1,extra\n", sample));
  assert(!parseFitCsv("1000,250,90,10,11,0,0,1.2,2\n", sample));
  assert(!parseFitCsv("1000,250,90,10,11\n", sample));
  std::vector<uint8_t> bytes;
  FitActivity writer(writeVector, &bytes);
  constexpr uint32_t utc = 1792483200;
  assert(writer.begin(3, utc, utc + 2, 2000, 2000));
  for (unsigned i = 0; i < 3; ++i) {
    assert(parseFitCsv("1000,250.0,90.0,10.000,11.200,-1.0,-15.0,1.2250,1\n", sample));
    assert(writer.record(utc + i, sample, i * 1000));
  }
  assert(writer.finish());
  assert(bytes.size() == 14 + 480 + 3 * 31 + 2);
  assert(!memcmp(bytes.data() + 8, ".FIT", 4));
  const uint32_t length = bytes[4] | bytes[5] << 8 | bytes[6] << 16 | bytes[7] << 24;
  assert(length + 16 == bytes.size());
  assert(crc(bytes.data(), 14) == 0 && crc(bytes.data(), bytes.size()) == 0);
  if (argc == 2) {
    FILE *file = fopen(argv[1], "wb");
    assert(file && fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    assert(fclose(file) == 0);
  }
  FitActivity failure([](void *, const uint8_t *, size_t) { return false; }, nullptr);
  assert(!failure.begin(1, utc, utc, 0, 0));
  bytes.clear();
  FitActivity incomplete(writeVector, &bytes);
  assert(!incomplete.begin(0, utc, utc, 0, 0));
  bytes.clear();
  FitActivity missing(writeVector, &bytes);
  assert(missing.begin(1, utc, utc, 0, 0));
  assert(!missing.finish());
  std::cout << "PASS FIT: CSV validation, message lengths, header/file CRC, timestamps, short write, incomplete records\n";
}
