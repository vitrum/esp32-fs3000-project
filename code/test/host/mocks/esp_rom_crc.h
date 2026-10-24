#pragma once

#include <cstdint>

inline uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t *data, uint32_t size) {
  uint32_t crc = ~seed;
  for (uint32_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  return ~crc;
}