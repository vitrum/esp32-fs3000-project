#pragma once

#include "esp_err.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

constexpr int ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_ANY = 255;
struct esp_partition_t { size_t size = 8 * 1024 * 1024; };
inline esp_partition_t hostJournalPartition;
inline std::vector<uint8_t> hostJournalFlash(8 * 1024 * 1024, 0xff);
inline int hostFlashWriteBudget = -1;
inline bool hostFlashReadFailure = false, hostFlashEraseFailure = false, hostJournalPresent = true;
inline const esp_partition_t *esp_partition_find_first(int, int, const char *) {
  return hostJournalPresent ? &hostJournalPartition : nullptr;
}
inline int esp_partition_read(const esp_partition_t *partition, size_t offset, void *out, size_t size) {
  if (hostFlashReadFailure || offset + size > partition->size) return -1;
  memcpy(out, hostJournalFlash.data() + offset, size);
  return ESP_OK;
}
inline int esp_partition_write(const esp_partition_t *partition, size_t offset, const void *data, size_t size) {
  if (offset + size > partition->size) return -1;
  const size_t written = hostFlashWriteBudget < 0 ? size : std::min(size, size_t(hostFlashWriteBudget));
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0; index < written; ++index) {
    if ((hostJournalFlash[offset + index] & bytes[index]) != bytes[index]) return -1;
    hostJournalFlash[offset + index] &= bytes[index];
  }
  if (hostFlashWriteBudget >= 0) hostFlashWriteBudget -= written;
  return written == size ? ESP_OK : -1;
}
inline int esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size) {
  if (hostFlashEraseFailure || offset % 4096 || size % 4096 || offset + size > partition->size) return -1;
  std::fill(hostJournalFlash.begin() + offset, hostJournalFlash.begin() + offset + size, 0xff);
  return ESP_OK;
}