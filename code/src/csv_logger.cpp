#include "csv_logger.h"
#include "config.h"

bool CsvLogger::begin() {
  SPI.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI);
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("SD card init failed");
    _ok = false;
    return _ok;
  }

  // 以启动时间生成唯一文件名：/fs_<秒>.csv
  snprintf(_name, sizeof(_name), "/fs_%06lu.csv",
           (unsigned long)(millis() % 1000000UL));

  _f = SD.open(_name, FILE_WRITE);
  if (!_f) {
    Serial.println("SD open failed");
    _ok = false;
    return _ok;
  }

  _f.println("t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc");
  _f.flush();
  _lastFlush = millis();
  _ok = true;
  Serial.printf("Logging to %s\n", _name);
  return _ok;
}

void CsvLogger::log(uint32_t tMs, float powerW, float cadRpm, float vgMps,
                    float vAirMps, float yawDeg, float postureDeg, float rho,
                    uint8_t powerSrc) {
  if (!_ok) return;

  _f.printf("%lu,%.1f,%.1f,%.3f,%.3f,%.1f,%.1f,%.4f,%u\n",
            (unsigned long)tMs, powerW, cadRpm, vgMps, vAirMps, yawDeg,
            postureDeg, rho, (unsigned)powerSrc);

  if (millis() - _lastFlush >= 5000) {   // 每 5s 落盘一次，防掉电丢数据
    _f.flush();
    _lastFlush = millis();
  }
}
