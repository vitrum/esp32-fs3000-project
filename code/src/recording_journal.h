#pragma once

#include "fit_activity.h"
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <freertos/task.h>
#include <cstring>

class RecordingJournal {
public:
  enum class TimeSource : uint32_t { Default, Network, Gps };
  struct Reference {
    Reference(uint32_t utcValue = 0, uint32_t millisecondsValue = 0,
              TimeSource sourceValue = TimeSource::Default)
        : utc(utcValue), milliseconds(millisecondsValue), source(sourceValue) {}
    uint32_t utc;
    uint32_t milliseconds;
    TimeSource source;
  };

  bool begin() {
    partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_ANY, "ridecache");
    if (!partition_ || partition_->size < 8192) return false;
    active_ = false;
    poisoned_ = false;
    records_ = samples_ = 0;
    reference_ = Reference{};
    Record first;
    if (!read(0, first)) return false;
    if (erased(first)) return true;
    if (first.commit == 0xffffffffu) return true;
    if (validBody(first) && first.kind == kStart && first.index == 0 &&
      first.commit != kCommit && (first.commit & kCommit) == kCommit) return true;
    if (!validBody(first) || first.kind != kStart || first.index != 0 ||
        (first.commit | kCommit) != kCommit)
      return false;
    session_ = first.session;
    if (first.commit == 0) return true;
    active_ = true;
    records_ = 1;
    for (uint32_t index = 1; index < partition_->size / sizeof(Record); ++index) {
      Record record;
      if (!read(index, record)) return false;
      if (erased(record)) break;
      if (record.session != session_) {
        if (record.commit == kCommit && !validBody(record)) return false;
        break;
      }
      if (!valid(record, index)) {
        if (record.commit == kCommit) return false;
        poisoned_ = true;
        break;
      }
      if (record.kind == kReference) memcpy(&reference_, record.payload, sizeof(reference_));
      else if (record.kind == kSample) ++samples_;
      else { poisoned_ = true; break; }
      ++records_;
      if (index % 128 == 0) vTaskDelay(1);
    }
    return true;
  }

  bool start(uint32_t session) {
    if (!partition_ || active_ || session == 0) return false;
    session_ = session;
    records_ = samples_ = 0;
    reference_ = Reference{};
    poisoned_ = false;
    if (!append(kStart, nullptr, 0)) return false;
    active_ = true;
    return true;
  }

  bool setReference(const Reference &reference) {
    if (!active_ || reference.utc < 1577836800) return false;
    if (reference.utc == reference_.utc && reference.milliseconds == reference_.milliseconds &&
        reference.source == reference_.source) return true;
    if (!append(kReference, &reference, sizeof(reference))) return false;
    reference_ = reference;
    return true;
  }

  bool log(const FitSample &sample) {
    if (!active_ || reference_.utc == 0) return false;
    if (!append(kSample, &sample, sizeof(sample))) return false;
    ++samples_;
    return true;
  }

  bool sample(uint32_t index, FitSample &sample, bool &isSample) const {
    isSample = false;
    if (!active_ || index == 0 || index >= records_) return false;
    Record record;
    if (!read(index, record) || !valid(record, index)) return false;
    isSample = record.kind == kSample;
    if (isSample) memcpy(&sample, record.payload, sizeof(sample));
    return true;
  }

  bool finish() {
    if (!active_) return true;
    const uint32_t done = 0;
    if (esp_partition_write(partition_, offsetof(Record, commit), &done, sizeof(done)) != ESP_OK)
      return false;
    active_ = false;
    return true;
  }

  bool pending() const { return active_; }
  bool damagedTail() const { return poisoned_; }
  uint32_t records() const { return records_; }
  uint32_t samples() const { return samples_; }
  uint32_t session() const { return session_; }
  Reference reference() const { return reference_; }

private:
  static constexpr uint32_t kMagic = 0x41524a31;
  static constexpr uint32_t kCommit = 0xa55a5aa5;
  static constexpr uint32_t kStart = 1, kReference = 2, kSample = 3;
  struct Record {
    uint32_t magic, session, index, kind;
    uint8_t payload[40];
    uint32_t crc, commit;
  };
  static_assert(sizeof(Record) == 64, "Journal records must be sector aligned");
  static_assert(sizeof(FitSample) <= sizeof(Record::payload), "Sample must fit journal record");
  static_assert(sizeof(Reference) <= sizeof(Record::payload), "Reference must fit journal record");

  static bool erased(const Record &record) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&record);
    for (size_t index = 0; index < sizeof(record); ++index)
      if (bytes[index] != 0xff) return false;
    return true;
  }

  static bool validBody(const Record &record) {
    return record.magic == kMagic && record.crc == esp_rom_crc32_le(
        0, reinterpret_cast<const uint8_t *>(&record), offsetof(Record, crc));
  }

  bool valid(const Record &record, uint32_t index) const {
    return validBody(record) && record.commit == kCommit && record.session == session_ &&
           record.index == index;
  }

  bool read(uint32_t index, Record &record) const {
    return partition_ && (uint64_t(index) + 1) * sizeof(record) <= partition_->size &&
           esp_partition_read(partition_, index * sizeof(record), &record, sizeof(record)) == ESP_OK;
  }

  bool append(uint32_t kind, const void *payload, size_t size) {
    if (!partition_ || poisoned_ || size > sizeof(Record::payload) ||
        (uint64_t(records_) + 1) * sizeof(Record) > partition_->size) return false;
    const size_t offset = records_ * sizeof(Record);
    if (offset % 4096 == 0 && esp_partition_erase_range(partition_, offset, 4096) != ESP_OK) {
      poisoned_ = true;
      return false;
    }
    Record record{};
    record.magic = kMagic;
    record.session = session_;
    record.index = records_;
    record.kind = kind;
    if (size) memcpy(record.payload, payload, size);
    record.crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&record), offsetof(Record, crc));
    record.commit = kCommit;
    if (esp_partition_write(partition_, offset, &record, offsetof(Record, commit)) != ESP_OK ||
        esp_partition_write(partition_, offset + offsetof(Record, commit),
                            &record.commit, sizeof(record.commit)) != ESP_OK) {
      poisoned_ = true;
      return false;
    }
    ++records_;
    return true;
  }

  const esp_partition_t *partition_ = nullptr;
  uint32_t session_ = 0, records_ = 0, samples_ = 0;
  Reference reference_;
  bool active_ = false, poisoned_ = false;
};