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
#ifdef ANT_POWER_ENABLE
#include "ant_power.h"
#endif

FS3000       windSensor;
BlePowerMeter powerMeter;
BlePosture   posture;
EnvSensor    env;
CsvLogger    logger;
DisplayLcd   lcd;
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
static uint32_t lastBleMs    = 0;
static uint32_t lastBlePowerNotification = 0;
static uint32_t lastBlePostureNotification = 0;
static uint32_t lastBleDisconnectCount = 0;
static bool     powerConnected = false;
static bool     postureConnected = false;
static bool     manualBlePairing = false;
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
  Serial.begin(115200);
  delay(200);
  Serial.println("\n== Aero Probe (single FS3000 + touch UI + BLE/ANT+ PM) ==");
  Serial.println("[PM] Use SELECT POWER METER to choose BLE, ANT+, virtual, or automatic input.");
  Serial.println("[PM] BLE/ANT+ measurements are streamed to this monitor.");

  // 原型阶段单风速传感器；Wire1 由 LCD 初始化为 CST328 触摸总线
  Wire.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, 400000);

  bool windOk = windSensor.begin(Wire);
  Serial.printf("FS3000 %s\n", windOk ? "OK" : "FAIL");

  env.begin(Wire);                   // 可选 BME280（挂在 I2C1）
  Serial.printf("BME280 %s\n", env.present() ? "OK" : "absent");

  logger.begin();                    // TF 卡（可选，失败不阻塞）
  lcd.begin();                       // 屏幕

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

  // 1) 探针采样（8Hz，受 125ms 响应限制）
  if (now - lastSampleMs >= PROBE_SAMPLE_MS) {
    lastSampleMs = now;
    float windMps = 0.0f;
    if (windSensor.readMps(windMps)) {
      wv = WindVector();
      wv.vAlong = windMps;
      wv.vAir = windMps;
      win.n++;
      win.sumPower   += pmPower();
      win.sumVg      += powerMeter.speed();
      win.sumVAir    += wv.vAir;
      win.sumYaw     += wv.yawDeg;
      win.sumPosture += posture.pitchDeg();
    }
  }

  // 2) 每秒输出一条日志 + 刷新屏幕
  if (now - lastLogMs >= LOG_WINDOW_MS && win.n > 0) {
    lastLogMs = now;

    float power   = win.sumPower   / (float)win.n;
    float vg      = win.sumVg      / (float)win.n;
    float vAir    = win.sumVAir    / (float)win.n;
    float yaw     = win.sumYaw     / (float)win.n;
    float post    = win.sumPosture / (float)win.n;
    float rho     = env.rhoKgM3();
    int   src     = pmSrc();

    logger.log(now, power, pmCadence(), vg, vAir, yaw, post, rho, src);
    lcd.update(wv, pmPower(), pmCadence(), vg, post, rho, src, logger.ok());

    // 串口同输出一行，便于现场监控
    const char *sourceName = src == 3 ? "VIRTUAL" :
                             (src == 2 ? "ANT" : (src == 1 ? "BLE" : "--"));
    Serial.printf("%lu P=%5.0fW(%s) cad=%4.0f vg=%4.2f vAir=%4.2f yaw=%+5.1f pos=%+5.1f rho=%.3f\n",
                  (unsigned long)now, power,
                  sourceName,
                  pmCadence(), vg, vAir, yaw, post, rho);

    win = Window();   // 清窗口
  }

  lcd.tick();

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
  if (blePowerNotifications != lastBlePowerNotification) {
    lastBlePowerNotification = blePowerNotifications;
    Serial.printf("[BLE-DATA] P=%.0f W cad=%.0f rpm speed=%.2f m/s\n",
                  powerMeter.power(), powerMeter.cadence(), powerMeter.speed());
  }
  static uint32_t lastBleFtmsNotification = 0;
  uint32_t bleFtmsNotifications = powerMeter.fitnessMachineNotificationCount();
  if (bleFtmsNotifications != lastBleFtmsNotification) {
    lastBleFtmsNotification = bleFtmsNotifications;
    const uint16_t flags = powerMeter.lastFitnessMachineFlags();
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
  bool shouldStartAnt = powerMode == 2 ||
                        (powerMode == 0 && now >= ANT_POWER_START_DELAY_MS);
  if (!antStarted && shouldStartAnt &&
      (powerMode == 2 || now >= ANT_POWER_START_DELAY_MS)) {
    antStarted = true;
    antPower.begin();
  }
  if (!antStarted && !manualBlePairing && !postureConnected &&
      now - lastBleMs >= BLE_RECONNECT_MS) {
    lastBleMs = now;
    postureConnected = posture.tryConnect();
  }
#else
  if (!manualBlePairing && !postureConnected &&
      now - lastBleMs >= BLE_RECONNECT_MS) {
    lastBleMs = now;
    postureConnected = posture.tryConnect();
  }
#endif
}
