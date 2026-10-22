#pragma once
#include "Arduino.h"
#include <map>

class Preferences {
public:
  bool begin(const char *, bool) { return !failOpen; }
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
  void end() {}
  inline static bool failOpen = false, failWrite = false;
  inline static std::map<std::string, String> values;
};