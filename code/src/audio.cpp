// SD-card audio playback through ESP32-audioI2S.
#include "audio.h"
#include "runtime_log.h"
#include "config.h"
#include "csv_logger.h"   // sdFs() / sdCardReady()

#include <Audio.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <memory>
#include <new>

AudioPlayer audio;

namespace {
constexpr i2s_port_t kI2sPort = I2S_NUM_0;   // 与 PCM5101 一致
constexpr size_t kAudioDmaCount = 4;
constexpr size_t kAudioDmaLength = 128;
constexpr size_t kAudioMinDmaBlock = 12 * 1024;

std::unique_ptr<Audio> g_audio;   // 仅在 MP3 播放期间存在
bool g_audioI2sInstalled = false;

bool installAudioI2s() {
  if (g_audioI2sInstalled) return true;
  const size_t largestBlock =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (largestBlock < kAudioMinDmaBlock) {
    AppLog.printf("[AUDIO] insufficient DMA heap: largest=%u required=%u\n",
                  (unsigned)largestBlock, (unsigned)kAudioMinDmaBlock);
    return false;
  }

  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = 44100;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = kAudioDmaCount;
  config.dma_buf_len = kAudioDmaLength;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;

  const esp_err_t result = i2s_driver_install(kI2sPort, &config, 0, nullptr);
  if (result != ESP_OK) {
    AppLog.printf("[AUDIO] I2S DMA install failed: %s free=%u largest=%u\n",
                  esp_err_to_name(result),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return false;
  }
  g_audioI2sInstalled = true;
  return true;
}

void uninstallAudioI2s() {
  if (!g_audioI2sInstalled) return;
  i2s_driver_uninstall(kI2sPort);
  g_audioI2sInstalled = false;
}
}  // namespace

void AudioPlayer::setTitle(const char *t) {
  snprintf(_title, sizeof(_title), "%s", t ? t : "");
}

// ============================================================================
// 初始化 / 泵
// ============================================================================
bool AudioPlayer::begin() {
#if AUDIO_ENABLE
  _enabled = true;
  AppLog.printf("[AUDIO] ready (BCLK=%d LRC=%d DOUT=%d)\n",
                PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  return true;
#else
  AppLog.println("[AUDIO] disabled by config");
  return false;
#endif
}

void AudioPlayer::tick() {
  if (!_enabled || !g_audio) return;
  if (!lockSdCard(0)) {
    static uint32_t lastBusyMs = 0;
    if (millis() - lastBusyMs >= 5000) {
      lastBusyMs = millis();
      AppLog.println("[AUDIO] waiting for SD card");
    }
    return;
  }
  g_audio->loop();
  unlockSdCard();
  if (_playing && !g_audio->isRunning()) _playing = false;
}

void AudioPlayer::playStartupChime() {
  AppLog.println("[AUDIO] startup chime disabled");
}

void AudioPlayer::playShutdownChime() {
  AppLog.println("[AUDIO] shutdown chime disabled");
}

void AudioPlayer::playBeepPreview() {
  AppLog.println("[AUDIO] beep preview disabled");
}

// ============================================================================
// MP3 播放（按需创建 Audio，独占 I2S_NUM_0）
// ============================================================================
void AudioPlayer::beginPlayback(const char *path) {
  if (!sdCardReady() || !path || !path[0]) return;

  if (!g_audio) {
    if (!installAudioI2s()) return;
    Audio *player = new (std::nothrow) Audio(kI2sPort);
    if (!player) {
      uninstallAudioI2s();
      AppLog.println("[AUDIO] insufficient heap for decoder");
      return;
    }
    g_audio.reset(player);
    g_audio->setBufsize(5000, 0);
    if (!g_audio->setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT)) {
      AppLog.println("[AUDIO] could not configure I2S pins");
      releasePlayback();
      return;
    }
  }
  g_audio->setVolume(_volume);
  if (!lockSdCard()) {
    AppLog.println("[AUDIO] SD card busy; could not open audio file");
    return;
  }
  const bool opened = g_audio->connecttoFS(sdFs(), path);
  unlockSdCard();
  if (!opened) {
    AppLog.printf("[AUDIO] play failed: %s\n", path);
    _playing = false;
    return;
  }
  _playing = true;
  setTitle(path);
  AppLog.printf("[AUDIO] playing: %s\n", path);
}

bool AudioPlayer::playFile(const char *path) {
  if (!_enabled || !path || !path[0]) return false;
  if (!releasePlayback()) return false;
  beginPlayback(path);
  return _playing;
}

bool AudioPlayer::releasePlayback() {
  if (g_audio) {
    if (!lockSdCard()) {
      AppLog.println("[AUDIO] SD card busy; could not release playback");
      return false;
    }
    g_audio.reset();
    unlockSdCard();
  }
  uninstallAudioI2s();
  _playing = false;
  setTitle("");
  return true;
}

bool AudioPlayer::stop() {
  if (!_enabled) return true;
  return releasePlayback();
}

void AudioPlayer::pauseResume() {
  if (!_enabled || !g_audio) return;
  g_audio->pauseResume();
  _playing = g_audio->isRunning();
}

void AudioPlayer::setVolume(uint8_t vol) {
  _volume = vol > 100 ? 100 : vol;
  if (g_audio) g_audio->setVolume(_volume);
}
