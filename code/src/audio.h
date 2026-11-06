#pragma once

#include <Arduino.h>
#include "config.h"

// ============================================================================
// 音频模块（PCM5101 I2S DAC + 板载喇叭）
//
// 系统提示音与音量设置。媒体文件播放已移除。
// ============================================================================
class AudioPlayer {
public:
  // 初始化声音控制。
  bool begin();
  void tick();
  void playStartupChime();
  void playShutdownChime();
  void playBeepPreview();
  void setVolume(uint8_t vol);           // 0~100
  uint8_t volume() const { return _volume; }
  bool enabled() const { return _enabled; }

private:
  bool _enabled = false;
  uint8_t _volume = AUDIO_VOLUME;
};

extern AudioPlayer audio;
