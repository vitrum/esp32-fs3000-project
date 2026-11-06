#pragma once

#include "imu_comparison.h"
#include "csv_logger.h"
#include "runtime_log.h"
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

class ImuComparisonLogger {
public:
  bool begin() {
    if (queue_) return true;
    exited_ = xSemaphoreCreateBinary();
    if (!exited_) return false;
    queue_ = xQueueCreate(32, sizeof(Command));
    if (!queue_) { vSemaphoreDelete(exited_); exited_ = nullptr; return false; }
    if (xTaskCreate(workerEntry, "imuWriter", 6144, this, 1, nullptr) == pdPASS) return true;
    vQueueDelete(queue_);
    vSemaphoreDelete(exited_);
    exited_ = nullptr;
    queue_ = nullptr;
    return false;
  }

  bool end() {
    if (!queue_) return true;
    requestStop();
    exitRequested_ = true;
    if (xSemaphoreTake(exited_, pdMS_TO_TICKS(3000)) != pdTRUE) return false;
    vQueueDelete(queue_);
    vSemaphoreDelete(exited_);
    queue_ = nullptr;
    exited_ = nullptr;
    exitRequested_ = false;
    return true;
  }

  bool start(uint32_t session) {
    if (!queue_ || busy()) return false;
    Command command;
    command.start = true;
    command.session = session;
    dropped_ = written_ = 0;
    failed_ = false;
    drained_ = false;
    accepting_ = true;
    if (xQueueSend(queue_, &command, 0) == pdTRUE) return true;
    accepting_ = false;
    drained_ = true;
    return false;
  }

  bool log(const ImuComparisonRecord &record) {
    if (!accepting_) return false;
    Command command;
    command.record = record;
    if (xQueueSend(queue_, &command, 0) == pdTRUE) return true;
    ++dropped_;
    return false;
  }

  void requestStop() {
    accepting_ = false;
    if (!drained_) stopping_ = true;
  }
  bool accepting() const { return accepting_.load(); }
  bool busy() const { return !drained_.load(); }
  bool failed() const { return failed_.load(); }
  uint32_t dropped() const { return dropped_.load(); }
  uint32_t written() const { return written_.load(); }

private:
  struct Command {
    bool start = false;
    uint32_t session = 0;
    ImuComparisonRecord record;
  };

  bool writeAll(const char *text, size_t length) {
    size_t offset = 0;
    while (offset < length) {
      const ssize_t count = ::write(fd_, text + offset, length - offset);
      if (count <= 0) return false;
      offset += size_t(count);
    }
    return true;
  }

  bool open(uint32_t session) {
    if (!sdCardReady() || !lockSdCard(pdMS_TO_TICKS(1000))) return false;
    char directory[160];
    snprintf(directory, sizeof(directory), "%s/IMU", sdMountPoint());
    bool success = mkdir(directory, 0775) == 0 || errno == EEXIST;
    char path[200] = {};
    for (unsigned attempt = 0; success && attempt < 8; ++attempt) {
      snprintf(path, sizeof(path), "%s/I%08lX-%u.csv", directory, (unsigned long)session, attempt);
      fd_ = ::open(path, O_WRONLY | O_CREAT | O_EXCL, 0664);
      if (fd_ >= 0 || errno != EEXIST) break;
    }
    success = success && fd_ >= 0;
    if (success) success = writeAll(ImuComparison::header(), strlen(ImuComparison::header()));
    unlockSdCard();
    if (success) AppLog.printf("[IMU] recording %s (g, deg/s; native axes; host timestamps)\n", path);
    return success;
  }

  bool flush() {
    if (fd_ < 0) return false;
    if (!lockSdCard(pdMS_TO_TICKS(1000))) return false;
    const bool success = (!bufferSize_ || writeAll(buffer_, bufferSize_)) && fsync(fd_) == 0;
    unlockSdCard();
    if (success) {
      written_.fetch_add(bufferRows_);
      bufferSize_ = bufferRows_ = 0;
      lastFlushMs_ = millis();
    }
    return success;
  }

  bool close() {
    if (fd_ < 0) return true;
    if (!lockSdCard(pdMS_TO_TICKS(1000))) return false;
    const int result = ::close(fd_);
    fd_ = -1;
    unlockSdCard();
    if (result != 0) failed_ = true;
    return true;
  }

  void fail() {
    if (!failed_.exchange(true)) AppLog.println("[IMU] SD write/open failed; comparison recording stopped");
    accepting_ = false;
    stopping_ = true;
    dropped_.fetch_add(bufferRows_);
    bufferRows_ = bufferSize_ = 0;
  }

  static void workerEntry(void *context) { static_cast<ImuComparisonLogger *>(context)->worker(); }

  void worker() {
    for (;;) {
      Command command;
      const bool received = xQueueReceive(queue_, &command, pdMS_TO_TICKS(20)) == pdTRUE;
      if (received && command.start) {
        bufferSize_ = bufferRows_ = 0;
        lastFlushMs_ = millis();
        if (!open(command.session)) fail();
      } else if (received) {
        if (failed_ || fd_ < 0) ++dropped_;
        else {
          char line[768];
          const size_t length = ImuComparison::format(command.record, line, sizeof(line));
          if (!length) ++dropped_;
          else {
            if (bufferSize_ + length > sizeof(buffer_) && !flush()) fail();
            if (!failed_) {
              memcpy(buffer_ + bufferSize_, line, length);
              bufferSize_ += length;
              ++bufferRows_;
            } else ++dropped_;
          }
        }
      }
      if (!failed_ && bufferSize_ && uint32_t(millis() - lastFlushMs_) >= 1000)
        if (!flush()) fail();
      if (!received && stopping_) {
        if (!failed_ && fd_ >= 0 && !flush()) fail();
        if (close()) {
          stopping_ = false;
          drained_ = true;
          AppLog.printf("[IMU] stopped rows=%lu dropped=%lu failed=%u\n",
                        (unsigned long)written(), (unsigned long)dropped(), unsigned(failed()));
        }
      }
      if (exitRequested_ && drained_) {
        xSemaphoreGive(exited_);
        vTaskDelete(nullptr);
        return;
      }
    }
  }

  QueueHandle_t queue_ = nullptr;
  SemaphoreHandle_t exited_ = nullptr;
  std::atomic<bool> exitRequested_{false};
  std::atomic<bool> accepting_{false}, drained_{true}, stopping_{false}, failed_{false};
  std::atomic<uint32_t> dropped_{0}, written_{0};
  int fd_ = -1;
  char buffer_[8192] = {};
  size_t bufferSize_ = 0, bufferRows_ = 0;
  uint32_t lastFlushMs_ = 0;
};