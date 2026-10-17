#include "csv_logger.h"

#include "config.h"

#include <SPI.h>
#include <FS.h>
#include <dirent.h>
#include <sys/stat.h>
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
const char *sdMountPoint() { return kSdMountPoint; }

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

// 运行期轻量检测：写入 SD 之前确认卡仍在位并响应。
// g_sdMounted 仅在挂载时置 true，运行中拔卡不会自动更新，因此这里用
// sdmmc_get_status() 主动查询卡状态；失败即视为掉线。
// 这样可避免对不在线的卡反复 write 造成 ESP_ERR_TIMEOUT(0x107) 刷屏与阻塞。
bool sdCardOnline() {
  if (!g_sdMounted || !g_sdCard) return false;
  return sdmmc_get_status(g_sdCard) == ESP_OK;
}

// 确保目录存在（不存在则创建；已存在直接返回 true）。
bool CsvLogger::ensureDir(const char *path) {
  if (!path || !path[0]) return false;
  if (sdFs().exists(path)) return true;
  return sdFs().mkdir(path);
}

// 依据当前日期选定目录并新建文件，写入表头。调用方需已持有 SD 锁。
bool CsvLogger::openFileForDate() {
  // 若已有打开的文件，先关闭（flush 剩余数据）。
  if (_f) {
    flushBuffer();
    _f.flush();
    _f.close();
  }

  // 选定目录：有日期 → /LOG/<date>，否则 → /LOG
  if (_date[0]) {
    snprintf(_dir, sizeof(_dir), "/LOG/%s", _date);
  } else {
    snprintf(_dir, sizeof(_dir), "/LOG");
  }
  if (!ensureDir("/LOG")) {
    Serial.println("[SD] Could not create /LOG directory");
    return false;
  }
  if (_date[0] && !ensureDir(_dir)) {
    Serial.printf("[SD] Could not create date directory %s\n", _dir);
    return false;
  }

  // 生成唯一文件名：/LOG[/date]/fs_<millis>.csv
  uint32_t stamp = millis() % 1000000UL;
  bool uniqueName = false;
  for (uint16_t attempt = 0; attempt < 1000; ++attempt) {
    snprintf(_name, sizeof(_name), "%s/fs_%06lu.csv", _dir, (unsigned long)stamp);
    if (!sdFs().exists(_name)) {
      uniqueName = true;
      break;
    }
    stamp = (stamp + 1) % 1000000UL;
  }
  if (!uniqueName) {
    Serial.println("[SD] Could not find an unused log filename");
    return false;
  }

  _f = sdFs().open(_name, FILE_WRITE);
  if (!_f) {
    Serial.printf("[SD] open failed: %s\n", _name);
    return false;
  }
  writeHeader();
  _bufCount = 0;
  _lastFlush = millis();
  return true;
}

void CsvLogger::writeHeader() {
  if (!_f) return;
  _f.println("t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc");
  _f.flush();
}

bool CsvLogger::flushBuffer() {
  if (!_f) return false;
  if (_bufCount == 0) return true;

  for (size_t i = 0; i < _bufCount; ++i) {
    const Sample &s = _buf[i];
    // 检查单次写入结果，确保数据真正落盘（避免"只写标题不写数据"）。
    int n = _f.printf("%lu,%.1f,%.1f,%.3f,%.3f,%.1f,%.1f,%.4f,%u\n",
                      (unsigned long)s.tMs, s.powerW, s.cadRpm, s.vgMps,
                      s.vAirMps, s.yawDeg, s.postureDeg, s.rho,
                      (unsigned)s.powerSrc);
    if (n <= 0) {
      Serial.printf("[SD] write failed at buffer index %u\n", (unsigned)i);
      // 保留未写入的样本，下次重试；已写入的前 i 条丢弃。
      memmove(_buf, _buf + i, (size_t)(_bufCount - i) * sizeof(Sample));
      _bufCount -= i;
      _f.flush();
      return false;
    }
  }
  _f.flush();
  _bufCount = 0;
  _lastFlush = millis();
  return true;
}

