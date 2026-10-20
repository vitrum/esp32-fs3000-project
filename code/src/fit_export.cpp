#include "fit_export.h"
#include "fit_activity.h"
#include "runtime_log.h"
#include "config.h"
#include <freertos/task.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>

FitExport fitExport;

namespace {
bool sink(void *context, const uint8_t *data, size_t size) {
  return fwrite(data, 1, size, static_cast<FILE *>(context)) == size;
}

struct Scan {
  uint32_t count = 0, first = 0, last = 0;
  uint64_t elapsed = 0;
  double distance = 0;
  float lastSpeed = 0;
  bool add(const FitSample &sample) {
    if (!count) first = sample.milliseconds;
    else {
      const uint32_t delta = sample.milliseconds - last;
      if (!delta || delta > 3600000) return false;
      elapsed += delta;
      if (elapsed > 0xFFFFFFFE) return false;
      distance += (lastSpeed + double(sample.speed)) * 0.5 * delta / 1000;
      if (distance * 100 >= 0xFFFFFFFE) return false;
    }
    last = sample.milliseconds;
    lastSpeed = sample.speed;
    return ++count <= 10000000;
  }
};
}

bool FitExport::start(CsvLogger &logger, bool timeValid) {
  if (busy() || ready_.load()) return false;
  uint32_t utc, milliseconds;

  // 只有在记录期间（main 循环）已写入兜底时才可能走到这里；若仍无参考，
  // 用固定默认时间 + 当前 millis 作为锚点，保证 FIT 一定能生成。
  if (!logger.utcReference(utc, milliseconds) && FIT_DEFAULT_UTC != 0) {
    logger.useFallbackUtcReference(FIT_DEFAULT_UTC, millis());
    AppLog.printf("[FIT] no GPS time; using default UTC %lu for this export\n",
                  (unsigned long)FIT_DEFAULT_UTC);
  }

  // 错误提示要能区分「完全没时间」与「有 GPS 但记录期间没授时」。
  const char *error = nullptr;
  if (!logger.utcReference(utc, milliseconds)) {
    // 走到这里说明既没有 GPS 参考，也没有可用的默认时间（FIT_DEFAULT_UTC==0）。
    error = timeValid ? "No trusted time for this recording."
                      : "Acquire GPS time before saving FIT.";
  } else if (!sdCardReady()) {
    error = "SD card is not ready.";
  } else if (!logger.sampleCount()) {
    error = "No recorded samples. Start recording first.";
  } else if (!logger.sessionComplete()) {
    error = "Too many daily CSV segments for FIT export.";
  }
  if (error) {
    snprintf(result_, sizeof(result_), "%s", error);
    success_ = false;
    ready_.store(true);
    AppLog.printf("[FIT] %s\n", error);
    return false;
  }
  logger_ = &logger;
  busy_.store(true);
  if (xTaskCreate(task, "fitExport", 6144, this, 1, nullptr) != pdPASS) {
    busy_.store(false);
    snprintf(result_, sizeof(result_), "Could not create FIT export task.");
    success_ = false;
    ready_.store(true);
    AppLog.printf("[FIT] %s\n", result_);
    return false;
  }
  return true;
}

bool FitExport::takeResult(bool &success, char *text, size_t capacity) {
  if (busy() || !ready_.exchange(false)) return false;
  success = success_;
  snprintf(text, capacity, "%s", result_);
  return true;
}

void FitExport::task(void *argument) {
  auto &self = *static_cast<FitExport *>(argument);
  self.success_ = false;
  self.result_[0] = '\0';
  if (!self.logger_->stop()) {
    snprintf(self.result_, sizeof(self.result_), "Could not finish CSV. Retry; data retained.");
  } else {
    self.success_ = self.convert();
    if (self.success_)
      snprintf(self.result_, sizeof(self.result_), "Saved %s", self.output_);
    else if (!self.result_[0])
      snprintf(self.result_, sizeof(self.result_), "FIT export failed. CSV retained; retry saving.");
  }
  AppLog.printf("[FIT] %s\n", self.result_);
  self.ready_.store(true);
  self.busy_.store(false);
  vTaskDelete(nullptr);
}

