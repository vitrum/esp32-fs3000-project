#pragma once

#include "config.h"
#include "csv_logger.h"
#include "fit_export.h"
#include "recording_journal.h"
#include <atomic>
#include <cmath>
#include <cstdio>

class RideRecorder {
public:
  RideRecorder(CsvLogger &logger, FitExport &exporter, RecordingJournal &journal)
      : logger_(logger), exporter_(exporter), journal_(journal) {}

  bool begin() {
    ready_ = journal_.begin();
    if (!ready_) { phase_ = Phase::Error; note("Flash recovery cache unavailable. Recording disabled."); }
    else if (journal_.pending()) { phase_ = Phase::Restoring; note("Recovering recording from Flash..."); }
    return ready_;
  }

  bool requestStart() {
    if (phase_ == Phase::Waiting || phase_ == Phase::Recording) return true;
    if (!ready_ || shutdown_ || busy() || journal_.pending()) {
      note("Finish saving/recovery before starting another recording.");
      return false;
    }
    duration_ = skipped_ = 0;
    phase_ = Phase::Waiting;
    note("Waiting for fresh power and valid wind...");
    return true;
  }

  void requestStop(uint32_t now) {
    if (phase_ == Phase::Waiting) { phase_ = Phase::Idle; return; }
    if (phase_ != Phase::Recording) return;
    duration_ = now - startedMs_;
    phase_ = logger_.droppedCount() ? Phase::Restoring : Phase::Saving;
    exportRequested_ = phase_ == Phase::Saving;
  }

  bool requestSave(uint32_t now) {
    if (phase_ == Phase::Recording) { requestStop(now); return true; }
    if (busy()) return true;
    if (!ready_) { note("Flash cache unavailable/corrupt. Recovery blocked; data retained."); return false; }
    if (!journal_.pending()) { note("No unsaved recording."); return false; }
    phase_ = Phase::Restoring;
    restoreStarted_ = false;
    return true;
  }

  void requestShutdown(uint32_t now) {
    shutdown_ = true;
    if (phase_ == Phase::Error && !journal_.pending() && !logger_.ok()) { phase_ = Phase::Idle; return; }
    requestStop(now);
    if (phase_ == Phase::Error && journal_.pending()) requestSave(now);
  }

  bool accept(const FitSample &sample, bool powerValid, bool windValid, uint32_t session) {
    if (phase_ != Phase::Waiting && phase_ != Phase::Recording) return false;
    const bool valid = powerValid && windValid && sample.source != 0 &&
        std::isfinite(sample.power) && std::isfinite(sample.cadence) &&
        std::isfinite(sample.speed) && std::isfinite(sample.wind) &&
        std::isfinite(sample.yaw) && std::isfinite(sample.posture) &&
        std::isfinite(sample.density) && sample.wind >= 0 && sample.speed >= 0 &&
        sample.density > 0;
    if (!valid) { if (recording()) ++skipped_; return false; }
    if (phase_ == Phase::Waiting) {
      if (!logger_.start()) { phase_ = Phase::Idle; note("Could not start recording; check SD card."); return false; }
      if (!journal_.start(session)) {
        logger_.stop(); phase_ = Phase::Error;
        note("Could not start Flash recovery cache. CSV was not started.");
        return false;
      }
      startedMs_ = sample.milliseconds;
      phase_ = Phase::Recording;
    }
    uint32_t utc = 0, milliseconds = 0;
    if (!logger_.utcReference(utc, milliseconds) && FIT_DEFAULT_UTC)
      logger_.useFallbackUtcReference(FIT_DEFAULT_UTC, sample.milliseconds);
    logger_.utcReference(utc, milliseconds);
    const auto source = logger_.utcReferenceFromGps() ? RecordingJournal::TimeSource::Gps :
        logger_.utcReferenceFromNetwork() ? RecordingJournal::TimeSource::Network :
        RecordingJournal::TimeSource::Default;
    if (!journal_.setReference({utc, milliseconds, source}) || !journal_.log(sample)) {
      requestStop(sample.milliseconds);
      note("Flash cache full or write failed. Saving committed samples.");
      return false;
    }
    logger_.log(sample.milliseconds, sample.power, sample.cadence, sample.speed,
                sample.wind, sample.yaw, sample.posture, sample.density, sample.source);
    return true;
  }