void CsvLogger::setLocalDate(const char *dateYmd) {
  const char *d = dateYmd ? dateYmd : "";
  if (strncmp(_date, d, sizeof(_date)) == 0) return;   // 未变化
  snprintf(_date, sizeof(_date), "%s", d);
  // 若正在记录且日期变化，切换到新目录/新文件（未开始的记录会在 start() 时使用新日期）。
  if (_ok) {
    if (!lockSdCard(pdMS_TO_TICKS(500))) return;
    openFileForDate();
    unlockSdCard();
    if (_f) Serial.printf("[SD] date changed -> %s\n", _name);
  }
}

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
  // 记录功能默认不开启；仅准备好 SD 与 /LOG 目录。
  unlockSdCard();
  return true;
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

  _bufCount = 0;
  if (!openFileForDate()) {
    _ok = false;
    unlockSdCard();
    return false;
  }
  _ok = true;
  unlockSdCard();
  Serial.printf("Logging to %s (buffered, 1-min chunks)\n", _name);
  return _ok;
}

bool CsvLogger::stop() {
  if (!_ok) return true;
  if (!lockSdCard()) {
    Serial.println("[SD] Could not acquire SD mutex to stop logging");
    return false;
  }
  flushBuffer();          // 写入剩余缓存，确保不丢尾部数据
  if (_f) {
    _f.flush();
    _f.close();
  }
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
  if (!g_sdMounted) return 0;   // 未挂载：立即返回，避免在空挂载点上卡住

  if (!lockSdCard(pdMS_TO_TICKS(500))) return 0;

  DIR *dir = opendir(kSdMountPoint);
  if (!dir) {
    Serial.printf("[SD] opendir(%s) failed\n", kSdMountPoint);
    unlockSdCard();
    return 0;
  }

  // 先把条目读入临时数组（音频优先排序由调用方/两遍实现完成）。
  // 注意：使用 POSIX readdir/stat 而非 Arduino File::openNextFile，
  // 后者在 FATFS 上遇到 DT_UNKNOWN 会递归并在某些卡上重复返回同一条目，
  // 导致"进入 SD 文件页卡死"。这里额外加硬性计数上限，确保绝不无限循环。
  size_t count = 0;
  static const int kMaxEntries = 400;   // 防御性上限
  int guard = 0;
  struct dirent *de;
  while ((de = readdir(dir)) != nullptr && guard++ < kMaxEntries &&
         count < maxCount) {
    const char *name = de->d_name;
    if (!name || name[0] == '.') continue;   // 跳过 . .. 及隐藏文件

    char full[300];
    snprintf(full, sizeof(full), "%s/%s", kSdMountPoint, name);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) continue;       // 跳过目录（含 /LOG）

    const bool audioFile = isAudioName(name);
    // 两遍收集：先把音频放入前部，等等——这里单遍收集，稍后由排序保证音频在前。
    snprintf(out[count].name, sizeof(out[count].name), "%s", name);
    out[count].size = (uint32_t)st.st_size;
    out[count].isAudio = audioFile;
    ++count;
  }
  closedir(dir);

  // 音频文件排在前面（稳定：保持各自相对顺序）。
  for (size_t i = 1; i < count; ++i) {
    SdFileEntry key = out[i];
    size_t j = i;
    while (j > 0 && out[j - 1].isAudio < key.isAudio) {
      out[j] = out[j - 1];
      --j;
    }
    out[j] = key;
  }

  unlockSdCard();
  Serial.printf("[SD] listed %u file(s) under %s\n", (unsigned)count, kSdMountPoint);
  return count;
}

void CsvLogger::log(uint32_t tMs, float powerW, float cadRpm, float vgMps,
                    float vAirMps, float yawDeg, float postureDeg, float rho,
                    uint8_t powerSrc) {
  if (!_ok) return;

  // 先缓存到内存（1Hz，60 条=1 分钟），不每秒写 SD。
  if (_bufCount >= kMaxSamples) {
    // 缓冲已满：尝试一次性写入。若暂停中/卡忙，暂不写并丢弃本样本，避免无限增长。
    if (!_paused && lockSdCard(pdMS_TO_TICKS(200))) {
      flushBuffer();
      unlockSdCard();
    }
    if (_bufCount >= kMaxSamples) {
      // 仍满（暂停或写失败）→ 丢弃本样本，保留已有缓存。
      return;
    }
  }

  Sample &s = _buf[_bufCount++];
  s.tMs = tMs;
  s.powerW = powerW;
  s.cadRpm = cadRpm;
  s.vgMps = vgMps;
  s.vAirMps = vAirMps;
  s.yawDeg = yawDeg;
  s.postureDeg = postureDeg;
  s.rho = rho;
  s.powerSrc = powerSrc;

  // 缓冲刚好满 60 条 → 立即写入这一分钟的数据。
  if (_bufCount >= kMaxSamples && !_paused && lockSdCard(pdMS_TO_TICKS(200))) {
    flushBuffer();
    unlockSdCard();
  }
}
