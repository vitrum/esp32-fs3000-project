#pragma once

#include <Arduino.h>
#include <FS.h>
#include "config.h"

// ============================================================================
// 音频模块（PCM5101 I2S DAC + 板载喇叭）
//
// 功能：播放 SD 卡内音频文件。开关机和试听提示音已关闭，以免额外申请 I2S DMA。
// MP3 播放使用 schreibfaul1/ESP32-audioI2S 和 I2S_NUM_0。
// ============================================================================
class AudioPlayer {
public:
  // 初始化音频播放模块。需在 SD 挂载之后调用。
  bool begin();

  // 主循环泵：推进解码/播放状态。非阻塞。
  void tick();

  // 播放开机 / 关机提示音（两短一长的「嘟嘟」声）。
  void playStartupChime();
  void playShutdownChime();

  // 用当前音量发一声短「叮」（设置界面试听）。
  void playBeepPreview();

  // 播放 SD 卡内指定文件（MP3/WAV）。返回是否成功启动。
  bool playFile(const char *path);
  void releasePlayback();

  void stop();
  void pauseResume();
  void setVolume(uint8_t vol);           // 0~100
  uint8_t volume() const { return _volume; }

  bool playing() const { return _playing; }
  bool enabled() const { return _enabled; }
  const char *currentTitle() const { return _title; }
  void setTitle(const char *t);

private:
  void  beginPlayback(const char *path);

  bool _enabled = false;
  bool _playing = false;
  uint8_t _volume = AUDIO_VOLUME;
  char _title[96] = {};
};

extern AudioPlayer audio;


