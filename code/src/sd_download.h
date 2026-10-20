#pragma once

#include <ESPAsyncWebServer.h>
#include <WebResponseImpl.h>
#include <atomic>
#include "csv_logger.h"
#include "runtime_log.h"

class SdDownloadResponse : public AsyncAbstractResponse {
public:
  static bool tryAcquire() { return !active().exchange(true); }
  static void releaseSlot() { active().store(false); }

  SdDownloadResponse(const String &path, size_t size) : _path(path) {
    _code = 200;
    _contentType = "application/octet-stream";
    _contentLength = size;
    _sendContentLength = true;
    _chunked = false;
    _lastProgressMs = millis();
  }

  ~SdDownloadResponse() override { releaseSlot(); }
  bool _sourceValid() const override { return !_failed; }

  size_t _fillBuffer(uint8_t *buffer, size_t maxLen) override {
    if (_offset >= _contentLength || maxLen == 0) return 0;
    const uint32_t now = millis();
    if (_offset > 0 && now - _lastProgressMs < kChunkIntervalMs)
      return RESPONSE_TRY_AGAIN;
    if (!lockSdCard(0)) {
      if (!_waitingForSd) {
        _waitingForSd = true;
        _waitStartedMs = now;
      } else if (now - _waitStartedMs >= kStallTimeoutMs) {
        fail("SD lock timeout");
      }
      return RESPONSE_TRY_AGAIN;
    }
    _waitingForSd = false;
    File file = sdFs().open(_path, FILE_READ);
    if (!file || file.isDirectory() || !file.seek(_offset)) {
      if (file) file.close();
      unlockSdCard();
      fail("file open/seek failed");
      return RESPONSE_TRY_AGAIN;
    }
    size_t wanted = maxLen < kChunkBytes ? maxLen : kChunkBytes;
    if (wanted > _contentLength - _offset) wanted = _contentLength - _offset;
    const size_t count = file.read(buffer, wanted);
    file.close();
    unlockSdCard();
    if (count != wanted) {
      fail("short read");
      return RESPONSE_TRY_AGAIN;
    }
    _offset += count;
    _lastProgressMs = millis();
    return count;
  }

private:
  static constexpr size_t kChunkBytes = 1024;
  static constexpr uint32_t kChunkIntervalMs = 20;
  static constexpr uint32_t kStallTimeoutMs = 5000;
  static std::atomic<bool> &active() {
    static std::atomic<bool> value{false};
    return value;
  }
  void fail(const char *reason) {
    _failed = true;
    AppLog.printf("[WEB] download failed: %s at %u/%u: %s\n",
                  _path.c_str(), (unsigned)_offset,
                  (unsigned)_contentLength, reason);
  }
  String _path;
  size_t _offset = 0;
  uint32_t _lastProgressMs = 0;
  uint32_t _waitStartedMs = 0;
  bool _waitingForSd = false;
  bool _failed = false;
};
