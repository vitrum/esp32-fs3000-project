// ============================================================================
// 音频模块
//   1) 开机 / 关机提示音：直接用 I2S 发「嘟嘟」声（两短一长），
//      不依赖 SD 卡、不依赖任何音频文件。
//   2) 播放 SD 卡内 MP3：用 ESP32-audioI2S 库。
//
// 设计：统一使用 I2S_NUM_0（即 PCM5101 所在的 I2S 端口）。
//   - 为避免两个 I2S 使用者争用，MP3 播放的 Audio 对象按需创建、
//     播完/停止后销毁（其析构会 uninstall I2S）。
//   - 提示音直接安装 I2S_NUM_0 并写合成 PCM；发完即卸载。
//   - 二者不会同时占用 I2S。
// ============================================================================
#include "audio.h"
#include "config.h"
#include "csv_logger.h"   // sdFs() / sdCardReady()

#include <Audio.h>
#include <driver/i2s.h>
#include <memory>

AudioPlayer audio;

namespace {
constexpr uint32_t kSampleRate = 44100;
constexpr i2s_port_t kI2sPort = I2S_NUM_0;   // 与 PCM5101 一致

std::unique_ptr<Audio> g_audio;   // 仅在 MP3 播放期间存在
bool g_beeperReady = false;

// ---- 提示音 I2S（独占 I2S_NUM_0）----
bool beeperInit() {
  if (g_beeperReady) return true;
  if (g_audio) { g_audio->stopSong(); g_audio.reset(); }  // 释放库占用的 I2S

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = kSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;

  esp_err_t err = i2s_driver_install(kI2sPort, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] beeper i2s_driver_install failed: %d\n", err);
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.bck_io_num = PIN_I2S_BCLK;
  pins.ws_io_num = PIN_I2S_LRC;
  pins.data_out_num = PIN_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  err = i2s_set_pin(kI2sPort, &pins);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] beeper i2s_set_pin failed: %d\n", err);
    i2s_driver_uninstall(kI2sPort);
    return false;
  }
  i2s_zero_dma_buffer(kI2sPort);
  g_beeperReady = true;
  Serial.println("[AUDIO] beeper I2S ready (I2S0 48/38/47)");
  return true;
}

void beeperDeinit() {
  if (!g_beeperReady) return;
  i2s_driver_uninstall(kI2sPort);
  g_beeperReady = false;
}

// 发一个音：freq(Hz)，durMs 时长。50% 方波，两端 4ms 淡入淡出。
void beepTone(float freq, uint32_t durMs, float amplitude = 0.5f) {
  if (!g_beeperReady) return;
  const uint32_t total = (uint32_t)((uint64_t)kSampleRate * durMs / 1000);
  const uint32_t fade = kSampleRate * 4 / 1000;
  uint32_t halfPeriod = (uint32_t)(kSampleRate / (2.0f * freq));
  if (halfPeriod == 0) halfPeriod = 1;

  int16_t frame[256 * 2];
  uint32_t phase = 0;
  uint32_t produced = 0;
  while (produced < total) {
    uint32_t chunk = total - produced;
    if (chunk > 256) chunk = 256;
    for (uint32_t n = 0; n < chunk; ++n) {
      const uint32_t i = produced + n;
      float env = 1.0f;
      if (i < fade) env = (float)i / fade;
      else if (i > total - fade) env = (float)(total - i) / fade;
      if (env < 0) env = 0;
      if (env > 1) env = 1;
      const int16_t s = (phase < halfPeriod)
                            ? (int16_t)(30000 * amplitude * env)
                            : (int16_t)(-30000 * amplitude * env);
      frame[n * 2] = s;
      frame[n * 2 + 1] = s;
      if (++phase >= 2 * halfPeriod) phase = 0;
    }
    size_t written = 0;
    i2s_write(kI2sPort, frame, chunk * 2 * sizeof(int16_t), &written,
              portMAX_DELAY);
    produced += chunk;
  }
}

void beepSilence(uint32_t durMs) {
  if (!g_beeperReady) return;
  const uint32_t total = (uint32_t)((uint64_t)kSampleRate * durMs / 1000);
  int16_t frame[256 * 2] = {};
  uint32_t produced = 0;
  while (produced < total) {
    uint32_t chunk = total - produced;
    if (chunk > 256) chunk = 256;
    size_t written = 0;
    i2s_write(kI2sPort, frame, chunk * 2 * sizeof(int16_t), &written,
              portMAX_DELAY);
    produced += chunk;
  }
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
  Serial.printf("[AUDIO] ready (BCLK=%d LRC=%d DOUT=%d)\n",
                PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  return true;
#else
  Serial.println("[AUDIO] disabled by config");
  return false;
#endif
}

void AudioPlayer::tick() {
  if (!_enabled) return;
  if (g_audio) {
    g_audio->loop();
    if (_playing && !g_audio->isRunning()) {
      _playing = false;
    }
  }
}

// ============================================================================
// 提示音（两短一长）
// ============================================================================
void AudioPlayer::playStartupChime() {
  if (!_enabled) return;
  if (!beeperInit()) return;
  _playing = false;
  setTitle("beep");
  Serial.println("[AUDIO] startup chime (short-short-long)");
  beepTone(880.0f, 120);
  beepSilence(70);
  beepTone(880.0f, 120);
  beepSilence(70);
  beepTone(1175.0f, 400);
  beepSilence(80);
  beeperDeinit();
  setTitle("");
}

void AudioPlayer::playShutdownChime() {
  if (!_enabled) return;
  if (!beeperInit()) return;
  _playing = false;
  setTitle("beep");
  Serial.println("[AUDIO] shutdown chime (short-short-long, falling)");
  beepTone(880.0f, 120);
  beepSilence(70);
  beepTone(660.0f, 120);
  beepSilence(70);
  beepTone(440.0f, 400);
  beepSilence(80);
  beeperDeinit();
  setTitle("");
}

// 用当前音量发一声短促「叮」，用于设置界面试听。
void AudioPlayer::playBeepPreview() {
  if (!_enabled) return;
  if (!beeperInit()) return;
  beepTone(1046.5f, 180);   // C6
  beepSilence(40);
  beeperDeinit();
}

// ============================================================================
// MP3 播放（按需创建 Audio，独占 I2S_NUM_0）
// ============================================================================
void AudioPlayer::beginPlayback(const char *path) {
  if (!sdCardReady()) return;

  if (g_beeperReady) beeperDeinit();   // 释放提示音占用的 I2S

  if (!g_audio) {
    g_audio = std::unique_ptr<Audio>(new Audio(kI2sPort));
    g_audio->setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  }
  g_audio->setVolume(_volume);
  if (!g_audio->connecttoFS(sdFs(), path)) {
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
  if (g_audio) g_audio->stopSong();
  beginPlayback(path);
  return _playing;
}

void AudioPlayer::stop() {
  if (!_enabled) return;
  if (g_audio) g_audio->stopSong();
  _playing = false;
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
