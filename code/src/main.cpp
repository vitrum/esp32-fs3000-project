// ============================================================================
// ESP32-S3-Touch-LCD-2.8 骑行气动测试固件
//   原型阶段单 FS3000-1015 风速传感器
//   + BLE 功率计（功率/踏频/车速）
//   + BLE 姿态传感器 WT9011DCL-BT50（躯干俯仰角 posture，无线）
//   + BME280 可选（空气密度 ρ）
//   + ST7789 屏显示 + TF 卡 CSV 日志（1s 一行）
//
// 解算（后处理 tools/cda_postprocess.py）：
//   CdA = (P − P_rr − P_g − P_a) / (0.5·ρ·v_a²·v_g·cosφ)
//   姿态分箱：按 postureDeg 分组对比 CdA（气动姿势 vs 直立姿势）
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <cstring>
#include <errno.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "config.h"
#include "runtime_log.h"
#include "ble_diagnostics.h"
#include "fit_export.h"
#include "ride_recorder.h"
#include "fs3000.h"
#include "wind_probe.h"
#include "ble_power.h"
#include "ble_posture.h"
#include "ble_manager.h"
#include "startup_connections.h"
#include "bme280_env.h"
#include "csv_logger.h"
#include "display_lcd.h"
#include "power_key.h"
#include "audio.h"
#include "gps_time.h"
#if WIFI_WEB_ENABLE
#include "wifi_web.h"
#endif

FS3000       windSensor;
BlePowerMeter powerMeter;
BlePosture   posture;
BleManager   bleManager(powerMeter, posture);
StartupConnections startupConnections;
EnvSensor    env;
CsvLogger    logger;
RecordingJournal recordingJournal;
RideRecorder recorder(logger, fitExport, recordingJournal);
DisplayLcd   lcd;
PowerKey     powerKey;
GpsTime      gps;

// ---- 1s 窗口统计 ----
struct Window {
  uint32_t n = 0;
  uint32_t powerSamples = 0;
  float sumPower = 0;
  float sumVg    = 0;
  float sumVAir  = 0;
  float sumYaw   = 0;
  float sumPosture = 0;
};
Window win;

static uint32_t lastSampleMs = 0;
static uint32_t lastLogMs    = 0;
static uint32_t lastWindProbeMs = 0;
static uint32_t lastWindValidMs = 0;
static uint32_t windWindowSamples = 0;
#if WIFI_WEB_ENABLE
static uint32_t lastWebMs    = 0;
#endif
static uint32_t lastBlePowerNotification = 0;
static uint32_t lastBleFtmsNotification = 0;
static uint32_t lastBlePostureNotification = 0;
static uint32_t lastBleDiagnosticsMs = 0;
static uint32_t lastBleDisconnectCount = 0;
static uint32_t lastBlePostureAttemptMs = 0;
static bool     postAudioBleCatchUpNeeded = false;
static bool     powerConnected = false;
static bool     postureConnected = false;
static bool     manualPairingActive = false;   // 用户正在手动挑选/连接设备
static bool     wirelessSuspendedForAudio = false;
static int      powerMode = 0;      // 0=AUTO 1=BLE 3=VIRTUAL; 2 is reserved.
static WindVector wv;            // 最近一次探针读数（供显示）

static bool beginBleWorker() {
  return bleManager.begin();
}

static bool enqueueBleCommand(BleOpKind kind, const char *address = nullptr,
                              uint8_t addressType = 0xFF) {
  return bleManager.enqueue(kind, address, addressType);
}

static bool takeBleOutcome(BleOutcome &out) {
  return bleManager.takeOutcome(out);
}

static void requestBleCancel() {
  bleManager.cancel();
}

