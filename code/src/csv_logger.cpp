#include "csv_logger.h"

#include "config.h"

#include <SPI.h>
#include <FS.h>
#include <vfs_api.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>
#include <esp_err.h>

namespace {
SemaphoreHandle_t g_sdMutex = nullptr;

// 我们把 IDF 的 FATFS 挂到该挂载点，并用一个 Arduino fs::FS 包装它，
// 这样 SD.open()/exists()/mkdir() 等以及音频库(需要 fs::FS)都能继续用。
constexpr const char *kSdMountPoint = "/sdcard";
constexpr spi_host_device_t kSpiHost = SPI2_HOST;  // ESP32-S3 的空闲 SPI 主控

// 继承 fs::FS 以便设置受保护的 _impl 的 mountpoint
class VfsFs : public fs::FS {
public:
  VfsFs() : fs::FS(fs::FSImplPtr(new VFSImpl())) {}
  void setMountPoint(const char *mp) { _impl->mountpoint(mp); }
};

VfsFs  g_sdFs;
sdmmc_card_t *g_sdCard = nullptr;
bool    g_sdMounted = false;

// 与 Arduino SD() 默认挂载点不同：我们显式用 kSdMountPoint。
// 但为兼容历史代码里以 "/" 开头的路径，这里提供转换：VFSImpl 会自动
// 拼接 mountpoint，因此 "/xxx" 会正确落到 "/sdcard/xxx"。
}  // namespace

fs::FS &sdFs() { return g_sdFs; }

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

// 挂载 SD（SDSPI，走 ESP-IDF 原生 FATFS，比 Arduino SD 驱动更稳健：
// 不会因一次 CMD13 假失败就永久把卡标成 STA_NOINIT）。
static bool mountSdCard() {
  if (g_sdMounted) return true;

  Serial.printf("[SD] mounting via IDF SDSPI (SCLK=%d MISO=%d MOSI=%d CS=%d @ %luHz)...\n",
                PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS,
                (unsigned long)SD_SPI_FREQ_HZ);

  spi_bus_config_t busCfg = {};
  busCfg.mosi_io_num = PIN_SD_MOSI;
  busCfg.miso_io_num = PIN_SD_MISO;
  busCfg.sclk_io_num = PIN_SD_SCLK;
  busCfg.quadwp_io_num = -1;
  busCfg.quadhd_io_num = -1;
  busCfg.max_transfer_sz = 4000;

  esp_err_t err = spi_bus_initialize(kSpiHost, &busCfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    Serial.printf("[SD] spi_bus_initialize failed: %s\n", esp_err_to_name(err));
    return false;
  }

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = kSpiHost;
  host.max_freq_khz = SD_SPI_FREQ_HZ / 1000;
  // 大容量 SDXC 卡 SPI 下写后偶发状态查询慢；给足超时避免 ESP_ERR_TIMEOUT(0x107)。
  host.command_timeout_ms = 2000;

  sdspi_device_config_t slotCfg = SDSPI_DEVICE_CONFIG_DEFAULT();
  slotCfg.gpio_cs = (gpio_num_t)PIN_SD_CS;
  slotCfg.host_id = kSpiHost;

  esp_vfs_fat_sdmmc_mount_config_t mountCfg = {};
  mountCfg.format_if_mount_failed = false;   // 绝不自动格式化
  mountCfg.max_files = 5;
  mountCfg.allocation_unit_size = 16 * 1024;

  err = esp_vfs_fat_sdspi_mount(kSdMountPoint, &host, &slotCfg, &mountCfg,
                                &g_sdCard);
  if (err != ESP_OK) {
    Serial.printf("[SD] mount FAILED: %s\n", esp_err_to_name(err));
    Serial.println("[SD] hints: (1) the card must be FAT16/FAT32; a 64GB card is");
    Serial.println("[SD]        usually exFAT, which is NOT supported by this FW.");
    Serial.println("[SD]        Reformat it as FAT32 (use a PC tool for >32GB).");
    Serial.println("[SD]        (2) check wiring: SCLK=14 MISO=16 MOSI=17 CS=21.");
    spi_bus_free(kSpiHost);
    return false;
  }

  g_sdFs.setMountPoint(kSdMountPoint);
  g_sdMounted = true;

  const uint64_t mb = g_sdCard ? ((uint64_t)g_sdCard->csd.capacity / 1024) : 0;
  Serial.printf("[SD] mounted OK. size=%lluMB @ %luHz (IDF FATFS)\n",
                (unsigned long long)mb, (unsigned long)SD_SPI_FREQ_HZ);
  return true;
}

// 供其它模块查询 SD 是否就绪（替代 Arduino SD.cardType()）
bool sdCardReady() { return g_sdMounted; }

bool CsvLogger::begin() {
  if (!lockSdCard()) {
    Serial.println("[SD] Could not acquire SD mutex for initialization");
    return false;
  }
  if (!mountSdCard()) {
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
    _sdReady = mountSdCard();
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
    if (!sdFs().exists(_name)) {
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
  _f = sdFs().open(_name, FILE_WRITE);
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

static bool isAudioName(const char *n) {
  const char *dot = strrchr(n, '.');
  if (!dot) return false;
  return !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".wav") ||
         !strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".flac") ||
         !strcasecmp(dot, ".aac") || !strcasecmp(dot, ".ogg");
}

size_t listSdFiles(SdFileEntry *out, size_t maxCount) {
  if (!out || maxCount == 0) return 0;
  if (!lockSdCard(pdMS_TO_TICKS(500))) return 0;

  size_t count = 0;
  // 两遍扫描：先音频文件，再其它文件
  for (int pass = 0; pass < 2 && count < maxCount; ++pass) {
    File dir = sdFs().open("/");
    if (!dir) break;
    File entry = dir.openNextFile();
    while (entry && count < maxCount) {
      if (!entry.isDirectory()) {
        const char *name = entry.name();
        // Arduino SD 在不同 core 下可能带前导 '/'
        const char *base = name;
        const char *slash = strrchr(name, '/');
        if (slash) base = slash + 1;
        if (base[0] != '.') {
          const bool audioFile = isAudioName(base);
          if ((pass == 0 && audioFile) || (pass == 1 && !audioFile)) {
            snprintf(out[count].name, sizeof(out[count].name), "%s", base);
            out[count].size = entry.size();
            out[count].isAudio = audioFile;
            ++count;
          }
        }
      }
      entry = dir.openNextFile();
    }
    dir.close();
  }

  unlockSdCard();
  Serial.printf("[SD] listed %u file(s)\n", (unsigned)count);
  return count;
}

void CsvLogger::log(uint32_t tMs, float powerW, float cadRpm, float vgMps,
                    float vAirMps, float yawDeg, float postureDeg, float rho,
                    uint8_t powerSrc) {
  if (!_ok) return;
  if (_paused) return;   // 音频占用 SD 时暂不写盘
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