bool FitExport::convert() {
  uint32_t anchorUtc, anchorMs;
  if (!logger_->utcReference(anchorUtc, anchorMs)) return false;
  Scan summary;
  FILE *output = nullptr;
  char finalPath[160] = {}, temporary[168] = {};
  bool ok = true;
  uint32_t startUtc = 0;
  // Pass 1 validates every CSV row and computes the exact FIT header and summaries.
  for (unsigned pass = 0; pass < 2 && ok; ++pass) {
    Scan scan;
    FitActivity encoder(sink, output);
    if (pass) {
      const int64_t first = int64_t(anchorUtc) +
          int32_t(summary.first - anchorMs) / 1000;
      if (first < 1577836800 || first + summary.elapsed / 1000 > UINT32_MAX) {
        ok = false; break;
      }
      startUtc = uint32_t(first);
      snprintf(output_, sizeof(output_), "%s", logger_->sessionFile(0));
      char *extension = strrchr(output_, '.');
      if (!extension) { ok = false; break; }
      snprintf(extension, size_t(output_ + sizeof(output_) - extension), ".fit");
      snprintf(finalPath, sizeof(finalPath), "%s%s", sdMountPoint(), output_);
      snprintf(temporary, sizeof(temporary), "%s.tmp", finalPath);
      if (!lockSdCard()) { ok = false; break; }
      struct stat info;
      const bool exists = stat(finalPath, &info) == 0;
      if (exists)
        snprintf(result_, sizeof(result_), "FIT already exists: %s", output_);
      if (!exists) output = fopen(temporary, "wb");
      unlockSdCard();
      if (!output) { ok = false; break; }
      encoder = FitActivity(sink, output);
      if (!lockSdCard()) { ok = false; break; }
      ok = encoder.begin(summary.count, startUtc, startUtc + summary.elapsed / 1000,
                         uint32_t(summary.elapsed), uint32_t(std::llround(summary.distance * 100)));
      unlockSdCard();
    }
    for (size_t file = 0; file < logger_->sessionFileCount() && ok; ++file) {
      char path[160];
      snprintf(path, sizeof(path), "%s%s", sdMountPoint(), logger_->sessionFile(file));
      if (!lockSdCard()) { ok = false; break; }
      FILE *input = fopen(path, "r");
      char line[192];
      static const char header[] = "t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc\n";
      ok = input && fgets(line, sizeof(line), input) && !strcmp(line, header);
      unlockSdCard();
      if (!input) {
        AppLog.printf("[FIT] could not open input %s\n", path);
        break;
      }
      while (ok) {
        if (!lockSdCard()) { ok = false; break; }
        const bool read = fgets(line, sizeof(line), input) != nullptr;
        if (!read) ok = !ferror(input);
        if (read) {
          FitSample sample;
          ok = strchr(line, '\n') && parseFitCsv(line, sample) && scan.add(sample);
          if (!ok) AppLog.printf("[FIT] invalid sample after row %lu in %s\n",
                                 (unsigned long)scan.count, logger_->sessionFile(file));
          if (ok && pass)
            ok = encoder.record(startUtc + uint32_t(scan.elapsed / 1000), sample,
                                uint32_t(std::llround(scan.distance * 100)));
        }
        unlockSdCard();
        if (!read) break;
        if (scan.count % 16 == 0) vTaskDelay(pdMS_TO_TICKS(1));
      }
      // Cleanup must not race audio/filesystem operations even on error.
      while (!lockSdCard()) vTaskDelay(pdMS_TO_TICKS(1));
      if (fclose(input) != 0) ok = false;
      unlockSdCard();
    }
    if (!pass) { summary = scan; ok = ok && scan.count != 0; }
    else if (ok) {
      if (!lockSdCard()) { ok = false; break; }
      ok = scan.count == summary.count && scan.elapsed == summary.elapsed && encoder.finish();
      unlockSdCard();
    }
  }
  if (output) {
    while (!lockSdCard()) vTaskDelay(pdMS_TO_TICKS(1));
    if (fflush(output) != 0 || fsync(fileno(output)) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    if (ok && rename(temporary, finalPath) != 0) ok = false;
    if (!ok && unlink(temporary) != 0)
      AppLog.println("[FIT] could not remove incomplete temporary file");
    unlockSdCard();
  }
  if (!ok) AppLog.println("[FIT] validation or SD I/O failed; no completed FIT published");
  return ok;
}
