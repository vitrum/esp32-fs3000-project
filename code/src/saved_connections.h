#pragma once

#include <Preferences.h>
#include <esp_rom_crc.h>
#include <cstring>
#include <cctype>
#include <strings.h>

class SavedConnections {
public:
  static constexpr uint8_t kCapacity = 4;
  enum class Role : uint8_t { Power, Posture, Heart };
  struct Wifi { char ssid[33]; char password[65]; };
  struct Ble { char address[18]; uint8_t addressType; Role role; };
  struct Favorite { Ble device; char name[32]; };

  bool loadFavorites() {
    Preferences prefs;
    if (!prefs.begin("aero-links", false)) return false;
    Favorites next{};
    const size_t length = prefs.getBytesLength("favorites");
    const bool read = length == 0 || (length == sizeof(next) &&
        prefs.getBytes("favorites", &next, sizeof(next)) == sizeof(next));
    prefs.end();
    if (!read) return false;
    if (length && (next.magic != kMagic || next.count > kCapacity ||
        next.crc != esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&next), offsetof(Favorites, crc)))) return false;
    for (uint8_t index = 0; index < next.count; ++index)
      if (!validBle(next.entries[index].device) || next.entries[index].device.role != Role::Power ||
          !terminated(next.entries[index].name)) return false;
    favorites_ = next;
    return true;
  }
  uint8_t favoriteCount() const { return favorites_.count; }
  const Favorite *favorite(uint8_t index) const {
    return index < favoriteCount() ? &favorites_.entries[index] : nullptr;
  }
  bool isFavorite(const char *address, uint8_t type) const {
    if (!address) return false;
    for (uint8_t index = 0; index < favoriteCount(); ++index)
      if (favorites_.entries[index].device.addressType == type &&
          !strcasecmp(favorites_.entries[index].device.address, address)) return true;
    return false;
  }
  bool setFavorite(const char *address, uint8_t type, const char *name, bool enabled) {
    if (!address || !name || strlen(address) != 17 || strlen(name) >= 32) return false;
    Favorite entry{};
    snprintf(entry.device.address, sizeof(entry.device.address), "%s", address);
    entry.device.addressType = type;
    entry.device.role = Role::Power;
    snprintf(entry.name, sizeof(entry.name), "%s", name);
    if (!validBle(entry.device)) return false;
    Favorites next = favorites_;
    uint8_t found = next.count;
    for (uint8_t index = 0; index < next.count; ++index)
      if (next.entries[index].device.addressType == type &&
          !strcasecmp(next.entries[index].device.address, address)) { found = index; break; }
    if (enabled) {
      if (found == next.count) {
        if (next.count == kCapacity) return false;
        ++next.count;
      }
      next.entries[found] = entry;
    } else {
      if (found == next.count) return true;
      for (uint8_t index = found; index + 1 < next.count; ++index) next.entries[index] = next.entries[index + 1];
      next.entries[--next.count] = Favorite{};
    }
    next.magic = kMagic;
    next.crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&next), offsetof(Favorites, crc));
    Preferences prefs;
    if (!prefs.begin("aero-links", false)) return false;
    const bool written = prefs.putBytes("favorites", &next, sizeof(next)) == sizeof(next);
    prefs.end();
    if (written) favorites_ = next;
    return written;
  }

  bool load() {
    Preferences prefs;
    if (!prefs.begin("aero-links", false)) return false;
    const size_t length = prefs.getBytesLength("profiles");
    Store next{};
    const bool read = length == 0 || (length == sizeof(next) &&
        prefs.getBytes("profiles", &next, sizeof(next)) == sizeof(next));
    prefs.end();
    if (!read) return false;
    if (length == 0) { store_ = Store{}; return true; }
    if (next.magic != kMagic || next.version != 1 || next.wifiCount > kCapacity ||
        next.bleCount > kCapacity || next.crc != checksum(next)) return false;
    for (uint8_t index = 0; index < next.wifiCount; ++index)
      if (!terminated(next.wifi[index].ssid) || !next.wifi[index].ssid[0] ||
          !terminated(next.wifi[index].password)) return false;
    for (uint8_t index = 0; index < next.bleCount; ++index)
      if (!validBle(next.ble[index])) return false;
    store_ = next;
    return true;
  }

  uint8_t wifiCount() const { return store_.wifiCount; }
  uint8_t bleCount() const { return store_.bleCount; }
  const Wifi *wifi(uint8_t index) const { return index < wifiCount() ? &store_.wifi[index] : nullptr; }
  const Ble *ble(uint8_t index) const { return index < bleCount() ? &store_.ble[index] : nullptr; }

  bool rememberWifi(const char *ssid, const char *password) {
    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 64) return false;
    Store next = store_;
    uint8_t found = next.wifiCount;
    for (uint8_t index = 0; index < next.wifiCount; ++index)
      if (!strcmp(next.wifi[index].ssid, ssid)) { found = index; break; }
    const uint8_t last = found < kCapacity ? found : kCapacity - 1;
    for (uint8_t index = last; index > 0; --index) next.wifi[index] = next.wifi[index - 1];
    next.wifi[0] = Wifi{};
    snprintf(next.wifi[0].ssid, sizeof(next.wifi[0].ssid), "%s", ssid);
    snprintf(next.wifi[0].password, sizeof(next.wifi[0].password), "%s", password);
    if (found == next.wifiCount && next.wifiCount < kCapacity) ++next.wifiCount;
    if (!memcmp(next.wifi, store_.wifi, sizeof(next.wifi)) && next.wifiCount == store_.wifiCount) return true;
    return save(next);
  }

  bool rememberBle(const char *address, uint8_t type, Role role) {
    if (!address || strlen(address) != 17) return false;
    Ble entry{};
    snprintf(entry.address, sizeof(entry.address), "%s", address);
    entry.addressType = type;
    entry.role = role;
    if (!validBle(entry)) return false;
    Store next = store_;
    uint8_t found = next.bleCount;
    for (uint8_t index = 0; index < next.bleCount; ++index)
      if (!strcmp(next.ble[index].address, address) && next.ble[index].addressType == type &&
          next.ble[index].role == role) { found = index; break; }
    const uint8_t last = found < kCapacity ? found : kCapacity - 1;
    for (uint8_t index = last; index > 0; --index) next.ble[index] = next.ble[index - 1];
    next.ble[0] = entry;
    if (found == next.bleCount && next.bleCount < kCapacity) ++next.bleCount;
    if (!memcmp(next.ble, store_.ble, sizeof(next.ble)) && next.bleCount == store_.bleCount) return true;
    return save(next);
  }

  bool clearWifi() {
    Store next = store_;
    next.wifiCount = 0;
    memset(next.wifi, 0, sizeof(next.wifi));
    return save(next);
  }

