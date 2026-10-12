#pragma once

#include <Arduino.h>
#include <FS.h>
#include "config.h"

// ============================================================================
// 音频模块（PCM5101 I2S DAC + 板载喇叭）
//
// 功能：
//   1) 开机 / 关机提示音（Wahoo 码表风格：上扬三音 / 下抑音）
//   2) 播放 SD 卡内 MP3 文件
//
// 实现：基于 schreibfaul1/ESP32-audioI2S。
//   - 单一 I2S 持有者（Audio 对象），避免多 I2S 争用引脚。
//   - 提示音以"合成 WAV 文件"形式实现：首次启动时若 SD 上缺少
//     /system/boot.wav、/system/shutdown.wav，则用正弦波合成写入 SD；
//     之后通过 connecttoFS 播放。复用同一 I2S 通路，稳定可靠。
//
// 依赖 SD 卡（与 CSV 日志同一张卡）。无卡时提示音自动跳过。
// ============================================================================

class AudioPlayer {
public:
  // 初始化 I2S 与引脚，并确保系统音效 WAV 存在（必要时合成到 SD）。
  // 需在 SD.begin() 之后调用。
  bool begin();

  // 主循环泵：推进解码/播放状态。非阻塞。
  void tick();

  // 播放开机 / 关机提示音（Wahoo 风格）。
  void playStartupChime();
  void playShutdownChime();

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
  void  ensureChimeFiles();               // 生成缺失的提示音 WAV
  bool  writeChimeWav(const char *path, const float *notes, const uint16_t *durs,
                      size_t count);
  void  beginPlayback(const char *path);

  bool _enabled = false;
  bool _playing = false;
  bool _chimeFilesReady = false;
  uint8_t _volume = AUDIO_VOLUME;
  char _title[96] = {};
};

extern AudioPlayer audio;