#if AUDIO_ENABLE
static bool suspendWirelessForAudio() {
  if (wirelessSuspendedForAudio) return true;
  if (bleManager.busy()) {
    AppLog.println("[AUDIO] wait for BLE operation to finish before playback");
    return false;
  }
  if (!bleManager.suspend()) {
    AppLog.println("[AUDIO] could not release BLE stack for playback");
    return false;
  }
  powerConnected = false;
  postureConnected = false;
  wirelessSuspendedForAudio = true;
  AppLog.printf("[AUDIO] BLE stack suspended; internal heap free=%u largest=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  return true;
}

static void resumeWirelessAfterAudio() {
  if (!audio.releasePlayback()) return;
  if (!wirelessSuspendedForAudio) return;
  if (!beginBleWorker()) {
    AppLog.println("[BLE] worker could not be restarted after audio");
    return;
  }
  wirelessSuspendedForAudio = false;
  // 播放期间姿态重连被挂起，恢复后立刻补一次，不必再等 30s 周期。
  lastBlePostureAttemptMs = millis();
  postAudioBleCatchUpNeeded = true;
  AppLog.println("[AUDIO] BLE stack resumed");
}
#endif  // AUDIO_ENABLE

// Keep historical CSV source IDs: 0=none, 1=BLE, 2=reserved, 3=virtual.
static int pmSrc() {
  if (powerMode == 3) return 3;
  return powerConnected ? 1 : 0;
}

static float pmPower() {
  if (powerMode == 3) return lcd.virtualPowerW();
  return powerMeter.power();
}
static float pmCadence() {
  if (powerMode == 3) return lcd.virtualCadenceRpm();
  return powerMeter.cadence();
}

static void armRecording() {
  if (recorder.recording() || recorder.waiting()) return;
  if (!recorder.requestStart()) return;
  win = Window();
  windWindowSamples = 0;
  lastLogMs = lastSampleMs = millis();
}

static void cancelStartupConnections(bool stopWifiAttempt = false) {
  if (startupConnections.waiting()) bleManager.cancel();
  startupConnections.cancel();
#if WIFI_WEB_ENABLE
  if (stopWifiAttempt && wifiWeb.connecting()) wifiWeb.setEnabled(false);
#else
  (void)stopWifiAttempt;
#endif
}

void setup() {
  // 电源开关管理：尽早初始化以锁定供电（开机需按电池开关）
  powerKey.begin();

  Serial.begin(115200);
  if (!AppLog.begin()) Serial.println("[LOG] output task unavailable");
  delay(200);
  AppLog.println("\n== Aero Probe (single FS3000 + touch UI + BLE PM) ==");
  AppLog.println("[PM] Use SELECT POWER METER to choose BLE, virtual, or automatic input.");
  AppLog.println("[PM] BLE measurements are streamed to this monitor.");
  AppLog.printf("[MEM] PSRAM=%u internal free=%u largest=%u\n",
                (unsigned)ESP.getPsramSize(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  // GPS UART 先启动并进行限时、非阻塞探测；不等待定位。
  // 放在可能耗时的 SD 初始化之前，避免错过模块输出的 NMEA 数据。
  gps.begin();

  // 原型阶段单风速传感器；Wire1 由 LCD 初始化为 CST328 触摸总线
  Wire.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, 400000);

  bool windOk = windSensor.begin(Wire);
  AppLog.printf("FS3000 %s addr=0x%02X bus=GPIO%d/%d clock=400kHz\n",
                windOk ? "OK" : "FAIL", FS3000_ADDR,
                PIN_I2C1_SDA, PIN_I2C1_SCL);

  env.begin(Wire);                   // 可选 BME280（挂在 I2C1）
  AppLog.printf("BME280 %s\n", env.present() ? "OK" : "absent");

  // BOOT 按钮（GPIO0）：按下为低电平，用于打开设置页。
  pinMode(PIN_BOOT_KEY, INPUT_PULLUP);

  if (!beginSdCardMutex())
    AppLog.println("[SD] Could not create SD card mutex; SD access is unavailable");
  logger.begin();                    // TF 卡（可选，失败不阻塞）
  if (!recorder.begin()) AppLog.println("[REC] Flash cache unavailable; recording disabled");
  lcd.begin();                       // 屏幕

  // 音频 + 保存的音量设置
  audio.begin();
  {
    Preferences prefs;
    uint8_t v = AUDIO_VOLUME;
    if (prefs.begin("aero-ui", true)) {
      v = prefs.getUChar("vol", AUDIO_VOLUME);
      prefs.end();
    }
    audio.setVolume(v);
    lcd.setVolume(v);
  }
  audio.playStartupChime();          // 开机提示音（两短一长）

  // BLE 设备（NimBLE 单例，两个客户端共用；init 幂等）
  // 单个串行工作线程负责所有扫描/连接；主循环只投递命令与取结果。
  if (!beginBleWorker())
    AppLog.println("[BLE] serial worker unavailable; BLE pairing disabled");
  if (!startupConnections.begin())
    AppLog.println("[STARTUP] saved connection profiles unavailable; manual setup required");
#if WIFI_WEB_ENABLE
  wifiWeb.begin(false);
#endif
  AppLog.println("[STARTUP] saved Wi-Fi first, then saved BLE devices; one bounded attempt each");
}

void loop() {
  uint32_t now = millis();
  bleManager.tick();

  // 电源开关：长按 5 秒关机（非阻塞）
  static bool shutdownPending = false;
  if (!shutdownPending) powerKey.tick();
  if (powerKey.takeShutdownRequest() && !shutdownPending) {
    shutdownPending = true;
    recorder.requestShutdown(now);
  }
  recorder.poll(now);
  lcd.setRecording(recorder.recording(), recorder.waiting());
  lcd.setFitSaving(recorder.busy());
  char recorderMessage[160];
  if (recorder.takeMessage(recorderMessage, sizeof(recorderMessage))) {
    AppLog.printf("[REC] %s\n", recorderMessage);
    if (!recorder.waiting()) lcd.showFitResult(recorderMessage);
  }
  if (recorder.readyToPowerOff()) {
    audio.playShutdownChime();
    powerKey.shutdown();
    return;
  }
  if (powerKey.shuttingDown()) return;
  if (shutdownPending) {
    if (lcd.takeFitSaveRequest()) recorder.requestSave(now);
    bool ignored;
    lcd.takeRecordingRequest(ignored);
    lcd.tick();
    delay(1);
    return;
  }

  // 音频解码/播放泵 + 状态回显
  audio.tick();
  lcd.updateAudioStatus(audio.playing(), audio.currentTitle());

  // 保持接收任务运行，以便离线模块重新接入后自动恢复。
  if (gps.enabled()) {
    gps.tick();
    gps.pollPresence();
  }
  // 回填检测结果，供 GPS 页面显示或提示未找到模块。
  lcd.setGpsModulePresent(gps.modulePresent(), gps.presenceChecked());
  if (!recorder.busy() && gps.timeValid())
    logger.setUtcReference(gps.unixSec(), now);
#if WIFI_WEB_ENABLE
  if (!recorder.busy() && wifiWeb.unixSec() != 0)
    logger.setNetworkUtcReference(wifiWeb.unixSec(), now);
#endif
  // 测试阶段兜底：拿不到 GPS 授时就用开机默认时间做 FIT 锚点。
  // 样本时间轴来自 millis()，相对时间正确，仅绝对日期不准。
  // 放在 GPS 之后，确保真实授时优先（useFallback 不会覆盖已存在的 GPS 时间）。
  if (!recorder.busy() && logger.ok() && FIT_DEFAULT_UTC != 0)
    logger.useFallbackUtcReference(FIT_DEFAULT_UTC, now);
  if (lcd.takeFitSaveRequest()) {
    if (audio.playing())
      lcd.showFitResult("Stop audio before saving FIT.");
    else recorder.requestSave(now);
  }

  // 本地"开始/停止记录"入口（日志页 RECORD/STOP 按钮）。
  bool localStartRecording = false;
  if (lcd.takeRecordingRequest(localStartRecording)) {
    if (recorder.busy()) {
      AppLog.println("[FIT] recording control rejected during export");
      lcd.showFitResult("Wait for FIT export to finish.");
    } else if (localStartRecording) {
      armRecording();
    } else {
      recorder.requestStop(now);
    }
    lcd.setRecording(recorder.recording(), recorder.waiting());
  }

  // 播放音频时暂停提交新的 CSV 批次；已排队批次仍由后台任务完成。
  // 音频整体禁用时，这段逻辑不参与，BLE 与日志互不影响。
#if AUDIO_ENABLE
  static bool audioWasPlaying = false;
  const bool audioPlaying = audio.playing();
  if (audioPlaying && !audioWasPlaying) {
    logger.pause();
    AppLog.println("[SD] audio playback started -> CSV logging paused");
  } else if (!audioPlaying && audioWasPlaying) {
    logger.resume();
    AppLog.println("[SD] audio playback ended -> CSV logging resumed");
  }
  if (!audioPlaying && wirelessSuspendedForAudio) resumeWirelessAfterAudio();
  audioWasPlaying = audioPlaying;
#endif

  // 声音设置：应用音量并用该音量试听
  uint8_t newVolume = 0;
  if (lcd.takeVolumeRequest(newVolume)) {
    audio.setVolume(newVolume);
    Preferences prefs;
    if (prefs.begin("aero-ui", false)) {
      prefs.putUChar("vol", newVolume);
      prefs.end();
    }
    AppLog.printf("[AUDIO] volume set to %u%%\n", (unsigned)newVolume);
    audio.playBeepPreview();
  }

  // SD 卡浏览器：加载文件列表 / 播放 / 停止
  char sdDirectory[128];
  if (lcd.takeSdListRequest(sdDirectory, sizeof(sdDirectory))) {
    static SdFileEntry files[12];
    int readError = 0;
    size_t count = recorder.busy() ? 0 : listSdFiles(sdDirectory, files, 12, &readError);
    if (recorder.busy()) readError = EBUSY;
    char status[64] = {};
    if (readError == EOVERFLOW)
      snprintf(status, sizeof(status), "Partial list: %u item(s)", (unsigned)count);
    else if (readError != 0)
      snprintf(status, sizeof(status), "SD read error (errno=%d)", readError);
    lcd.setSdFiles(files, count, readError != 0 ? status : nullptr);
  }
  char sdPlayPath[256] = {};
  if (lcd.takeSdPlayRequest(sdPlayPath, sizeof(sdPlayPath))) {
#if AUDIO_ENABLE
    if (recorder.busy()) {
      lcd.showFitResult("Wait for FIT export before playing audio.");
    } else {
      const bool wirelessReady = suspendWirelessForAudio();
      if (wirelessReady && audio.playFile(sdPlayPath)) {
        AppLog.printf("[SD] play mp3: %s\n", sdPlayPath);
      } else {
        AppLog.printf("[SD] could not play: %s\n", sdPlayPath);
        resumeWirelessAfterAudio();
      }
    }
#else
    // 音频已禁用：绝不要为此 suspending BLE 协议栈（曾导致扫描时提示
    // "BLE is paused while audio is playing" 的误报）。直接忽略播放请求。
    (void)sdPlayPath;
    AppLog.println("[SD] audio disabled; ignoring play request");
    lcd.showFitResult("Audio playback is disabled.");
#endif
  }
  if (lcd.takeSdStopRequest()) {
    if (audio.stop())
      AppLog.println("[SD] playback stopped");
    else
      AppLog.println("[SD] playback could not stop; SD card is busy");
  }

#if WIFI_WEB_ENABLE
  bool wifiEnabled = false;
  if (lcd.takeWifiEnableRequest(wifiEnabled)) {
    cancelStartupConnections();
    if (!wifiWeb.setEnabled(wifiEnabled))
      AppLog.println("[WiFi] Could not change Wi-Fi state");
  }
  if (lcd.takeWifiSetupRequest()) {
    cancelStartupConnections();
    wifiWeb.startSmartConfig();
  }

  // Wi-Fi / SmartConfig / Web 服务器事件泵（非阻塞）
  wifiWeb.tick();
  if (wifiWeb.provisioning()) cancelStartupConnections();
  const String wifiSsid = wifiWeb.ssid();
  const String wifiIp = wifiWeb.ip().toString();
  lcd.updateWifiStatus(wifiWeb.enabled(), wifiWeb.connected(),
                       wifiWeb.provisioning(), wifiSsid.c_str(), wifiIp.c_str());

  bool startRecording = false;
  if (wifiWeb.takeRecordingRequest(startRecording)) {
    if (recorder.busy()) {
      AppLog.println("[FIT] recording control rejected during export");
    } else if (startRecording) {
      armRecording();
    } else {
      recorder.requestStop(now);
    }
  }

  // 消费来自网页 /api/control 的控制请求
  int webSource = -1;
  if (wifiWeb.takeSourceRequest(webSource) &&
      (webSource == 0 || webSource == 1 || webSource == 3)) {
    powerMode = webSource;
    AppLog.printf("[WEB] power source -> %d\n", powerMode);
  }
#endif

  // 1) 探针采样（8Hz，受 125ms 响应限制）
  if (now - lastSampleMs >= PROBE_SAMPLE_MS) {
    lastSampleMs = now;
    float windMps = 0.0f;
    if (windSensor.readMps(windMps)) {
      wv = WindVector();
      wv.vAlong = windMps;
      wv.vAir = windMps;
      lastWindValidMs = now;
      ++windWindowSamples;
      win.n++;
      if (powerMode == 3 || powerMeter.measurementValid(now)) ++win.powerSamples;
      win.sumPower   += pmPower();
      win.sumVg      += powerMeter.speed();
      win.sumVAir    += wv.vAir;
      win.sumYaw     += wv.yawDeg;
      win.sumPosture += posture.pitchDeg();
    }
  }

  // Log and refresh on schedule even if the wind sensor misses a sample window.
  if (now - lastLogMs >= LOG_WINDOW_MS) {
    lastLogMs = now;

    float power   = win.n > 0 ? win.sumPower / (float)win.n : pmPower();
    float vg      = win.n > 0 ? win.sumVg / (float)win.n : powerMeter.speed();
    float vAir    = win.n > 0 ? win.sumVAir / (float)win.n : wv.vAir;
    float yaw     = win.n > 0 ? win.sumYaw / (float)win.n : wv.yawDeg;
    float post    = win.n > 0 ? win.sumPosture / (float)win.n : posture.pitchDeg();
    float rho     = env.rhoKgM3();
    int   src     = pmSrc();

#if GPS_USE_SPEED_FOR_VG
    // 有 GPS 定位时用 GPS 地速替代功率计轮速（多数功率计不发轮速，
    // 此时原 vg=0 会让 CdA 失效；GPS 地速更可靠）。
    if (gps.hasFix()) {
      vg = gps.speedMps();
    }
#endif

    const bool windValid = lastWindValidMs != 0 &&
                           now - lastWindValidMs <= 2000;
    if (!recorder.busy()) {
      const bool powerValid = win.n > 0 && win.powerSamples == win.n &&
                              (powerMode == 3 || powerMeter.measurementValid(now));
      FitSample sample{now, power, pmCadence(), vg, vAir, yaw, post, rho, (unsigned)src};
      recorder.accept(sample, powerValid, windValid && win.n > 0,
                      recorder.waiting() ? (esp_random() | 1u) : 1u);
      lcd.setRecording(recorder.recording(), recorder.waiting());
      lcd.updateRideLog(logger.fileName(), recorder.recording(), logger.sampleCount(),
                        logger.droppedCount(), power, pmCadence(), vg, recorder.duration(now), recorder.skipped());
    }

  lcd.update(wv, windValid, pmPower(), pmCadence(), powerMeter.speed(),
         posture.pitchDeg(), env.rhoKgM3(), pmSrc(), recorder.recording());
#if WIFI_WEB_ENABLE
    wifiWeb.broadcastTelemetry(now, power, pmCadence(), vg, vAir, windValid,
                 yaw, post, rho, src, recorder.recording(), lcd.estimatedCda(),
                 gps.timeValid() ? gps.unixSec() : wifiWeb.unixSec(), recorder.waiting(),
                 recorder.busy(), recorder.duration(now), recorder.skipped());
#endif

    const char *sourceName = src == 3 ? "VIRTUAL" : (src == 1 ? "BLE" : "--");
    const uint8_t *frame = windSensor.lastFrame();
    AppLog.printf("%lu P=%5.0fW(%s) cad=%4.0f vg=%4.2f vAir=%4.2f yaw=%+5.1f pos=%+5.1f rho=%.3f windSamples=%lu fs=%s err=%s short=%lu checksum=%lu bytes=%u raw=%02X%02X%02X%02X%02X sd=%s gps=%s fix=%u sat=%u hdop=%u lat=%.5f lon=%.5f gspd=%.2f utc=%lu\n",
                  (unsigned long)now, power,
                  sourceName,
                  pmCadence(), vg, vAir, yaw, post, rho,
                  (unsigned long)windWindowSamples,
                  windSensor.present() ? "ACK" : "NO_ACK",
                  windSensor.lastErrorName(),
                  (unsigned long)windSensor.shortReads(),
                  (unsigned long)windSensor.checksumErrors(),
                  windSensor.lastBytesReceived(), frame[0], frame[1], frame[2],
                  frame[3], frame[4], logger.ok() ? "OK" : "OFF",
                  gps.hasFix() ? "FIX" : (gps.modulePresent() ? "NOFIX" : "OFF"),
                  (unsigned)gps.hasFix(),
                  (unsigned)gps.satellites(), (unsigned)gps.hdop(),
                  gps.latitude(), gps.longitude(), gps.speedMps(),
                  (unsigned long)gps.unixSec());

    win = Window();
    windWindowSamples = 0;
  }

  if (!windSensor.present() && now - lastWindProbeMs >= 5000) {
    lastWindProbeMs = now;
    if (windSensor.begin(Wire))
      AppLog.println("[FS3000] Sensor recovered on I2C bus");
    else
      AppLog.printf("[FS3000] Not found at 0x%02X on GPIO%d/%d; retrying\n",
                    FS3000_ADDR, PIN_I2C1_SDA, PIN_I2C1_SCL);
  }

  lcd.tick();

  // BOOT 按钮（GPIO0）：检测按下沿（消抖），打开设置页。
  {
    static bool bootWasPressed = false;
    static uint32_t bootLastChangeMs = 0;
    const bool bootPressed = (digitalRead(PIN_BOOT_KEY) == LOW);
    if (bootPressed != bootWasPressed && now - bootLastChangeMs >= 30) {
      bootLastChangeMs = now;
      bootWasPressed = bootPressed;
      if (bootPressed) {
        lcd.showSettings();
        AppLog.println("[BOOT] settings opened");
      }
    }
  }

  // GPS 手动重启请求（来自 GPS 页面按钮）
  if (lcd.takeGpsRestartRequest()) {
    gps.restart();
  }

  // GPS 状态回填到 GPS 页面（1Hz）
  {
    static uint32_t lastGpsUiMs = 0;
    if (now - lastGpsUiMs >= LOG_WINDOW_MS) {
      lastGpsUiMs = now;
      char utcStr[24] = {};
      char localStr[24] = {};
      gps.formatDateTime(utcStr, sizeof(utcStr));
      // 本地时间：固定 UTC+8（中国标准时间）。如需可调，改为配置项。
      gps.formatLocalDateTime(localStr, sizeof(localStr), 8 * 60);
      // 把本地日期（"YYYY-MM-DD"）交给日志器：有定位时按日期建子目录。
      // localStr 形如 "YYYY-MM-DD HH:MM:SS"（无定位时为 "--"）。
      const bool gpsOnline = gps.enabled() &&
                 (!gps.presenceChecked() || gps.modulePresent());
      lcd.updateGpsStatus(gpsOnline, gps.hasFix(), gps.satellites(),
                          gps.hdop(), gps.latitude(), gps.longitude(),
                          gps.altitudeM(), gps.speedMps(), utcStr, localStr,
                          gps.fixAgeMs());
      // 主界面顶部 GPS 状态图标：探测期间显示搜星中，未找到后显示关闭态。
      lcd.updateGpsIndicator(gpsOnline,
                             gps.hasFix(), gps.satellites());
      uint32_t clockUtc = gps.timeValid() ? gps.unixSec() : 0;
    #if WIFI_WEB_ENABLE
      if (clockUtc == 0) clockUtc = wifiWeb.unixSec();
    #endif
      lcd.updateClock(clockUtc);
      if (!recorder.busy() && clockUtc != 0) {
        const time_t localTime = (time_t)((uint64_t)clockUtc + 8 * 3600);
        struct tm date;
        if (gmtime_r(&localTime, &date)) {
          char ymd[12];
          strftime(ymd, sizeof(ymd), "%Y-%m-%d", &date);
          logger.setLocalDate(ymd);
        }
      }
    }
  }

  int requestedMode = -1;
  if (lcd.takePowerSourceRequest(requestedMode)) {
    cancelStartupConnections();
    powerMode = requestedMode;
    AppLog.printf("[PM] selected mode: %s\n",
                  powerMode == 3 ? "VIRTUAL" :
                  (powerMode == 1 ? "BLE" : "AUTO"));
  }

  // ------------------------------------------------------------------------
  // BLE：用户请求 → 命令队列 → 工作线程执行 → 结果队列 → 界面更新
  // ------------------------------------------------------------------------
  // 音频播放期间 BLE 协议栈会被释放，此时任何 BLE 请求都直接拒绝，
  // 避免在未初始化的 NimBLE 上调用扫描/连接。
  // 注意：音频整体禁用（AUDIO_ENABLE=0）时 wirelessSuspendedForAudio 永远为
  // false，BLE 不受任何音频状态影响，这里再显式兜一层，杜绝误报。
#if AUDIO_ENABLE
  const bool bleAvailable = !wirelessSuspendedForAudio && bleManager.available();
#else
  const bool bleAvailable = bleManager.available();
#endif

  if (lcd.takeBleCancelRequest()) {
    if (bleManager.busy()) {
      requestBleCancel();
      AppLog.println("[BLE] cancellation requested; waiting for the current operation");
    } else {
      // 没有正在执行的操作：这是一次纯界面返回，直接还原界面即可。
      // 不要在这里清扫描缓存——用户按返回后可能马上再次选择同一台设备，
      // 缓存留着无害（连接本身不依赖它），清了反而多一次不必要的释放。
      manualPairingActive = false;
      AppLog.println("[BLE] device selection cancelled");
    }
  }

  if (lcd.takeBleScanRequest()) {
    cancelStartupConnections(true);
    manualPairingActive = true;
    powerMode = 1;
    if (!bleAvailable) {
      manualPairingActive = false;
      lcd.showBleConnectFailure(
          "BLE is not ready. Wait a moment and try again.");
    } else if (!enqueueBleCommand(BleOpKind::Scan)) {
      AppLog.println("[BLE] scan request dropped: worker busy");
      lcd.showBleConnectFailure(
          "The BLE module is still finishing the previous operation. "
          "Wait a moment and press SCAN AGAIN.");
    } else {
      AppLog.println("[BLE] scan queued (6 seconds)");
    }
  }

  char selectedAddress[18] = {};
  uint8_t selectedAddressType = 0xFF;
  bool selectedPosture = false;
  if (lcd.takeBleConnectRequest(selectedAddress, sizeof(selectedAddress),
                                &selectedAddressType, &selectedPosture)) {
                  cancelStartupConnections(true);
    AppLog.printf("[BLE] user confirmed connection to %s (addressType=%u)\n",
                  selectedAddress, (unsigned)selectedAddressType);
    if (!bleAvailable) {
      manualPairingActive = false;
      lcd.showBleConnectFailure(
          "BLE is not ready. Wait a moment and try again.");
    } else if (!enqueueBleCommand(selectedPosture ? BleOpKind::Posture : BleOpKind::Connect, selectedAddress,
                                  selectedAddressType)) {
      manualPairingActive = false;
      lcd.showBleConnectFailure(
          "The BLE module is still finishing the previous operation. "
          "Wait a moment and select the device again.");
    } else {
      manualPairingActive = true;
    }
  }

  BleOutcome outcome;
  if (takeBleOutcome(outcome)) {
    if (startupConnections.waiting()) {
      startupConnections.complete();
      powerConnected = powerMeter.connected();
      postureConnected = posture.connected();
      AppLog.printf("[STARTUP] BLE %s: %s\n", outcome.address,
                    outcome.success ? "connected and saved" : "not available; continuing list");
    } else if (outcome.kind == BleOpKind::Posture) {
      manualPairingActive = false;
      postureConnected = outcome.success;
      if (outcome.success) lcd.closePowerMeterPanels();
      else if (outcome.cancelled) lcd.finishBleCancelled();
      else lcd.showBleConnectFailure(outcome.error);
      AppLog.printf("[BLE] posture discovery %s\n",
                    postureConnected ? "connected"
                                     : (outcome.cancelled
                                            ? "cancelled"
                                            : "no connection; retry later"));
      if (!postureConnected && !outcome.cancelled && outcome.error[0])
        AppLog.printf("[BLE-POSTURE] %s\n", outcome.error);
    } else if (outcome.cancelled) {
      manualPairingActive = false;
      lcd.finishBleCancelled();
      AppLog.println("[BLE] operation cancelled");
    } else if (outcome.success) {
      if (outcome.kind == BleOpKind::Scan) {
        char status[48];
        snprintf(status, sizeof(status), "%u device(s) found",
                 (unsigned)outcome.deviceCount);
        lcd.setBleDevices(outcome.devices, outcome.deviceCount, status);
      } else {
        powerConnected = powerMeter.connected();
        manualPairingActive = false;
        lcd.closePowerMeterPanels();
      }
    } else {
      powerConnected = powerMeter.connected();
      if (outcome.kind == BleOpKind::Connect) manualPairingActive = false;
      lcd.showBleConnectFailure(outcome.error[0]
                                    ? outcome.error
                                    : "BLE operation failed without a reported reason.");
    }
  }

  powerConnected = powerMeter.connected();
  uint32_t bleDisconnects = powerMeter.disconnectCount();
  if (bleDisconnects != lastBleDisconnectCount) {
    lastBleDisconnectCount = bleDisconnects;
    AppLog.printf("[BLE] power meter disconnected (total %lu)\n",
                  (unsigned long)bleDisconnects);
  }

  uint32_t blePowerNotifications = powerMeter.notificationCount();
  const bool powerNotificationChanged =
      blePowerNotifications != lastBlePowerNotification;
  lastBlePowerNotification = blePowerNotifications;
  uint32_t bleFtmsNotifications = powerMeter.fitnessMachineNotificationCount();
  const bool ftmsNotificationChanged =
      bleFtmsNotifications != lastBleFtmsNotification;
  lastBleFtmsNotification = bleFtmsNotifications;
  if ((powerNotificationChanged || ftmsNotificationChanged) &&
      now - lastBleDiagnosticsMs >= 5000) {
    lastBleDiagnosticsMs = now;
    const uint16_t flags = powerMeter.lastFitnessMachineFlags();
    AppLog.printf("[BLE-DATA] P=%.0f W cad=%.0f rpm speed=%.2f m/s\n",
                  powerMeter.power(), powerMeter.cadence(), powerMeter.speed());
    AppLog.printf("[BLE-FTMS] notify=%lu flags=0x%04X powerField=%s P=%.0f W cadenceField=%s cad=%.1f rpm speedField=%s speed=%.2f m/s\n",
                  (unsigned long)bleFtmsNotifications, flags,
                  (flags & 0x0040) ? "yes" : "no", powerMeter.power(),
                  (flags & 0x0004) ? "yes" : "no", powerMeter.cadence(),
                  (flags & 0x0001) ? "no" : "yes", powerMeter.speed());
  }
  uint32_t blePostureNotifications = posture.notificationCount();
  if (blePostureNotifications != lastBlePostureNotification) {
    lastBlePostureNotification = blePostureNotifications;
    AppLog.printf("[BLE-POSTURE] pitch=%+.2f deg\n", posture.pitchDeg());
  }
  bool wifiBusy = false;
#if WIFI_WEB_ENABLE
  wifiBusy = wifiWeb.connecting() || wifiWeb.provisioning();
#endif
  if (bleAvailable && !manualPairingActive && !recorder.busy()) {
    SavedConnections::Ble entry;
    if (startupConnections.takeNext(wifiBusy, bleManager.busy(), powerMeter.connected(),
                                     posture.connected(), entry)) {
      AppLog.printf("[STARTUP] BLE %u/%u: %s\n", startupConnections.attemptedIndex(),
                    startupConnections.count(), entry.address);
      if (!enqueueBleCommand(entry.role == SavedConnections::Role::Power
                                ? BleOpKind::Connect : BleOpKind::Posture,
                             entry.address, entry.addressType)) {
        startupConnections.complete();
        AppLog.println("[STARTUP] BLE attempt unavailable; waiting for next saved entry/manual setup");
      }
    }
  }
}
