#pragma once

#include <Arduino.h>
#include <FS.h>
#include "config.h"

// ============================================================================
// 音频模块（PCM5101 I2S DAC + 板载喇叭）
//
// 功能：
//   1) 开机 / 关机提示音：优先播放编译进固件的 WAV 音效
//      （assets/boot.wav / assets/shutdown.wav，PCM 16bit 44.1kHz 立体声）；
//      若内嵌资源缺失或解析失败，回退到 I2S 合成的「嘟嘟」声。
//      不依赖 SD 卡、不依赖任何外部音频文件。
//   2) 播放 SD 卡内 MP3 文件
//
// 实现：
//   - MP3：基于 schreibfaul1/ESP32-audioI2S，走 I2S_NUM_0。
//   - 提示音：独立 I2S_NUM_1 直接输出合成 PCM（方波），
//     仅开机/关机时发声，与 MP3 播放不重叠。
// ============================================================================
class AudioPlayer {
public:
  // 初始化 I2S 与引脚。需在 SD.begin() 之后调用。
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

  // 从内存 WAV 数据（PCM 16bit）直接写 I2S 播放；成功返回 true。
  // 需已 beeperInit()。解析失败返回 false（调用方可回退合成音）。
  bool  playEmbeddedWav(const uint8_t *data, size_t len);

  bool _enabled = false;
  bool _playing = false;
  uint8_t _volume = AUDIO_VOLUME;
  char _title[96] = {};
};

extern AudioPlayer audio;


