#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <climits>

class UsbSdExport {
public:
  enum class Mode : uint8_t { Probe, Ride, Export, Ejected };
  enum class ReadError : uint8_t { None, NotReady, InvalidRequest, OutOfRange, Media };
  using ReadSector = bool (*)(void *context, uint32_t sector, void *buffer);

  void selectAtBoot(bool hostPresent) {
    Mode expected = Mode::Probe;
    mode_.compare_exchange_strong(expected, hostPresent ? Mode::Export : Mode::Ride);
  }
  Mode mode() const { return mode_.load(); }
  bool exclusive() const { return mode() == Mode::Export || mode() == Mode::Ejected; }
  bool attach(uint64_t sectors, uint32_t sectorSize, ReadSector reader, void *context) {
    if (mode() != Mode::Export || !sectors || sectors > UINT32_MAX || sectorSize != 512 || !reader)
      return false;
    sectors_ = static_cast<uint32_t>(sectors);
    reader_ = reader;
    context_ = context;
    ready_.store(true);
    return true;
  }
  bool ready() const { return mode() == Mode::Export && ready_.load(); }
  uint32_t sectors() const { return sectors_; }
  ReadError readError() const { return readError_.load(); }
  uint8_t senseKey() const {
    switch (readError()) {
      case ReadError::NotReady: return 0x02;
      case ReadError::InvalidRequest:
      case ReadError::OutOfRange: return 0x05;
      case ReadError::Media: return 0x03;
      default: return 0;
    }
  }
  uint8_t senseCode() const {
    switch (readError()) {
      case ReadError::NotReady: return 0x3a;
      case ReadError::InvalidRequest: return 0x24;
      case ReadError::OutOfRange: return 0x21;
      case ReadError::Media: return 0x11;
      default: return 0;
    }
  }
  void mediaFailed() { ready_.store(false); }
  void eject() {
    if (mode() == Mode::Export) mode_.store(Mode::Ejected);
  }
  int32_t write(uint32_t, uint32_t, const void *, uint32_t) const { return -1; }
  int32_t read(uint32_t sector, uint32_t offset, void *buffer, uint32_t size) {
    readError_.store(ReadError::None);
    if (!ready()) return failRead(ReadError::NotReady);
    if ((!buffer && size) || size > INT32_MAX) return failRead(ReadError::InvalidRequest);
    uint64_t position = static_cast<uint64_t>(sector) * 512 + offset;
    const uint64_t capacity = static_cast<uint64_t>(sectors_) * 512;
    if (position > capacity || size > capacity - position) return failRead(ReadError::OutOfRange);
    auto *output = static_cast<uint8_t *>(buffer);
    uint32_t remaining = size;
    alignas(4) uint8_t scratch[512];
    while (remaining) {
      const uint32_t currentSector = static_cast<uint32_t>(position / 512);
      const uint32_t within = position % 512;
      const uint32_t chunk = remaining < 512 - within ? remaining : 512 - within;
      if (!ready()) return failRead(ReadError::NotReady);
      if (!reader_(context_, currentSector, scratch)) return failRead(ReadError::Media);
      memcpy(output, scratch + within, chunk);
      output += chunk;
      position += chunk;
      remaining -= chunk;
    }
    return static_cast<int32_t>(size);
  }

private:
  int32_t failRead(ReadError error) { readError_.store(error); return -1; }
  std::atomic<ReadError> readError_{ReadError::None};
  std::atomic<Mode> mode_{Mode::Probe};
  std::atomic<bool> ready_{false};
  uint32_t sectors_ = 0;
  ReadSector reader_ = nullptr;
  void *context_ = nullptr;
};