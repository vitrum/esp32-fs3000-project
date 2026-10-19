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

// ============================================================================
// BLE 串行工作线程
// ----------------------------------------------------------------------------
// 之前的实现每来一个请求就 xTaskCreate 一个任务，两个任务（手动扫描/连接、
// 姿态发现）会同时操作 NimBLE 的全局扫描器，还会争写共享的结果结构，导致
// 「状态卡在 Running / 结果被覆盖 / 姿态发现饿死 / 界面莫名刷新」。
//
// 现在改为：一个常驻工作线程 + 一条命令队列。主循环（生产者）只负责入队和
// 取结果，工作线程（唯一消费者）独占所有 NimBLE 调用，天然串行。
//   主循环 --xQueueSend--> [cmd queue] --> bleWorkerTask --> [result queue] --> 主循环
// 取消通过一个 volatile 标志传递，工作线程在每次长操作前后检查。
// ============================================================================
enum class BleOpKind : uint8_t { Scan, Connect, Posture };

struct BleCommand {
  BleOpKind kind = BleOpKind::Scan;
  char address[18] = {};
  // 扫描阶段记录的真实地址类型，连接时原样透传给 connectDevice()。
  uint8_t addressType = 0xFF;   // 0xFF = 未知，按 PUBLIC 处理
};

struct BleOutcome {
  BleOpKind kind = BleOpKind::Scan;
  bool success = false;
  bool cancelled = false;
  size_t deviceCount = 0;
  BlePowerDevice devices[4] = {};
  char error[320] = {};
};

static QueueHandle_t bleCmdQueue = nullptr;
static QueueHandle_t bleResultQueue = nullptr;
static TaskHandle_t  bleWorkerHandle = nullptr;
static volatile bool bleOpBusy = false;         // 工作线程正在执行命令
static volatile bool bleOpCancelRequested = false;

static void bleWorkerTask(void *) {
  for (;;) {
    BleCommand cmd;
    if (xQueueReceive(bleCmdQueue, &cmd, portMAX_DELAY) != pdTRUE) continue;

    bleOpCancelRequested = false;
    bleOpBusy = true;

    BleOutcome out;
    out.kind = cmd.kind;

    // 注意：不要在这里清扫描缓存。
    //   * scanDevices() 内部第一步就是 clearResults()，Scan 自己会重置；
    //   * Connect 只用扫描阶段拿到的地址字符串（副本），connect() 走
    //     NimBLEAddress，不读扫描缓存；
    //   * 在 Scan 与 Connect 之间插一次 clearResults() 正是上一版
    //     "device is no longer in scan results" 的根因。
    switch (cmd.kind) {
      case BleOpKind::Scan:
        out.success = powerMeter.scanDevices(out.devices, 4, out.deviceCount,
                                             BLE_OP_SCAN_DURATION_MS);
        if (!out.success)
          snprintf(out.error, sizeof(out.error), "%s", powerMeter.lastError());
        break;

      case BleOpKind::Connect:
        out.success = powerMeter.connectDevice(cmd.address, cmd.addressType);
        if (!out.success)
          snprintf(out.error, sizeof(out.error), "%s", powerMeter.lastError());
        break;

      case BleOpKind::Posture:
        out.success = posture.tryConnect(BLE_OP_POSTURE_SCAN_MS,
                                         &bleOpCancelRequested);
        if (!out.success)
          snprintf(out.error, sizeof(out.error), "%s", posture.lastError());
        break;
    }

    out.cancelled = bleOpCancelRequested;
    if (out.cancelled && cmd.kind == BleOpKind::Connect) powerMeter.disconnect();

    bleOpBusy = false;
    xQueueSend(bleResultQueue, &out, portMAX_DELAY);

    // 两次操作之间留一点空档，让协议栈处理断连/缓存回收等收尾工作。
    vTaskDelay(pdMS_TO_TICKS(BLE_OP_MIN_GAP_MS));
  }
}

static bool beginBleWorker() {
  if (bleWorkerHandle) return true;
  bleCmdQueue = xQueueCreate(BLE_OP_QUEUE_LEN, sizeof(BleCommand));
  bleResultQueue = xQueueCreate(1, sizeof(BleOutcome));
  if (!bleCmdQueue || !bleResultQueue) {
    Serial.println("[BLE] could not create operation queues");
    return false;
  }
  if (xTaskCreatePinnedToCore(bleWorkerTask, "bleWorker", BLE_OP_TASK_STACK,
                              nullptr, 2, &bleWorkerHandle, 0) != pdPASS) {
    Serial.println("[BLE] could not create BLE worker task");
    bleWorkerHandle = nullptr;
    return false;
  }
  Serial.println("[BLE] serial worker started");
  return true;
}

