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
#include "usb_sd_storage.h"
#include "board_imu.h"
#include "imu_comparison_logger.h"
#if WIFI_WEB_ENABLE
#include "wifi_web.h"
#endif

extern "C" bool tud_msc_is_writable_cb(uint8_t) { return true; }

FS3000       windSensor;
BlePowerMeter powerMeter;
BlePosture   posture;
BleHeartRate heartRate;
BleManager   bleManager(powerMeter, posture, &heartRate);
StartupConnections startupConnections;
StartupConnections scanFavoriteConnections;
UsbSdStorage usbSdStorage;
EnvSensor    env;
CsvLogger    logger;
RecordingJournal recordingJournal;
RideRecorder recorder(logger, fitExport, recordingJournal);
DisplayLcd   lcd;
PowerKey     powerKey;
BoardImu boardImu;
ImuComparison imuComparison;
ImuComparisonLogger imuLogger;
RiderPosture riderPosture;
static RiderPosture::Estimate riderPostureEstimate;
static uint32_t lastPostureEstimateMs = 0;
static uint32_t lastImuRecordMs = 0;

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
static bool     powerConnected = false;
static bool     postureConnected = false;
static bool     manualPairingActive = false;   // 用户正在手动挑选/连接设备
static SavedConnections::Role manualScanRole = SavedConnections::Role::Power;
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

static uint32_t networkTimeUtc() {
#if WIFI_WEB_ENABLE
  return wifiWeb.unixSec();
#else
  return 0;
#endif
}

static bool networkConnected() {
#if WIFI_WEB_ENABLE
  return wifiWeb.connected();
#else
  return false;
#endif
}

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
  imuComparison = ImuComparison{};
  if (!imuLogger.start(esp_random())) AppLog.println("[IMU] could not start comparison recording");
  win = Window();
  windWindowSamples = 0;
  lastLogMs = lastSampleMs = millis();
}

static void cancelStartupConnections(bool stopWifiAttempt = false) {
  if (startupConnections.waiting() || scanFavoriteConnections.waiting()) bleManager.cancel();
  startupConnections.cancel();
  scanFavoriteConnections.cancel();
#if WIFI_WEB_ENABLE
  if (stopWifiAttempt && wifiWeb.connecting()) wifiWeb.setEnabled(false);
#else
  (void)stopWifiAttempt;
#endif
}

static void changeUsbStorageMode(bool enabled) {
  if (enabled) {
    if (!recorder.safeToRestartForUsb() || fitExport.busy()) {
      lcd.showUsbStorageError("Stop recording and finish SAVE FIT before enabling USB.");
      return;
    }
    if (logger.ok() && !logger.stop()) {
      lcd.showUsbStorageError("SD sync failed. USB mode unchanged.");
      return;
    }
  } else {
    const bool hadMedium = usbSdStorage.ready();
    usbSdStorage.eject();
    if (hadMedium && !lockSdCard(pdMS_TO_TICKS(2000))) {
      lcd.showUsbStorageError("USB read still active. Retry OFF.");
      return;
    }
    if (hadMedium) unlockSdCard();
  }
  if (!lcd.saveDashboardSettings()) return;
  imuLogger.requestStop();
  if (imuLogger.busy()) {
    lcd.showUsbStorageError("IMU recording is finishing. Retry USB mode.");
    return;
  }
  if (!UsbSdSetting::save(enabled)) {
    lcd.showUsbStorageError("Could not save USB mode. Retry.");
    return;
  }
  AppLog.printf("[USB-SD] manual mode %s; restarting with exclusive SD ownership\n", enabled ? "ON" : "OFF");
  ESP.restart();
}