private:
  struct Favorites {
    uint32_t magic;
    uint8_t count;
    uint8_t reserved[3];
    Favorite entries[kCapacity];
    uint32_t crc;
  };
  Favorites favorites_{};
  struct Store {
    uint32_t magic, version;
    uint8_t wifiCount, bleCount;
    uint16_t reserved;
    Wifi wifi[kCapacity];
    Ble ble[kCapacity];
    uint32_t crc;
  };
  static constexpr uint32_t kMagic = 0x41504c31;
  template<size_t size> static bool terminated(const char (&text)[size]) {
    return memchr(text, 0, size) != nullptr;
  }
  static bool validBle(const Ble &entry) {
    if (!terminated(entry.address) || strlen(entry.address) != 17 || entry.addressType > 3 ||
        (entry.role != Role::Power && entry.role != Role::Posture && entry.role != Role::Heart)) return false;
    for (unsigned index = 0; index < 17; ++index)
      if (index % 3 == 2 ? entry.address[index] != ':' :
          !std::isxdigit(static_cast<unsigned char>(entry.address[index]))) return false;
    return true;
  }
  static uint32_t checksum(const Store &store) {
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&store), offsetof(Store, crc));
  }
  bool save(Store &next) {
    next.magic = kMagic;
    next.version = 1;
    next.crc = checksum(next);
    Preferences prefs;
    if (!prefs.begin("aero-links", false)) return false;
    const bool written = prefs.putBytes("profiles", &next, sizeof(next)) == sizeof(next);
    prefs.end();
    if (written) store_ = next;
    return written;
  }
  Store store_{};
};