// 工作线程退出/重启（音频占用 BLE 协议栈时需要释放全部内存）。
static void stopBleWorker() {
  if (!bleWorkerHandle) return;
  vTaskDelete(bleWorkerHandle);
  bleWorkerHandle = nullptr;
  if (bleCmdQueue) { vQueueDelete(bleCmdQueue); bleCmdQueue = nullptr; }
  if (bleResultQueue) { vQueueDelete(bleResultQueue); bleResultQueue = nullptr; }
  bleOpBusy = false;
  bleOpCancelRequested = false;
}

// 入队一条命令。队列满说明上一个操作还没结束，直接返回 false 由调用者提示。
static bool enqueueBleCommand(BleOpKind kind, const char *address = nullptr,
                              uint8_t addressType = 0xFF) {
  if (!bleCmdQueue) return false;
  BleCommand cmd;
  cmd.kind = kind;
  cmd.addressType = addressType;
  if (address)
    snprintf(cmd.address, sizeof(cmd.address), "%s", address);
  if (xQueueSend(bleCmdQueue, &cmd, 0) != pdTRUE) return false;
  return true;
}

static bool takeBleOutcome(BleOutcome &out) {
  if (!bleResultQueue) return false;
  return xQueueReceive(bleResultQueue, &out, 0) == pdTRUE;
}

static void requestBleCancel() {
  if (!bleOpBusy) return;
  bleOpCancelRequested = true;
  powerMeter.cancelCurrentOperation();
  posture.cancelConnect();
}