void setup() {
  // 电源开关管理：尽早初始化以锁定供电（开机需按电池开关）
  powerKey.begin();

  Serial.begin(115200);
  Serial.enableReboot(true);
  if (!AppLog.begin()) Serial.println("[LOG] output task unavailable");
  delay(200);
  AppLog.println("\n== Aero Probe (single FS3000 + touch UI + BLE PM) ==");
  AppLog.println("[PM] Use SELECT POWER METER to choose BLE, virtual, or automatic input.");
  AppLog.println("[PM] BLE measurements are streamed to this monitor.");
  AppLog.printf("[MEM] PSRAM=%u internal free=%u largest=%u\n",
                (unsigned)ESP.getPsramSize(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  pinMode(PIN_BOOT_KEY, INPUT_PULLUP);
  if (usbSdStorage.beginAtBoot()) {
    lcd.begin();
    lcd.showUsbStorage(usbSdStorage.ready(), usbSdStorage.ejected(), usbSdStorage.hostConnected());
    return;
  }

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
  {
    SavedConnections saved;
    if (!saved.seedDevelopmentFavorite())
      AppLog.println("[BLE] WT901BLE67 development favorite unavailable; existing favorites preserved");
  }
  lcd.begin();                       // 屏幕
  AppLog.printf("[IMU] QMI8658 %s addr=0x6B bus=GPIO11/10\n", boardImu.begin(Wire) ? "ready" : "unavailable");
  if (!imuLogger.begin()) AppLog.println("[IMU] comparison worker unavailable");

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
  if (usbSdStorage.exclusive()) {
    static bool usbShutdownPending = false;
    bool enabled;
    if (lcd.takeUsbStorageRequest(enabled)) changeUsbStorageMode(enabled);
    powerKey.tick();
    if (powerKey.takeShutdownRequest()) usbShutdownPending = true;
    if (usbShutdownPending && lcd.saveDashboardSettings()) {
      usbSdStorage.eject();
      powerKey.shutdown();
    }
    if (!powerKey.shuttingDown()) {
      lcd.showUsbStorage(usbSdStorage.ready(), usbSdStorage.ejected(), usbSdStorage.hostConnected());
      lcd.tick();
    }
    delay(5);
    return;
  }
  bleManager.tick();

  // 电源开关：长按 5 秒关机（非阻塞）
  static bool shutdownPending = false;
  if (!shutdownPending) powerKey.tick();
  if (powerKey.takeShutdownRequest() && !shutdownPending) {
    shutdownPending = true;
    imuLogger.requestStop();
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
    if (imuLogger.busy()) {
      lcd.tick();
      delay(1);
      return;
    }
    if (!lcd.saveDashboardSettings()) {
      lcd.tick();
      delay(1);
      return;
    }
    audio.playShutdownChime();
    powerKey.shutdown();
    return;
  }
  if (powerKey.shuttingDown()) return;
  bool usbEnabled;
  if (!shutdownPending && lcd.takeUsbStorageRequest(usbEnabled)) changeUsbStorageMode(usbEnabled);
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
#if WIFI_WEB_ENABLE
  if (!recorder.busy() && wifiWeb.unixSec() != 0)
    logger.setNetworkUtcReference(wifiWeb.unixSec(), now);
#endif
  // 无网络时间时使用配置的固定 UTC 兜底，满足离线 FIT 导出。
  // 样本时间轴来自 millis()，相对时间正确，仅绝对日期不准。
  // 放在 NTP 之后，确保真实授时优先（useFallback 不会覆盖已存在的 NTP 时间）。
  if (!recorder.busy() && logger.ok() && FIT_DEFAULT_UTC != 0)
    logger.useFallbackUtcReference(FIT_DEFAULT_UTC, now);
  if (lcd.takeFitSaveRequest()) {
    recorder.requestSave(now);
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
      imuLogger.requestStop();
      recorder.requestStop(now);
    }
    lcd.setRecording(recorder.recording(), recorder.waiting());
  }

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
      imuLogger.requestStop();
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

  boardImu.poll(millis());
  const uint32_t postureNow = millis();
  riderPosture.setPlacement(lcd.posturePlacement());
  bool calibrateLean = false;
  if (lcd.takePostureCalibrationRequest(calibrateLean)) {
    const bool calibrated = riderPosture.calibrate(calibrateLean, postureNow,
        boardImu.sample(postureNow), posture.imuSample(postureNow));
    lcd.showPostureCalibrationResult(calibrated);
    AppLog.printf("[POSTURE] calibration=%s success=%u wear=%u\n",
                  calibrateLean ? "lean" : "upright", unsigned(calibrated), unsigned(lcd.posturePlacement()));
  }
  if (uint32_t(postureNow - lastPostureEstimateMs) >= 100) {
    lastPostureEstimateMs = postureNow;
    riderPostureEstimate = riderPosture.update(postureNow,
        boardImu.sample(postureNow), posture.imuSample(postureNow));
  }
  if (!recorder.waiting() && !recorder.recording()) imuLogger.requestStop();
  if (imuLogger.accepting() && uint32_t(millis() - lastImuRecordMs) >= 100) {
    lastImuRecordMs = millis();
    const auto record = imuComparison.capture(lastImuRecordMs,
        boardImu.sample(lastImuRecordMs), posture.imuSample(lastImuRecordMs),
        posture.rollDeg(), posture.pitchDeg(), posture.yawDeg());
    imuLogger.log(record);
  }

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
  lcd.updateHeartRate(heartRate.bpm(), heartRate.valid(now));
  const bool windResponding = lastWindValidMs != 0 &&
      now - lastWindValidMs <= 2000;
  const uint32_t sensorUiMs = millis();
  const ImuSample boardUi = boardImu.sample(sensorUiMs);
  const ImuSample wearableUi = posture.imuSample(sensorUiMs);
  const uint32_t sensorNetworkUtc = networkTimeUtc();
  lcd.updateOnlineDevices(powerMeter.connected(), powerMeter.measurementValid(now),
                          heartRate.connected(), heartRate.valid(now),
                          posture.connected(), wearableUi.valid,
                          networkConnected(), sensorNetworkUtc != 0,
                          windSensor.present(), windResponding);
  lcd.updateHeartDeviceStatus(heartRate.connected(), heartRate.valid(now),
                              heartRate.bpm());
  lcd.updatePostureDeviceStatus(posture.connected(), wearableUi.valid,
                                posture.rollDeg(), posture.pitchDeg(),
                                posture.yawDeg());
  char recentData[128];
  if (powerMeter.measurementValid(sensorUiMs)) {
    snprintf(recentData, sizeof(recentData), "%.0fW %.0frpm",
             powerMeter.power(), powerMeter.cadence());
    lcd.updateRecentSensor(0, recentData, powerMeter.measurementMs());
  }
  if (heartRate.valid(sensorUiMs)) {
    snprintf(recentData, sizeof(recentData), "%u bpm", unsigned(heartRate.bpm()));
    lcd.updateRecentSensor(1, recentData, heartRate.receivedMs());
  }
  for (unsigned index = 0; index < 2; ++index) {
    const ImuSample &sample = index ? boardUi : wearableUi;
    if (!sample.valid) continue;
    if (index)
      snprintf(recentData, sizeof(recentData), "A %+.2f %+.2f %+.2f g",
               sample.accelerationG[0], sample.accelerationG[1], sample.accelerationG[2]);
    else
      snprintf(recentData, sizeof(recentData), "P%+.0f R%+.0f Y%+.0f",
               posture.pitchDeg(), posture.rollDeg(), posture.yawDeg());
    lcd.updateRecentSensor(index ? 5 : 2, recentData, sample.receivedMs);
  }
  if (sensorNetworkUtc) {
    time_t epoch = time_t(sensorNetworkUtc);
    struct tm utc;
    if (gmtime_r(&epoch, &utc)) {
      strftime(recentData, sizeof(recentData), "%H:%M:%S UTC", &utc);
      lcd.updateRecentSensor(3, recentData, sensorUiMs);
    }
  } else {
    lcd.updateRecentSensor(3, "UTC --", sensorUiMs);
  }
  if (windResponding) {
    snprintf(recentData, sizeof(recentData), "%.2fm/s RAW%u",
             wv.vAir, unsigned(windSensor.lastRaw()));
    lcd.updateRecentSensor(4, recentData, lastWindValidMs);
  }
  lcd.refreshRecentSensors(sensorUiMs, boardImu.ready(), boardUi.valid);
  lcd.updatePostureEstimate(riderPostureEstimate);
#if WIFI_WEB_ENABLE
    wifiWeb.broadcastTelemetry(now, power, pmCadence(), vg, vAir, windValid,
                 yaw, post, rho, src, recorder.recording(), lcd.estimatedCda(),
                 sensorNetworkUtc, recorder.waiting(),
                 recorder.busy(), recorder.duration(now), recorder.skipped(), heartRate.bpm(), heartRate.valid(now));
#endif

    const char *sourceName = src == 3 ? "VIRTUAL" : (src == 1 ? "BLE" : "--");
    const uint8_t *frame = windSensor.lastFrame();
    AppLog.printf("%lu P=%5.0fW(%s) cad=%4.0f vg=%4.2f vAir=%4.2f yaw=%+5.1f pos=%+5.1f rho=%.3f windSamples=%lu fs=%s err=%s short=%lu checksum=%lu bytes=%u raw=%02X%02X%02X%02X%02X sd=%s ntp=%u utc=%lu\n",
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
                  unsigned(sensorNetworkUtc != 0), (unsigned long)sensorNetworkUtc);

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

  // Network time is maintained by SNTP; refresh the display and date once per second.
  {
    static uint32_t lastTimeUiMs = 0;
    if (now - lastTimeUiMs >= LOG_WINDOW_MS) {
      lastTimeUiMs = now;
      const uint32_t clockUtc = networkTimeUtc();
      lcd.updateNetworkTime(networkConnected(), clockUtc);
      lcd.updateTimeIndicator(clockUtc != 0);
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
  // BLE commands are serialized by the worker and rejected when the stack is unavailable.
  const bool bleAvailable = bleManager.available();

  if (lcd.takeBleCancelRequest()) {
    scanFavoriteConnections.cancel();
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

  if (lcd.takeBleScanRequest(&manualScanRole)) {
    scanFavoriteConnections.cancel();
    cancelStartupConnections(true);
    manualPairingActive = true;
    if (manualScanRole == SavedConnections::Role::Power) powerMode = 1;
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
  bool selectedHeart = false;
  if (lcd.takeBleConnectRequest(selectedAddress, sizeof(selectedAddress),
                                &selectedAddressType, &selectedPosture, &selectedHeart)) {
    scanFavoriteConnections.cancel();
    cancelStartupConnections(true);
    AppLog.printf("[BLE] user confirmed connection to %s (addressType=%u)\n",
                  selectedAddress, (unsigned)selectedAddressType);
    if (!bleAvailable) {
      manualPairingActive = false;
      lcd.showBleConnectFailure(
          "BLE is not ready. Wait a moment and try again.");
    } else if (!enqueueBleCommand(selectedHeart ? BleOpKind::Heart : selectedPosture ? BleOpKind::Posture : BleOpKind::Connect, selectedAddress,
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
    if (outcome.success && !outcome.cancelled) {
      if (outcome.kind == BleOpKind::Connect)
        lcd.setConnectedBle(outcome.address, outcome.addressType,
                            powerMeter.connected(), SavedConnections::Role::Power);
      else if (outcome.kind == BleOpKind::Heart)
        lcd.setConnectedBle(outcome.address, outcome.addressType,
                            heartRate.connected(), SavedConnections::Role::Heart);
      else if (outcome.kind == BleOpKind::Posture)
        lcd.setConnectedBle(outcome.address, outcome.addressType,
                            posture.connected(), SavedConnections::Role::Posture);
    }
    if (scanFavoriteConnections.waiting()) scanFavoriteConnections.complete();
    if (startupConnections.waiting()) {
      startupConnections.complete();
      powerConnected = powerMeter.connected();
      postureConnected = posture.connected();
      AppLog.printf("[STARTUP] BLE %s: %s\n", outcome.address,
                    outcome.success ? "connected and saved" : "not available; continuing list");
    } else if (outcome.kind == BleOpKind::Heart) {
      manualPairingActive = false;
      if (outcome.success) lcd.closePowerMeterPanels();
      else if (outcome.cancelled) lcd.finishBleCancelled();
      else lcd.showBleConnectFailure(outcome.error);
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
        if (manualScanRole == SavedConnections::Role::Power)
          scanFavoriteConnections.beginScan(outcome.devices, outcome.deviceCount);
        else
          scanFavoriteConnections.cancel();
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
    if (!powerMeter.connected()) lcd.updateBleFavoriteAvailability(false);
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
                                     posture.connected(), entry, heartRate.connected())) {
      AppLog.printf("[STARTUP] BLE %u/%u: %s\n", startupConnections.attemptedIndex(),
                    startupConnections.count(), entry.address);
      if (!enqueueBleCommand(entry.role == SavedConnections::Role::Power
                                ? BleOpKind::Connect : entry.role == SavedConnections::Role::Heart ? BleOpKind::Heart : BleOpKind::Posture,
                             entry.address, entry.addressType)) {
        startupConnections.complete();
        AppLog.println("[STARTUP] BLE attempt unavailable; waiting for next saved entry/manual setup");
      }
    }
  }
  if (bleAvailable && !recorder.busy()) {
    SavedConnections::Ble entry;
    if (scanFavoriteConnections.takeNext(wifiBusy, bleManager.busy(), powerMeter.connected(),
                                         posture.connected(), entry)) {
      if (enqueueBleCommand(BleOpKind::Connect, entry.address, entry.addressType)) {
        manualPairingActive = true;
        lcd.showBleLoading("Connecting to favorite...");
      } else {
        scanFavoriteConnections.complete();
      }
    }
  }
}
