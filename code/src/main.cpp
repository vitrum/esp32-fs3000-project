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

#include "config.h"
#include "fs3000.h"
#include "wind_probe.h"
#include "ble_power.h"
#include "ble_posture.h"
#include "bme280_env.h"
#include "csv_logger.h"
#include "display_lcd.h"
#include "power_key.h"
#include "audio.h"
#include "gps_time.h"
#ifdef ANT_POWER_ENABLE
#include "ant_power.h"
#endif
#if WIFI_WEB_ENABLE
#include "wifi_web.h"
#endif

FS3000       windSensor;
BlePowerMeter powerMeter;
BlePosture   posture;
EnvSensor    env;
CsvLogger    logger;
DisplayLcd   lcd;
PowerKey     powerKey;
GpsTime      gps;
#ifdef ANT_POWER_ENABLE
AntPower     antPower;
#endif

// ---- 1s 窗口统计 ----
struct Window {
  uint32_t n = 0;
  float sumPower = 0;
  float sumVg    = 0;
  float sumVAir  = 0;
  float sumYaw   = 0;
  float sumPosture = 0;
};
Window win;

static uint32_t lastSampleMs = 0;
static uint32_t lastLogMs    = 0;
static uint32_t lastUiMs     = 0;
static uint32_t lastWindProbeMs = 0;
static uint32_t lastWindValidMs = 0;
static uint32_t windWindowSamples = 0;
static uint32_t lastBleMs    = 0;
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
static bool     manualBlePairing = false;
static bool     wirelessSuspendedForAudio = false;
static int      powerMode = 0;      // 0=AUTO 1=BLE 2=ANT+ 3=VIRTUAL
#ifdef ANT_POWER_ENABLE
static bool     antStarted = false;   // ANT 节点已启动（此后 BLE 扫描不可用）
#endif
static WindVector wv;            // 最近一次探针读数（供显示）

enum class BleOperationState : uint8_t { Idle, Running, Complete };
enum class BleOperationKind : uint8_t { Scan, Connect };

struct BleOperationResult {
  BleOperationKind kind = BleOperationKind::Scan;
  bool success = false;
  bool cancelled = false;
  size_t deviceCount = 0;
  BlePowerDevice devices[4] = {};
  char error[320] = {};
};

static volatile BleOperationState bleOperationState = BleOperationState::Idle;
static volatile bool bleOperationCancelRequested = false;
static BleOperationResult bleOperationResult;
static char bleOperationAddress[18] = {};
static TaskHandle_t bleOperationTaskHandle = nullptr;
static portMUX_TYPE bleOperationMux = portMUX_INITIALIZER_UNLOCKED;

static void bleOperationTask(void *argument) {
  const BleOperationKind kind =
      (BleOperationKind)(uintptr_t)argument;
  BleOperationResult result;
  result.kind = kind;

  if (kind == BleOperationKind::Scan) {
    result.success = powerMeter.scanDevices(result.devices, 4, result.deviceCount);
    if (!result.success) {
      snprintf(result.error, sizeof(result.error), "%s", powerMeter.lastError());
    }
  } else {
    result.success = powerMeter.connectDevice(bleOperationAddress);
    if (!result.success) {
      snprintf(result.error, sizeof(result.error), "%s", powerMeter.lastError());
    }
  }

  result.cancelled = bleOperationCancelRequested;
  if (result.cancelled && kind == BleOperationKind::Connect) {
    powerMeter.disconnect();
  }

  portENTER_CRITICAL(&bleOperationMux);
  bleOperationResult = result;
  bleOperationState = BleOperationState::Complete;
  portEXIT_CRITICAL(&bleOperationMux);
  vTaskDelete(nullptr);
}

