#pragma once

#include "saved_connections.h"

class StartupConnections {
public:
  bool begin() {
    SavedConnections saved;
    if (!saved.load()) { stopped_ = true; return false; }
    count_ = saved.bleCount();
    for (uint8_t index = 0; index < count_; ++index) entries_[index] = *saved.ble(index);
    next_ = 0;
    waiting_ = stopped_ = false;
    return true;
  }

  bool takeNext(bool wifiBusy, bool bleBusy, bool powerConnected, bool postureConnected,
                SavedConnections::Ble &entry) {
    if (stopped_ || waiting_ || wifiBusy || bleBusy) return false;
    while (next_ < count_) {
      entry = entries_[next_++];
      if (entry.role == SavedConnections::Role::Power ? powerConnected : postureConnected) continue;
      waiting_ = true;
      return true;
    }
    stopped_ = true;
    return false;
  }

  void complete() { waiting_ = false; }
  void cancel() { stopped_ = true; }
  bool waiting() const { return waiting_; }
  uint8_t attemptedIndex() const { return next_; }
  uint8_t count() const { return count_; }

private:
  SavedConnections::Ble entries_[SavedConnections::kCapacity] = {};
  uint8_t count_ = 0, next_ = 0;
  bool waiting_ = false, stopped_ = false;
};