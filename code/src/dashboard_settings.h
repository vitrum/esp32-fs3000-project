#pragma once

#include "dashboard_layout.h"
#include <Preferences.h>
#include <esp_rom_crc.h>
#include <cstddef>

class DashboardSettings {
public:
  static bool load(DashboardLayout::Settings &settings) {
    Preferences prefs;
    if (!prefs.begin("aero-ui", false)) return false;
    Store store{};
    const size_t size = prefs.getBytesLength("dashboard");
    const bool read = size == 0 || (size == sizeof(store) && prefs.getBytes("dashboard", &store, sizeof(store)) == sizeof(store));
    prefs.end();
    DashboardLayout layout;
    if (!read || (size && (store.version != 1 || store.crc != checksum(store) || !layout.load(store.settings)))) return false;
    settings = layout.settings();
    return true;
  }
  static bool save(const DashboardLayout::Settings &settings) {
    DashboardLayout layout;
    if (!layout.load(settings)) return false;
    Store store{};
    store.version = 1;
    store.settings = settings;
    store.crc = checksum(store);
    Preferences prefs;
    if (!prefs.begin("aero-ui", false)) return false;
    const bool saved = prefs.putBytes("dashboard", &store, sizeof(store)) == sizeof(store);
    prefs.end();
    return saved;
  }
private:
  struct Store { uint32_t version; DashboardLayout::Settings settings; uint16_t reserved; uint32_t crc; };
  static uint32_t checksum(const Store &store) {
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&store), offsetof(Store, crc));
  }
};