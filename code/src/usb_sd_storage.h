#pragma once

#include "usb_sd_export.h"
#include <Preferences.h>

class UsbSdSetting {
public:
  static bool load(bool &enabled) {
    enabled = false;
    Preferences prefs;
    if (!prefs.begin("aero-ui", false)) return false;
    uint32_t value = 0;
    const size_t length = prefs.getBytesLength("usb-msc");
    const bool valid = length == 0 || (length == sizeof(value) &&
        prefs.getBytes("usb-msc", &value, sizeof(value)) == sizeof(value) &&
        (value == 0 || value == 0x55425331));
    prefs.end();
    if (valid) enabled = value == 0x55425331;
    return valid;
  }
  static bool save(bool enabled) {
    Preferences prefs;
    if (!prefs.begin("aero-ui", false)) return false;
    const uint32_t value = enabled ? 0x55425331 : 0;
    const bool saved = prefs.putBytes("usb-msc", &value, sizeof(value)) == sizeof(value);
    prefs.end();
    return saved;
  }
};

class UsbSdStorage {
public:
  bool beginAtBoot();
  bool exclusive() const;
  bool ready() const;
  bool ejected() const;
  bool hostConnected() const;
  void eject();
};