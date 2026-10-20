#include "fit_activity.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
uint16_t crcByte(uint16_t crc, uint8_t byte) {
  crc ^= byte;
  for (unsigned bit = 0; bit < 8; ++bit)
    crc = crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1;
  return crc;
}
uint32_t scaled(float value, double scale, uint32_t invalid) {
  const double result = double(value) * scale;
  return std::isfinite(result) && result >= 0 && result < double(invalid) - 0.5
      ? uint32_t(std::llround(result)) : invalid;
}
}

bool parseFitCsv(const char *line, FitSample &s) {
  unsigned milliseconds;
  int consumed = 0;
  if (sscanf(line, "%u,%f,%f,%f,%f,%f,%f,%f,%u%n", &milliseconds,
             &s.power, &s.cadence, &s.speed, &s.wind, &s.yaw, &s.posture,
             &s.density, &s.source, &consumed) != 9) return false;
  for (const char *tail = line + consumed; *tail; ++tail)
    if (*tail != '\r' && *tail != '\n') return false;
  if (!std::isfinite(s.power) || !std::isfinite(s.cadence) ||
      !std::isfinite(s.speed) || !std::isfinite(s.wind) ||
      !std::isfinite(s.yaw) || !std::isfinite(s.posture) ||
      !std::isfinite(s.density) || s.speed < 0 || s.source > 3 || s.source == 2)
    return false;
  s.milliseconds = milliseconds;
  return true;
}

bool FitActivity::bytes(const uint8_t *data, size_t length) {
  if (!ok_) return false;
  if (!sink_(context_, data, length)) return ok_ = false;
  for (size_t i = 0; i < length; ++i) crc_ = crcByte(crc_, data[i]);
  return true;
}

bool FitActivity::number(uint32_t value, size_t size) {
  uint8_t data[4];
  for (size_t i = 0; i < size; ++i) data[i] = uint8_t(value >> (i * 8));
  return bytes(data, size);
}

bool FitActivity::definition(uint8_t local, uint16_t global, const Field *fields,
                             size_t count, bool developer) {
  number(0x40 | local | (developer ? 0x20 : 0), 1);
  number(0, 1);
  number(0, 1);
  number(global, 2);
  number(count, 1);
  for (size_t i = 0; i < count; ++i) {
    number(fields[i].number, 1); number(fields[i].size, 1); number(fields[i].type, 1);
  }
  if (developer) {
    number(5, 1);
    for (unsigned i = 0; i < 5; ++i) {
      number(i, 1); number(i == 4 ? 1 : 4, 1); number(0, 1);
    }
  }
  return ok_;
}

bool FitActivity::begin(uint32_t count, uint32_t startUtc, uint32_t endUtc,
                        uint32_t elapsedMs, uint32_t distanceCm) {
  if (!count || count > 10000000 || startUtc < kEpoch || endUtc < startUtc)
    return ok_ = false;
  remaining_ = count; start_ = startUtc - kEpoch; end_ = endUtc - kEpoch;
  elapsed_ = elapsedMs; distance_ = distanceCm;
  // Definitions and fixed messages: 480 bytes; each record: 31 bytes.
  const uint32_t dataSize = 480 + count * 31;
  number(14, 1); number(0x20, 1); number(2100, 2); number(dataSize, 4);
  static const uint8_t signature[] = {'.', 'F', 'I', 'T'};
  bytes(signature, 4);
  const uint16_t headerCrc = crc_;
  number(headerCrc, 2);

  const Field file[] = {{0,1,0}, {1,2,0x84}, {4,4,0x86}};
  definition(0, 0, file, 3);
  number(0,1); number(4,1); number(255,2); number(start_,4);
  const Field developer[] = {{1,16,0x0D}, {3,1,2}};
  definition(1, 207, developer, 2);
  number(1,1);
  static const uint8_t appId[16] = {'A','e','r','o','P','r','o','b','e','F','I','T',0,0,0,1};
  bytes(appId, 16); number(0,1);
  const Field description[] = {{0,1,2}, {1,1,2}, {2,1,2}, {3,24,7}, {8,8,7}};
  definition(2, 206, description, 5);
  const char *names[] = {"airspeed", "yaw", "posture", "air_density", "power_source"};
  const char *units[] = {"m/s", "deg", "deg", "kg/m3", ""};
  for (unsigned i = 0; i < 5; ++i) {
    number(2,1); number(0,1); number(i,1); number(i == 4 ? 2 : 0x88,1);
    char name[24] = {}, unit[8] = {};
    snprintf(name, sizeof(name), "%s", names[i]);
    snprintf(unit, sizeof(unit), "%s", units[i]);
    bytes(reinterpret_cast<uint8_t *>(name), sizeof(name));
    bytes(reinterpret_cast<uint8_t *>(unit), sizeof(unit));
  }
  const Field events[] = {{253,4,0x86}, {0,1,0}, {1,1,0}};
  definition(3,21,events,3);
  event(start_,0);
  const Field records[] = {{253,4,0x86}, {7,2,0x84}, {4,1,2}, {6,2,0x84}, {5,4,0x86}};
  return definition(4,20,records,5,true);
}

bool FitActivity::record(uint32_t utc, const FitSample &s, uint32_t distanceCm) {
  if (!remaining_ || utc < start_ + kEpoch || utc > end_ + kEpoch) return ok_ = false;
  --remaining_;
  number(4,1); number(utc-kEpoch,4);
  number(s.source ? scaled(s.power,1,0xFFFF) : 0xFFFF,2);
  number(s.source ? scaled(s.cadence,1,0xFF) : 0xFF,1);
  number(scaled(s.speed,1000,0xFFFF),2); number(distanceCm,4);
  for (float value : {s.wind, s.yaw, s.posture, s.density}) {
    uint32_t bits;
    static_assert(sizeof(value) == sizeof(bits), "FIT requires float32");
    memcpy(&bits, &value, sizeof(bits));
    number(bits,4);
  }
  return number(s.source,1);
}

bool FitActivity::event(uint32_t utc, uint8_t type) {
  number(3,1); number(utc,4); number(0,1);
  return number(type,1);
}

bool FitActivity::summary(uint8_t local, bool lap) {
  const Field fields[] = {{253,4,0x86}, {0,1,0}, {1,1,0}, {2,4,0x86},
                         {7,4,0x86}, {8,4,0x86}, {9,4,0x86}};
  Field extended[11];
  memcpy(extended, fields, sizeof(fields));
  extended[7] = {uint8_t(lap ? 25 : 5),1,0};
  extended[8] = {6,1,0};
  extended[9] = {25,2,0x84};
  extended[10] = {26,2,0x84};
  definition(local,lap ? 19 : 18,extended,lap ? 8 : 11);
  number(local,1); number(end_,4); number(lap ? 9 : 8,1); number(1,1);
  number(start_,4); number(elapsed_,4); number(elapsed_,4); number(distance_,4); number(2,1);
  if (!lap) { number(0,1); number(0,2); number(1,2); }
  return ok_;
}

bool FitActivity::finish() {
  if (remaining_) return ok_ = false;
  event(end_,4);
  summary(5,true);
  summary(6,false);
  const Field activity[] = {{253,4,0x86}, {0,4,0x86}, {1,2,0x84}, {2,1,0}, {3,1,0}, {4,1,0}};
  definition(7,34,activity,6);
  number(7,1); number(end_,4); number(elapsed_,4); number(1,2);
  number(0,1); number(26,1); number(1,1);
  const uint16_t fileCrc = crc_;
  return number(fileCrc,2);
}
