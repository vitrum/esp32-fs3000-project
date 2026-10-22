#pragma once
#ifdef HOST_POSIX_SD
#include "posix_fs.h"
#else
#include "Arduino.h"
#include <algorithm>

constexpr int FILE_READ = 0;
extern bool sdLocked, sdBusy, sdOpenFails, sdSeekFails, sdShortRead;
extern std::string sdContents;
extern TickType_t lastSdTimeout;
class File {
public:
  explicit File(bool valid = false) : _valid(valid) {}
  explicit operator bool() const { return _valid; }
  bool isDirectory() const { return false; }
  size_t size() const { return sdContents.size(); }
  bool seek(size_t offset) {
    _offset = offset;
    return !sdSeekFails && offset <= sdContents.size();
  }
  size_t read(uint8_t *buffer, size_t length) {
    const size_t count = std::min(length, sdContents.size() - _offset);
    const size_t actual = sdShortRead && count ? count - 1 : count;
    memcpy(buffer, sdContents.data() + _offset, actual);
    _offset += actual;
    return actual;
  }
  void close() { _valid = false; }
private:
  bool _valid;
  size_t _offset = 0;
};
namespace fs {
class FS {
public:
  File open(const String &, int) { return File(!sdOpenFails); }
};
}
#endif