static bool startBleOperation(BleOperationKind kind, const char *address = nullptr) {
  if (bleOperationState != BleOperationState::Idle) return false;

  if (kind == BleOperationKind::Connect) {
    snprintf(bleOperationAddress, sizeof(bleOperationAddress), "%s",
             address ? address : "");
  }
  bleOperationCancelRequested = false;
  portENTER_CRITICAL(&bleOperationMux);
  bleOperationResult = BleOperationResult();
  bleOperationState = BleOperationState::Running;
  portEXIT_CRITICAL(&bleOperationMux);

  if (xTaskCreate(bleOperationTask, "bleOperation", 8192,
                  (void *)(uintptr_t)kind, 1, &bleOperationTaskHandle) != pdPASS) {
    portENTER_CRITICAL(&bleOperationMux);
    bleOperationState = BleOperationState::Idle;
    portEXIT_CRITICAL(&bleOperationMux);
    bleOperationTaskHandle = nullptr;
    return false;
  }
  return true;
}

static bool takeBleOperationResult(BleOperationResult &result) {
  portENTER_CRITICAL(&bleOperationMux);
  if (bleOperationState != BleOperationState::Complete) {
    portEXIT_CRITICAL(&bleOperationMux);
    return false;
  }
  result = bleOperationResult;
  bleOperationState = BleOperationState::Idle;
  portEXIT_CRITICAL(&bleOperationMux);
  bleOperationTaskHandle = nullptr;
  return true;
}

static bool suspendWirelessForAudio() {
  if (wirelessSuspendedForAudio) return true;
  if (bleOperationState != BleOperationState::Idle) {
    Serial.println("[AUDIO] wait for BLE operation to finish before playback");
    return false;
  }
#ifdef ANT_POWER_ENABLE
  if (antStarted) {
    antPower.stop();
    antStarted = false;
  }
#endif
  powerMeter.disconnect();
  posture.resetAfterStackShutdown();
  if (NimBLEDevice::isInitialized() && !NimBLEDevice::deinit(true)) {
    Serial.println("[AUDIO] could not release BLE stack for playback");
    powerMeter.begin(BLE_DEVICE_NAME, BLE_WHEEL_CIRC_M);
    posture.begin(BLE_DEVICE_NAME);
    return false;
  }
  powerConnected = false;
  postureConnected = false;
  wirelessSuspendedForAudio = true;
  Serial.printf("[AUDIO] BLE stack suspended; internal heap free=%u largest=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  return true;
}

static void resumeWirelessAfterAudio() {
  audio.releasePlayback();
  if (!wirelessSuspendedForAudio) return;
  powerMeter.begin(BLE_DEVICE_NAME, BLE_WHEEL_CIRC_M);
  posture.begin(BLE_DEVICE_NAME);
  wirelessSuspendedForAudio = false;
  lastBleMs = millis();
  Serial.println("[AUDIO] BLE stack resumed");
}

// Power source IDs are written to CSV: 0=none, 1=BLE, 2=ANT+, 3=virtual.
static int pmSrc() {
  if (powerMode == 3) return 3;
#ifdef ANT_POWER_ENABLE
  if (powerMode == 2) return antPower.started() && antPower.tracking() ? 2 : 0;
  if (powerMode == 1) return powerConnected ? 1 : 0;
  if (antPower.started() && antPower.tracking()) return 2;
#endif
  return powerConnected ? 1 : 0;
}

// 有效功率/踏频（ANT 跟踪时优先用 ANT+，否则退回 BLE 功率计）
static float pmPower() {
  if (powerMode == 3) return lcd.virtualPowerW();
#ifdef ANT_POWER_ENABLE
  if (powerMode == 2) return antPower.power();
  if (powerMode == 1) return powerMeter.power();
  if (antPower.started() && antPower.tracking()) return antPower.power();
#endif
  return powerMeter.power();
}
static float pmCadence() {
  if (powerMode == 3) return lcd.virtualCadenceRpm();
#ifdef ANT_POWER_ENABLE
  if (powerMode == 2) return antPower.cadence();
  if (powerMode == 1) return powerMeter.cadence();
  if (antPower.started() && antPower.tracking()) return antPower.cadence();
#endif
  return powerMeter.cadence();
}

void setup() {
  // 电源开关管理：尽早初始化以锁定供电（开机需按电池开关）
  powerKey.begin();

  Serial.begin(115200);
  delay(200);
  Serial.println("\n== Aero Probe (single FS3000 + touch UI + BLE/ANT+ PM) ==");
  Serial.println("[PM] Use SELECT POWER METER to choose BLE, ANT+, virtual, or automatic input.");
  Serial.println("[PM] BLE/ANT+ measurements are streamed to this monitor.");

  // GPS UART 先启动并进行限时、非阻塞探测；不等待定位。
  // 放在可能耗时的 SD 初始化之前，避免错过模块输出的 NMEA 数据。
  gps.begin();

  // 原型阶段单风速传感器；Wire1 由 LCD 初始化为 CST328 触摸总线
  Wire.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, 400000);

  bool windOk = windSensor.begin(Wire);
  Serial.printf("FS3000 %s addr=0x%02X bus=GPIO%d/%d clock=400kHz\n",
                windOk ? "OK" : "FAIL", FS3000_ADDR,
                PIN_I2C1_SDA, PIN_I2C1_SCL);

  env.begin(Wire);                   // 可选 BME280（挂在 I2C1）
  Serial.printf("BME280 %s\n", env.present() ? "OK" : "absent");

  // BOOT 按钮（GPIO0）：按下为低电平，用于打开设置页。
  pinMode(PIN_BOOT_KEY, INPUT_PULLUP);

  if (!beginSdCardMutex())
    Serial.println("[SD] Could not create SD card mutex; SD access is unavailable");
  logger.begin();                    // TF 卡（可选，失败不阻塞）
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
  powerMeter.begin(BLE_DEVICE_NAME, BLE_WHEEL_CIRC_M);
  posture.begin(BLE_DEVICE_NAME);
#ifdef ANT_POWER_ENABLE
  Serial.printf("Scanning for BLE power meter & posture sensor (%lu s before ANT+ starts)...\n",
                (unsigned long)(ANT_POWER_START_DELAY_MS / 1000));
#else
  Serial.println("Scanning for BLE power meter & posture sensor...");
#endif
}

