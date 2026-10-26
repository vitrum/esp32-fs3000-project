#pragma once

#include "saved_connections.h"
#include "ble_power.h"

class StartupConnections {
public:
  bool begin() {
    SavedConnections saved;
    if (!saved.load()) { stopped_ = true; return false; }
    if (!saved.loadFavorites()) { stopped_ = true; return false; }
    count_ = 0;
    for (uint8_t index = 0; index < saved.favoriteCount(); ++index)
      entries_[count_++] = saved.favorite(index)->device;
    for (uint8_t index = 0; index < saved.bleCount(); ++index) {
      const auto &entry = *saved.ble(index);
      if (entry.role == SavedConnections::Role::Power && saved.isFavorite(entry.address, entry.addressType)) continue;
      entries_[count_++] = entry;
    }
    next_ = 0;
    waiting_ = stopped_ = false;
    return true;
  }

  void beginScan(const BlePowerDevice *devices, size_t count) {
    count_ = next_ = 0;
    waiting_ = stopped_ = false;
    for (size_t index = 0; index < count && count_ < SavedConnections::kCapacity; ++index) {
      if (!devices[index].favorite) continue;
      auto &entry = entries_[count_++];
      snprintf(entry.address, sizeof(entry.address), "%s", devices[index].address);
      entry.addressType = devices[index].addressType;
      entry.role = SavedConnections::Role::Power;
    }
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
  SavedConnections::Ble entries_[SavedConnections::kCapacity * 2] = {};
  uint8_t count_ = 0, next_ = 0;
  bool waiting_ = false, stopped_ = false;
};