static bool suspendWirelessForAudio() {
  if (wirelessSuspendedForAudio) return true;
  if (bleOpBusy) {
    Serial.println("[AUDIO] wait for BLE operation to finish before playback");
    return false;
  }
  // 先释放工作线程和队列，再释放协议栈；否则线程会在 NimBLE 全局对象被
  // deinit 后继续轮询命令并调用已失效的 API。
  stopBleWorker();
  powerMeter.disconnect();
  posture.resetAfterStackShutdown();
  if (NimBLEDevice::isInitialized() && !NimBLEDevice::deinit(true)) {
    Serial.println("[AUDIO] could not release BLE stack for playback");
    powerMeter.begin(BLE_DEVICE_NAME, BLE_WHEEL_CIRC_M);
    posture.begin(BLE_DEVICE_NAME);
    if (!beginBleWorker())
      Serial.println("[BLE] worker could not be restarted after audio failure");
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
  if (!audio.releasePlayback()) return;
  if (!wirelessSuspendedForAudio) return;
  powerMeter.begin(BLE_DEVICE_NAME, BLE_WHEEL_CIRC_M);
  posture.begin(BLE_DEVICE_NAME);
  wirelessSuspendedForAudio = false;
  if (!beginBleWorker())
    Serial.println("[BLE] worker could not be restarted after audio");
  // 播放期间姿态重连被挂起，恢复后立刻补一次，不必再等 30s 周期。
  lastBlePostureAttemptMs = millis();
  postAudioBleCatchUpNeeded = true;
  Serial.println("[AUDIO] BLE stack resumed");
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

void setup() {
  // 电源开关管理：尽早初始化以锁定供电（开机需按电池开关）
  powerKey.begin();

  Serial.begin(115200);
  delay(200);
  Serial.println("\n== Aero Probe (single FS3000 + touch UI + BLE PM) ==");
  Serial.println("[PM] Use SELECT POWER METER to choose BLE, virtual, or automatic input.");
  Serial.println("[PM] BLE measurements are streamed to this monitor.");

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
  // 单个串行工作线程负责所有扫描/连接；主循环只投递命令与取结果。
  if (!beginBleWorker())
    Serial.println("[BLE] serial worker unavailable; BLE pairing disabled");
  Serial.println("Scanning for BLE power meter & posture sensor...");
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
  }
  if (!audioPlaying && wirelessSuspendedForAudio) resumeWirelessAfterAudio();
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
    if (audio.stop())
      Serial.println("[SD] playback stopped");
    else
      Serial.println("[SD] playback could not stop; SD card is busy");
  }

#if WIFI_WEB_ENABLE
  bool wifiEnabled = false;
  if (lcd.takeWifiEnableRequest(wifiEnabled)) {
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
      (webSource == 0 || webSource == 1 || webSource == 3)) {
    powerMode = webSource;
    Serial.printf("[WEB] power source -> %d\n", powerMode);
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

    const char *sourceName = src == 3 ? "VIRTUAL" : (src == 1 ? "BLE" : "--");
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
                  (powerMode == 1 ? "BLE" : "AUTO"));
  }

  // ------------------------------------------------------------------------
  // BLE：用户请求 → 命令队列 → 工作线程执行 → 结果队列 → 界面更新
  // ------------------------------------------------------------------------
  // 音频播放期间 BLE 协议栈被释放，此时任何 BLE 请求都直接拒绝，
  // 避免在未初始化的 NimBLE 上调用扫描/连接。
  const bool bleAvailable = !wirelessSuspendedForAudio && bleWorkerHandle;

  if (lcd.takeBleCancelRequest()) {
    if (bleOpBusy) {
      requestBleCancel();
      Serial.println("[BLE] cancellation requested; waiting for the current operation");
    } else {
      // 没有正在执行的操作：这是一次纯界面返回，直接还原界面即可。
      // 不要在这里清扫描缓存——用户按返回后可能马上再次选择同一台设备，
      // 缓存留着无害（连接本身不依赖它），清了反而多一次不必要的释放。
      manualPairingActive = false;
      Serial.println("[BLE] device selection cancelled");
    }
  }

  if (lcd.takeBleScanRequest()) {
    manualPairingActive = true;
    powerMode = 1;
    if (!bleAvailable) {
      manualPairingActive = false;
      lcd.showBleConnectFailure(
          "BLE is paused while audio is playing. Stop the music and try again.");
    } else if (!enqueueBleCommand(BleOpKind::Scan)) {
      Serial.println("[BLE] scan request dropped: worker busy");
      lcd.showBleConnectFailure(
          "The BLE module is still finishing the previous operation. "
          "Wait a moment and press SCAN AGAIN.");
    } else {
      Serial.println("[BLE] scan queued (6 seconds)");
    }
  }

  char selectedAddress[18] = {};
  uint8_t selectedAddressType = 0xFF;
  if (lcd.takeBleConnectRequest(selectedAddress, sizeof(selectedAddress),
                                &selectedAddressType)) {
    Serial.printf("[BLE] user confirmed connection to %s (addressType=%u)\n",
                  selectedAddress, (unsigned)selectedAddressType);
    if (!bleAvailable) {
      manualPairingActive = false;
      lcd.showBleConnectFailure(
          "BLE is paused while audio is playing. Stop the music and try again.");
    } else if (!enqueueBleCommand(BleOpKind::Connect, selectedAddress,
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
    if (outcome.kind == BleOpKind::Posture) {
      postureConnected = outcome.success;
      Serial.printf("[BLE] posture discovery %s\n",
                    postureConnected ? "connected"
                                     : (outcome.cancelled
                                            ? "cancelled"
                                            : "no connection; retry later"));
      if (!postureConnected && !outcome.cancelled && outcome.error[0])
        Serial.printf("[BLE-POSTURE] %s\n", outcome.error);
    } else if (outcome.cancelled) {
      manualPairingActive = false;
      lcd.finishBleCancelled();
      Serial.println("[BLE] operation cancelled");
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
  if (!wirelessSuspendedForAudio && bleWorkerHandle && !manualPairingActive &&
      !postureConnected) {
    const uint32_t elapsed = now - lastBlePostureAttemptMs;
    // 音频结束后补一次；其余情况按 BLE_POSTURE_RECONNECT_MS 限频。
    const bool due = postAudioBleCatchUpNeeded
                         ? !bleOpBusy
                         : elapsed >= BLE_POSTURE_RECONNECT_MS;
    // 队列满说明还有手动操作在排队/执行，姿态发现让位，下一轮再看。
    if (due && !bleOpBusy && uxQueueMessagesWaiting(bleCmdQueue) == 0) {
      if (enqueueBleCommand(BleOpKind::Posture)) {
        lastBlePostureAttemptMs = now;
        postAudioBleCatchUpNeeded = false;
      }
    }
  }
}