void loop() {
  uint32_t now = millis();

  // 电源开关：长按 5 秒关机（非阻塞）
  powerKey.tick();
  if (powerKey.takeShutdownRequest()) {
    static uint32_t lastLogStopAttemptMs = 0;
    const uint32_t shutdownAttemptMs = millis();
    if (logger.ok() && lastLogStopAttemptMs != 0 &&
        shutdownAttemptMs - lastLogStopAttemptMs < 1000)
      return;
    if (!logger.stop()) {
      lastLogStopAttemptMs = shutdownAttemptMs;
      Serial.println("[SD] log could not be fully synced before shutdown");
      return;
    }
    lastLogStopAttemptMs = 0;
    // 关机提示音已关闭；完成日志收尾后直接断电。
    audio.playShutdownChime();
    powerKey.shutdown();
    return;
  }
  if (powerKey.shuttingDown()) return;

  // 音频解码/播放泵 + 状态回显
  audio.tick();
  lcd.updateAudioStatus(audio.playing(), audio.currentTitle());

  // GPS 探测期间也必须读取 UART，才能判断模块是否存在。
  // 读到 NMEA 后继续后台解析；超时未收到数据则关闭 UART。
  if (gps.enabled()) {
    gps.tick();
    gps.pollPresence();
    if (gps.presenceChecked() && !gps.modulePresent()) gps.stop();
  }
  // 回填检测结果，供 GPS 页面显示或提示未找到模块。
  lcd.setGpsModulePresent(gps.modulePresent(), gps.presenceChecked());

  // 播放音频时暂停提交新的 CSV 批次；已排队批次仍由后台任务完成。
  static bool audioWasPlaying = false;
  const bool audioPlaying = audio.playing();
  if (audioPlaying && !audioWasPlaying) {
    logger.pause();
    Serial.println("[SD] audio playback started -> CSV logging paused");
  } else if (!audioPlaying && audioWasPlaying) {
    logger.resume();
    Serial.println("[SD] audio playback ended -> CSV logging resumed");
    resumeWirelessAfterAudio();
  }
  audioWasPlaying = audioPlaying;

  // 声音设置：应用音量并用该音量试听
  uint8_t newVolume = 0;
  if (lcd.takeVolumeRequest(newVolume)) {
    audio.setVolume(newVolume);
    Preferences prefs;
    if (prefs.begin("aero-ui", false)) {
      prefs.putUChar("vol", newVolume);
      prefs.end();
    }
    Serial.printf("[AUDIO] volume set to %u%%\n", (unsigned)newVolume);
    audio.playBeepPreview();
  }

  // SD 卡浏览器：加载文件列表 / 播放 / 停止
  char sdDirectory[128];
  if (lcd.takeSdListRequest(sdDirectory, sizeof(sdDirectory))) {
    static SdFileEntry files[12];
    size_t count = listSdFiles(sdDirectory, files, 12);
    lcd.setSdFiles(files, count, nullptr);
  }
  char sdPlayPath[256] = {};
  if (lcd.takeSdPlayRequest(sdPlayPath, sizeof(sdPlayPath))) {
    const bool wirelessReady = suspendWirelessForAudio();
    if (wirelessReady && audio.playFile(sdPlayPath)) {
      Serial.printf("[SD] play mp3: %s\n", sdPlayPath);
    } else {
      Serial.printf("[SD] could not play: %s\n", sdPlayPath);
      resumeWirelessAfterAudio();
    }
  }
  if (lcd.takeSdStopRequest()) {
    audio.stop();
    Serial.println("[SD] playback stopped");
  }

#if WIFI_WEB_ENABLE
  bool wifiEnabled = false;
  if (lcd.takeWifiEnableRequest(wifiEnabled)) {
#ifdef ANT_POWER_ENABLE
    if (wifiEnabled && antStarted) {
      antPower.stop();
      antStarted = false;
      Serial.println("[RF] ANT stopped to prioritize Wi-Fi/BLE coexistence");
    }
#endif
    if (!wifiWeb.setEnabled(wifiEnabled))
      Serial.println("[WiFi] Could not change Wi-Fi state");
  }

  // Wi-Fi / SmartConfig / Web 服务器事件泵（非阻塞）
  wifiWeb.tick();
  const String wifiSsid = wifiWeb.ssid();
  const String wifiIp = wifiWeb.ip().toString();
  lcd.updateWifiStatus(wifiWeb.enabled(), wifiWeb.connected(),
                       wifiWeb.provisioning(), wifiSsid.c_str(), wifiIp.c_str());

  bool startRecording = false;
  if (wifiWeb.takeRecordingRequest(startRecording)) {
    if (startRecording) {
      if (logger.start())
        Serial.println("[WEB] recording started");
      else
        Serial.println("[WEB] recording could not start; check SD card");
    } else {
      if (logger.stop())
        Serial.println("[WEB] recording stopped");
      else
        Serial.println("[WEB] recording could not stop; SD card is busy");
    }
  }

  // 消费来自网页 /api/control 的控制请求
  int webSource = -1;
  if (wifiWeb.takeSourceRequest(webSource) &&
      webSource >= 0 && webSource <= 3) {
    powerMode = webSource;
    Serial.printf("[WEB] power source -> %d\n", powerMode);
#ifdef ANT_POWER_ENABLE
    if ((powerMode == 1 || powerMode == 3) && antStarted) {
      antPower.stop();
      antStarted = false;
    }
#endif
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

    logger.log(now, power, pmCadence(), vg, vAir, yaw, post, rho, src);

#if WIFI_WEB_ENABLE
    const bool windValid = lastWindValidMs != 0 &&
                           now - lastWindValidMs <= 2000;
    wifiWeb.broadcastTelemetry(now, power, pmCadence(), vg, vAir, windValid,
                               yaw, post, rho, src, logger.ok());
#endif

    const char *sourceName = src == 3 ? "VIRTUAL" :
                             (src == 2 ? "ANT" : (src == 1 ? "BLE" : "--"));
    const uint8_t *frame = windSensor.lastFrame();
    Serial.printf("%lu P=%5.0fW(%s) cad=%4.0f vg=%4.2f vAir=%4.2f yaw=%+5.1f pos=%+5.1f rho=%.3f windSamples=%lu fs=%s err=%s short=%lu checksum=%lu bytes=%u raw=%02X%02X%02X%02X%02X sd=%s gps=%s fix=%u sat=%u hdop=%u lat=%.5f lon=%.5f gspd=%.2f utc=%lu\n",
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
                  gps.hasFix() ? "FIX" : (gps.enabled() ? "NOFIX" : "OFF"),
                  (unsigned)gps.satellites(), (unsigned)gps.hdop(),
                  gps.latitude(), gps.longitude(), gps.speedMps(),
                  (unsigned long)gps.unixSec());

    win = Window();
    windWindowSamples = 0;
  }

  if (now - lastUiMs >= LOG_WINDOW_MS) {
    lastUiMs = now;
    const bool windValid = lastWindValidMs != 0 &&
                           now - lastWindValidMs <= 2000;
    lcd.update(wv, windValid, pmPower(), pmCadence(), powerMeter.speed(),
               posture.pitchDeg(), env.rhoKgM3(), pmSrc(), logger.ok());
  }

  if (!windSensor.present() && now - lastWindProbeMs >= 5000) {
    lastWindProbeMs = now;
    if (windSensor.begin(Wire))
      Serial.println("[FS3000] Sensor recovered on I2C bus");
    else
      Serial.printf("[FS3000] Not found at 0x%02X on GPIO%d/%d; retrying\n",
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
        Serial.println("[BOOT] settings opened");
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
      if (gps.timeValid() && strlen(localStr) >= 10 && localStr[4] == '-') {
        char ymd[12];
        snprintf(ymd, sizeof(ymd), "%.10s", localStr);
        logger.setLocalDate(ymd);
      }
      lcd.updateGpsStatus(gps.enabled(), gps.hasFix(), gps.satellites(),
                          gps.hdop(), gps.latitude(), gps.longitude(),
                          gps.altitudeM(), gps.speedMps(), utcStr, localStr,
                          gps.fixAgeMs());
      // 主界面顶部 GPS 状态图标：探测期间显示搜星中，未找到后显示关闭态。
      lcd.updateGpsIndicator(gps.enabled(),
                             gps.hasFix(), gps.satellites());
    }
  }

  int requestedMode = -1;
  if (lcd.takePowerSourceRequest(requestedMode)) {
    powerMode = requestedMode;
    Serial.printf("[PM] selected mode: %s\n",
                  powerMode == 3 ? "VIRTUAL" :
                  (powerMode == 2 ? "ANT+" : (powerMode == 1 ? "BLE" : "AUTO")));
#ifdef ANT_POWER_ENABLE
    if ((powerMode == 1 || powerMode == 3) && antStarted) {
      antPower.stop();
      antStarted = false;
    }
#endif
  }

  if (lcd.takeBleCancelRequest()) {
    if (bleOperationState != BleOperationState::Idle) {
      bleOperationCancelRequested = true;
      powerMeter.cancelCurrentOperation();
      Serial.println("[BLE] cancellation requested; waiting for the current GATT operation");
    } else {
      manualBlePairing = false;
      powerMeter.clearScanResults();
      Serial.println("[BLE] device selection cancelled");
    }
  }

  if (lcd.takeBleScanRequest()) {
    manualBlePairing = true;
    powerMode = 1;
#ifdef ANT_POWER_ENABLE
    if (antStarted) {
      antPower.stop();
      antStarted = false;
    }
#endif
    Serial.println("[BLE] scanning for connectable devices (6 seconds)...");
    if (!startBleOperation(BleOperationKind::Scan)) {
      lcd.showBleConnectFailure("Could not start the BLE scan task. Close this message and try again.");
    }
  }

  char selectedAddress[18] = {};
  if (lcd.takeBleConnectRequest(selectedAddress, sizeof(selectedAddress))) {
    Serial.printf("[BLE] user confirmed connection to %s\n", selectedAddress);
    if (!startBleOperation(BleOperationKind::Connect, selectedAddress)) {
      lcd.showBleConnectFailure("Could not start the BLE connection task. Close this message and try again.");
    }
  }

  BleOperationResult operationResult;
  if (takeBleOperationResult(operationResult)) {
    if (operationResult.cancelled || bleOperationCancelRequested) {
      if (operationResult.kind == BleOperationKind::Connect) {
        powerMeter.disconnect();
      }
      powerMeter.clearScanResults();
      bleOperationCancelRequested = false;
      manualBlePairing = false;
      lcd.finishBleCancelled();
      Serial.println("[BLE] operation cancelled");
    } else if (operationResult.success) {
      if (operationResult.kind == BleOperationKind::Scan) {
        char status[48];
        snprintf(status, sizeof(status), "%u device(s) found",
                 (unsigned)operationResult.deviceCount);
        lcd.setBleDevices(operationResult.devices, operationResult.deviceCount, status);
      } else {
        powerConnected = powerMeter.connected();
        manualBlePairing = false;
        lcd.closePowerMeterPanels();
      }
    } else {
      powerConnected = powerMeter.connected();
      powerMeter.clearScanResults();
      lcd.showBleConnectFailure(operationResult.error[0]
                                    ? operationResult.error
                                    : "BLE operation failed without a reported reason.");
    }
  }

  powerConnected = powerMeter.connected();
  uint32_t bleDisconnects = powerMeter.disconnectCount();
  if (bleDisconnects != lastBleDisconnectCount) {
    lastBleDisconnectCount = bleDisconnects;
    Serial.printf("[BLE] power meter disconnected (total %lu)\n",
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
    Serial.printf("[BLE-DATA] P=%.0f W cad=%.0f rpm speed=%.2f m/s\n",
                  powerMeter.power(), powerMeter.cadence(), powerMeter.speed());
    Serial.printf("[BLE-FTMS] notify=%lu flags=0x%04X powerField=%s P=%.0f W cadenceField=%s cad=%.1f rpm speedField=%s speed=%.2f m/s\n",
                  (unsigned long)bleFtmsNotifications, flags,
                  (flags & 0x0040) ? "yes" : "no", powerMeter.power(),
                  (flags & 0x0004) ? "yes" : "no", powerMeter.cadence(),
                  (flags & 0x0001) ? "no" : "yes", powerMeter.speed());
  }
  uint32_t blePostureNotifications = posture.notificationCount();
  if (blePostureNotifications != lastBlePostureNotification) {
    lastBlePostureNotification = blePostureNotifications;
    Serial.printf("[BLE-POSTURE] pitch=%+.2f deg\n", posture.pitchDeg());
  }
#ifdef ANT_POWER_ENABLE
  static uint32_t lastAntDataCount = 0;
  uint32_t antDataCount = antPower.dataCount();
  if (antDataCount != lastAntDataCount) {
    lastAntDataCount = antDataCount;
    Serial.printf("[ANT-DATA] device=%u P=%.0f W cad=%.0f rpm\n",
                  antPower.deviceNum(), antPower.power(), antPower.cadence());
  }
#endif

  // AUTO mode starts ANT+ after its BLE posture discovery window.
#ifdef ANT_POWER_ENABLE
  const bool wifiActive =
  #if WIFI_WEB_ENABLE
      wifiWeb.enabled();
  #else
      false;
  #endif
  bool shouldStartAnt = !wifiActive && !wirelessSuspendedForAudio &&
                        (powerMode == 2 ||
                         (powerMode == 0 && now >= ANT_POWER_START_DELAY_MS));
  if (!antStarted && shouldStartAnt &&
      (powerMode == 2 || now >= ANT_POWER_START_DELAY_MS)) {
    antStarted = true;
    antPower.begin();
  }
  if (!wirelessSuspendedForAudio && !antStarted && !manualBlePairing && !postureConnected &&
      now - lastBleMs >= BLE_RECONNECT_MS) {
    lastBleMs = now;
    postureConnected = posture.tryConnect();
  }
#else
  if (!wirelessSuspendedForAudio && !manualBlePairing && !postureConnected &&
      now - lastBleMs >= BLE_RECONNECT_MS) {
    lastBleMs = now;
    postureConnected = posture.tryConnect();
  }
#endif
}
