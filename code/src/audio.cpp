#include "audio.h"
#include "runtime_log.h"

AudioPlayer audio;

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

void AudioPlayer::tick() {}

void AudioPlayer::playStartupChime() {
  AppLog.println("[AUDIO] startup chime disabled");
}

void AudioPlayer::playShutdownChime() {
  AppLog.println("[AUDIO] shutdown chime disabled");
}

void AudioPlayer::playBeepPreview() {
  AppLog.println("[AUDIO] beep preview disabled");
}

void AudioPlayer::setVolume(uint8_t vol) {
  _volume = vol > 100 ? 100 : vol;
}
