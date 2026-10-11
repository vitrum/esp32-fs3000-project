#include "csv_logger.h"
#include "config.h"

namespace {
SemaphoreHandle_t g_sdMutex = nullptr;
}

bool beginSdCardMutex() {
  if (g_sdMutex) return true;
  g_sdMutex = xSemaphoreCreateMutex();
  return g_sdMutex != nullptr;
}

bool lockSdCard(TickType_t timeout) {
  return g_sdMutex && xSemaphoreTake(g_sdMutex, timeout) == pdTRUE;
}

void unlockSdCard() {
  if (g_sdMutex) xSemaphoreGive(g_sdMutex);
}

bool CsvLogger::begin() {
  if (!lockSdCard()) {
    Serial.println("[SD] Could not acquire SD mutex for initialization");
    return false;
  }
  SPI.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI);
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("SD card init failed");
    _ok = false;
    _sdReady = false;
    unlockSdCard();
    return _ok;
  }
  _sdReady = true;
  unlockSdCard();
  return start();
}

bool CsvLogger::start() {
  if (_ok) return true;
  if (!lockSdCard()) {
    Serial.println("[SD] Could not acquire SD mutex to start logging");
    return false;
  }
  if (!_sdReady) {
    SPI.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI);
    _sdReady = SD.begin(PIN_SD_CS);
    if (!_sdReady) {
      Serial.println("[SD] Cannot start logging; card mount failed");
      unlockSdCard();
      return false;
    }
  }

  uint32_t stamp = millis() % 1000000UL;
  bool uniqueName = false;
  for (uint16_t attempt = 0; attempt < 1000; ++attempt) {
    snprintf(_name, sizeof(_name), "/fs_%06lu.csv", (unsigned long)stamp);
    if (!SD.exists(_name)) {
      uniqueName = true;
      break;
    }
    stamp = (stamp + 1) % 1000000UL;
  }
  if (!uniqueName) {
    Serial.println("[SD] Could not find an unused log filename");
    unlockSdCard();
    return false;
  }
  _f = SD.open(_name, FILE_WRITE);
  if (!_f) {
    Serial.println("SD open failed");
    _ok = false;
    unlockSdCard();
    return _ok;
  }

  _f.println("t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc");
  _f.flush();
  _lastFlush = millis();
  _ok = true;
  unlockSdCard();
  Serial.printf("Logging to %s\n", _name);
  return _ok;
}

bool CsvLogger::stop() {
  if (!_ok) return true;
  if (!lockSdCard()) {
    Serial.println("[SD] Could not acquire SD mutex to stop logging");
    return false;
  }
  _f.flush();
  _f.close();
  _ok = false;
  unlockSdCard();
  return true;
}

void CsvLogger::log(uint32_t tMs, float powerW, float cadRpm, float vgMps,
                    float vAirMps, float yawDeg, float postureDeg, float rho,
                    uint8_t powerSrc) {
  if (!_ok) return;
  if (!lockSdCard(pdMS_TO_TICKS(100))) {
    Serial.println("[SD] Skipped CSV sample because the card is busy");
    return;
  }

  _f.printf("%lu,%.1f,%.1f,%.3f,%.3f,%.1f,%.1f,%.4f,%u\n",
            (unsigned long)tMs, powerW, cadRpm, vgMps, vAirMps, yawDeg,
            postureDeg, rho, (unsigned)powerSrc);

  if (millis() - _lastFlush >= 5000) {   // 每 5s 落盘一次，防掉电丢数据
    _f.flush();
    _lastFlush = millis();
  }
  unlockSdCard();
}
