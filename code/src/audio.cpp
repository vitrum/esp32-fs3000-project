#include "audio.h"
#include "config.h"
#include "csv_logger.h"   // sdFs() / sdCardReady()

#include <Audio.h>

AudioPlayer audio;

namespace {
Audio g_audio(I2S_NUM_0);

constexpr uint32_t kSampleRate = 44100;

// ---- 小端写入辅助 ----
void wU16(File &f, uint16_t v) { f.write((uint8_t)(v & 0xFF)); f.write((uint8_t)(v >> 8)); }
void wU32(File &f, uint32_t v) { for (int i = 0; i < 4; ++i) f.write((uint8_t)((v >> (8 * i)) & 0xFF)); }
}

void AudioPlayer::setTitle(const char *t) {
  snprintf(_title, sizeof(_title), "%s", t ? t : "");
}

// ============================================================================
// 提示音 WAV 合成（正弦 + 衰减包络），返回是否写成功
// ============================================================================
bool AudioPlayer::writeChimeWav(const char *path, const float *notes,
                                const uint16_t *durs, size_t count) {
  // 计算总样本数
  uint32_t totalSamples = 0;
  for (size_t i = 0; i < count; ++i) {
    totalSamples += (uint32_t)((uint64_t)kSampleRate * durs[i] / 1000);
  }


  File f = sdFs().open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[AUDIO] cannot write chime %s\n", path);
    return false;
  }

  const uint16_t channels = 1;
  const uint16_t bits = 16;
  const uint32_t dataBytes = totalSamples * channels * (bits / 8);

  // ---- WAV 头 (44 字节) ----
  f.write((const uint8_t *)"RIFF", 4);
  wU32(f, 36 + dataBytes);
  f.write((const uint8_t *)"WAVE", 4);
  f.write((const uint8_t *)"fmt ", 4);
  wU32(f, 16);                 // fmt chunk size
  wU16(f, 1);                  // PCM
  wU16(f, channels);
  wU32(f, kSampleRate);
  wU32(f, kSampleRate * channels * (bits / 8));  // byte rate
  wU16(f, channels * (bits / 8));                // block align
  wU16(f, bits);
  f.write((const uint8_t *)"data", 4);
  wU32(f, dataBytes);

  // ---- 采样数据 ----
  // 简单包络：每音起头 5ms 淡入，结尾 30ms 淡出，避免爆音
  for (size_t n = 0; n < count; ++n) {
    const float freq = notes[n];
    const uint32_t len = (uint32_t)((uint64_t)kSampleRate * durs[n] / 1000);
    const uint32_t fadeIn = kSampleRate * 5 / 1000;
    const uint32_t fadeOut = kSampleRate * 30 / 1000;
    for (uint32_t i = 0; i < len; ++i) {
      float env = 1.0f;
      if (i < fadeIn) env = (float)i / fadeIn;
      else if (i > len - fadeOut) env = (float)(len - i) / fadeOut;
      if (env < 0) env = 0;
      if (env > 1) env = 1;
      float s = sinf(2.0f * PI * freq * (float)i / kSampleRate);
      // 叠加二次谐波，音色更接近码表"叮"声
      s = 0.75f * s + 0.25f * sinf(4.0f * PI * freq * (float)i / kSampleRate);
      int16_t v = (int16_t)(s * env * 30000.0f);
      wU16(f, (uint16_t)v);
    }
    // 音间小幅静音间隙（10ms）
    const uint32_t gap = kSampleRate * 10 / 1000;
    for (uint32_t i = 0; i < gap; ++i) wU16(f, 0);
  }

  f.flush();
  f.close();
  Serial.printf("[AUDIO] chime synthesized: %s (%lu samples)\n", path,
                (unsigned long)totalSamples);
  return true;
}

void AudioPlayer::ensureChimeFiles() {
  if (_chimeFilesReady) return;

  // 确保目录存在（Arduino SD 无 mkdir？有 SD.mkdir）
  if (!SD.exists(AUDIO_SYSTEM_DIR)) {
    SD.mkdir(AUDIO_SYSTEM_DIR);
  }

  // Wahoo 风格开机音：上扬四音 C5-E5-G5-C6（清脆、短促）
  if (!SD.exists(AUDIO_BOOT_WAV)) {
    static const float bootNotes[] = {523.25f, 659.25f, 783.99f, 1046.50f};
    static const uint16_t bootDurs[] = {110, 110, 110, 260};
    writeChimeWav(AUDIO_BOOT_WAV, bootNotes, bootDurs, 4);
  }
  // 关机音：下抑两音 G5-C5
  if (!SD.exists(AUDIO_SHUTDOWN_WAV)) {
    static const float downNotes[] = {783.99f, 523.25f};
    static const uint16_t downDurs[] = {160, 320};
    writeChimeWav(AUDIO_SHUTDOWN_WAV, downNotes, downDurs, 2);
  }
  _chimeFilesReady = true;
}

// ============================================================================
// 初始化 / 泵
// ============================================================================
bool AudioPlayer::begin() {
#if AUDIO_ENABLE
  g_audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  g_audio.setVolume(_volume);
  _enabled = true;
  Serial.printf("[AUDIO] PCM5101 I2S ready (BCLK=%d LRC=%d DOUT=%d)\n",
                PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);

  // 若 SD 可用，准备提示音文件
  if (SD.cardType() != CARD_NONE) {
    ensureChimeFiles();
  } else {
    Serial.println("[AUDIO] SD card absent; chimes will be silent");
  }
  return true;
#else
  Serial.println("[AUDIO] disabled by config");
  return false;
#endif
}

void AudioPlayer::tick() {
  if (!_enabled) return;
  g_audio.loop();
  if (_playing && !g_audio.isRunning()) {
    _playing = false;
  }
}

// ============================================================================
// 播放控制
// ============================================================================
void AudioPlayer::beginPlayback(const char *path) {
  if (SD.cardType() == CARD_NONE) return;
  g_audio.setVolume(_volume);
  if (!g_audio.connecttoFS(SD, path)) {
    Serial.printf("[AUDIO] play failed: %s\n", path);
    _playing = false;
    return;
  }
  _playing = true;
  setTitle(path);
  Serial.printf("[AUDIO] playing: %s\n", path);
}

bool AudioPlayer::playFile(const char *path) {
  if (!_enabled || !path || !path[0]) return false;
  g_audio.stopSong();
  beginPlayback(path);
  return _playing;
}

void AudioPlayer::playStartupChime() {
  if (!_enabled) return;
  if (!SD.exists(AUDIO_BOOT_WAV)) ensureChimeFiles();
  if (SD.exists(AUDIO_BOOT_WAV)) beginPlayback(AUDIO_BOOT_WAV);
}

void AudioPlayer::playShutdownChime() {
  if (!_enabled) return;
  if (SD.exists(AUDIO_SHUTDOWN_WAV)) beginPlayback(AUDIO_SHUTDOWN_WAV);
}

void AudioPlayer::stop() {
  if (!_enabled) return;
  g_audio.stopSong();
  _playing = false;
}

void AudioPlayer::pauseResume() {
  if (!_enabled) return;
  g_audio.pauseResume();
  _playing = g_audio.isRunning();
}

void AudioPlayer::setVolume(uint8_t vol) {
  _volume = vol > 100 ? 100 : vol;
  if (_enabled) g_audio.setVolume(_volume);
}