  void poll(uint32_t now) {
    if (phase_ == Phase::Restoring) {
      if (!restoreStarted_) {
        if (journal_.samples() == 0) { phase_ = Phase::Saving; exportRequested_ = true; }
        else {
          restoreDone_ = false;
          restoreStarted_ = true;
          if (xTaskCreate(restoreTask, "rideRecovery", 6144, this, 1, nullptr) != pdPASS) {
            restoreStarted_ = false; phase_ = Phase::Error;
            note("Recovery task unavailable; Flash recording retained.");
          }
        }
      } else if (restoreDone_.exchange(false)) {
        restoreStarted_ = false;
        if (!restoreSuccess_) { phase_ = Phase::Error; note("SD recovery failed. Flash retained; retry SAVE FIT."); }
        else { restored_ = true; phase_ = Phase::Saving; exportRequested_ = true; }
      }
    }
    if (phase_ != Phase::Saving) return;
    if (journal_.samples() == 0) {
      if (logger_.stop() && journal_.finish()) phase_ = Phase::Idle;
      return;
    }
    if (exportRequested_) {
      exportRequested_ = false;
      exporter_.start(logger_, true);
    }
    bool success;
    char result[160];
    if (exporter_.takeResult(success, result, sizeof(result))) {
      if (!success) { phase_ = Phase::Error; note(result); return; }
      published_ = true;
      snprintf(savedMessage_, sizeof(savedMessage_), "%s", result);
      lastAckMs_ = now - 1000;
    }
    if (published_ && now - lastAckMs_ >= 1000) {
      lastAckMs_ = now;
      if (!journal_.finish()) { note("FIT saved; Flash acknowledgement failed. Power remains on."); return; }
      published_ = false;
      phase_ = Phase::Idle;
      logger_.resetUtcReference();
      if (restored_) { logger_.setLocalDate(""); restored_ = false; }
      note(savedMessage_);
    }
  }

  bool waiting() const { return phase_ == Phase::Waiting; }
  bool recording() const { return phase_ == Phase::Recording; }
  bool restoring() const { return phase_ == Phase::Restoring; }
  bool busy() const { return restoring() || phase_ == Phase::Saving; }
  bool readyToPowerOff() const { return shutdown_ && phase_ == Phase::Idle && !journal_.pending(); }
  uint32_t duration(uint32_t now) const { return recording() ? now - startedMs_ : restoring() ? 0 : duration_; }
  uint32_t skipped() const { return skipped_; }
  bool takeMessage(char *out, size_t size) {
    if (!messageReady_) return false;
    snprintf(out, size, "%s", message_);
    messageReady_ = false;
    return true;
  }

private:
  enum class Phase { Idle, Waiting, Recording, Restoring, Saving, Error };

  void note(const char *text) {
    snprintf(message_, sizeof(message_), "%s", text);
    messageReady_ = true;
  }

  bool restore() {
    if (!logger_.stop() || !logger_.resetUtcReference()) return false;
    char recoveredDate[12];
    snprintf(recoveredDate, sizeof(recoveredDate), "R%08lX", (unsigned long)journal_.session());
    logger_.setLocalDate(recoveredDate);
    if (!logger_.start()) return false;
    const auto reference = journal_.reference();
    if (reference.source == RecordingJournal::TimeSource::Gps)
      logger_.setUtcReference(reference.utc, reference.milliseconds);
    else if (reference.source == RecordingJournal::TimeSource::Network)
      logger_.setNetworkUtcReference(reference.utc, reference.milliseconds);
    else logger_.useFallbackUtcReference(reference.utc, reference.milliseconds);
    uint32_t count = 0, first = 0, last = 0;
    for (uint32_t index = 1; index < journal_.records(); ++index) {
      FitSample sample;
      bool isSample;
      if (!journal_.sample(index, sample, isSample)) { logger_.stop(); return false; }
      if (!isSample) continue;
      if (!count) first = sample.milliseconds;
      last = sample.milliseconds;
      logger_.log(sample.milliseconds, sample.power, sample.cadence, sample.speed,
                  sample.wind, sample.yaw, sample.posture, sample.density, sample.source);
      if (++count % 60 == 0) {
        if (!logger_.sync()) { logger_.stop(); return false; }
        vTaskDelay(1);
      }
    }
    duration_ = last - first;
    return count == journal_.samples() && logger_.droppedCount() == 0 && logger_.stop();
  }

  static void restoreTask(void *context) {
    auto &self = *static_cast<RideRecorder *>(context);
    self.restoreSuccess_ = self.restore();
    self.restoreDone_.store(true);
    vTaskDelete(nullptr);
  }

  CsvLogger &logger_;
  FitExport &exporter_;
  RecordingJournal &journal_;
  Phase phase_ = Phase::Idle;
  bool ready_ = false, shutdown_ = false, exportRequested_ = false, restoreStarted_ = false;
  bool restoreSuccess_ = false, restored_ = false, published_ = false, messageReady_ = false;
  std::atomic<bool> restoreDone_{false};
  uint32_t startedMs_ = 0, duration_ = 0, skipped_ = 0, lastAckMs_ = 0;
  char message_[160] = {}, savedMessage_[160] = {};
};