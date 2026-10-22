#pragma once
#include "FS.h"
class Audio {
public:
  explicit Audio(int) {}
  void setBufsize(int, int) {}
  bool setPinout(int, int, int) { return true; }
  void setVolume(uint8_t) {}
  bool connecttoFS(fs::FS &, const char *) { return false; }
  void loop() {}
  bool isRunning() const { return false; }
  void pauseResume() {}
};