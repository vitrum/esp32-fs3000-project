#pragma once
#include "Arduino.h"
#include <map>
#include <vector>

class Preferences {
public:
  bool begin(const char *name, bool readOnly) { namespace_ = name; lastReadOnly = readOnly; return !failOpen; }
  String getString(const char *key, const char *fallback) const {
    auto found = values.find(key);
    return found == values.end() ? String(fallback) : found->second;
  }
  size_t putString(const char *key, const String &value) {
    if (failWrite) return 0;
    values[key] = value;
    return value.size() + 1;
  }
  bool remove(const char *key) { return values.erase(key) != 0; }
  size_t getBytesLength(const char *key) const {
    auto found = bytes.find(namespace_ + "/" + key);
    return found == bytes.end() ? 0 : found->second.size();
  }
  size_t getBytes(const char *key, void *out, size_t size) const {
    auto found = bytes.find(namespace_ + "/" + key);
    if (found == bytes.end() || found->second.size() > size) return 0;
    memcpy(out, found->second.data(), found->second.size());
    return found->second.size();
  }
  size_t putBytes(const char *key, const void *data, size_t size) {
    if (failWrite) return 0;
    const auto *begin = static_cast<const uint8_t *>(data);
    bytes[namespace_ + "/" + key] = std::vector<uint8_t>(begin, begin + size);
    return size;
  }
  void end() {}
  inline static bool failOpen = false, failWrite = false;
  inline static bool lastReadOnly = false;
  inline static std::map<std::string, String> values;
  inline static std::map<std::string, std::vector<uint8_t>> bytes;
private:
  std::string namespace_;
};