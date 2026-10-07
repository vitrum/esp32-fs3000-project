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
static bool     powerConnected = false;
static bool     postureConnected = false;
static int      powerMode = 0;      // 0=自动 1=BLE 2=ANT+
#ifdef ANT_POWER_ENABLE
static bool     antStarted = false;   // ANT 节点已启动（此后 BLE 扫描不可用）
#endif
static WindVector wv;            // 最近一次探针读数（供显示）

// 当前功率数据源：0=无 1=BLE 2=ANT+（写进 CSV 供后处理区分）
static int pmSrc() {
#ifdef ANT_POWER_ENABLE
  if (powerMode == 2) return antPower.started() && antPower.tracking() ? 2 : 0;
  if (powerMode == 1) return powerConnected ? 1 : 0;
  if (antPower.started() && antPower.tracking()) return 2;
#endif
  return powerConnected ? 1 : 0;
}

// 有效功率/踏频（ANT 跟踪时优先用 ANT+，否则退回 BLE 功率计）
static float pmPower() {
#ifdef ANT_POWER_ENABLE
  if (powerMode == 2) return antPower.power();
  if (powerMode == 1) return powerMeter.power();
  if (antPower.started() && antPower.tracking()) return antPower.power();
#endif
  return powerMeter.power();
}
static float pmCadence() {
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
    lcd.update(wv, power, pmCadence(), vg, post, rho, src, logger.ok(), env.temperatureC());

    // 串口同输出一行，便于现场监控
    Serial.printf("%lu P=%5.0fW(%s) cad=%4.0f vg=%4.2f vAir=%4.2f yaw=%+5.1f pos=%+5.1f rho=%.3f\n",
                  (unsigned long)now, power,
                  src == 2 ? "ANT" : (src == 1 ? "BLE" : "--"),
                  pmCadence(), vg, vAir, yaw, post, rho);

    win = Window();   // 清窗口
  }

  lcd.tick();

  int requestedMode = -1;
  if (lcd.takePowerSourceRequest(requestedMode)) {
    powerMode = requestedMode;
    Serial.printf("[PM] selected mode: %s\n",
                  powerMode == 2 ? "ANT+" : (powerMode == 1 ? "BLE" : "AUTO"));
#ifdef ANT_POWER_ENABLE
    if (powerMode == 1 && antStarted) {
      antPower.stop();
      antStarted = false;
    }
#endif
  }

  // 3) 自动模式先扫描 BLE；选择 ANT+ 或自动模式达到延迟后启动 ANT。
  //    ANT 使用 NimBLE 被动扫描窗口；切回 BLE 时停止 ANT 并恢复扫描。
#ifdef ANT_POWER_ENABLE
  bool shouldStartAnt = powerMode == 2 ||
                        (powerMode == 0 && now >= ANT_POWER_START_DELAY_MS);
  if (!antStarted && shouldStartAnt &&
      (powerMode == 2 || now >= ANT_POWER_START_DELAY_MS)) {
    antStarted = true;
    antPower.begin();
  }
  if (!antStarted && ((powerMode != 2 && !powerConnected) || !postureConnected) &&
      now - lastBleMs >= BLE_RECONNECT_MS) {
    lastBleMs = now;
    if (!powerConnected && powerMode != 2) {
      powerConnected = powerMeter.tryConnect();
    } else if (!postureConnected) {
      postureConnected = posture.tryConnect();
    }
  }
#else
  if ((!powerConnected || !postureConnected) &&
      now - lastBleMs >= BLE_RECONNECT_MS) {
    lastBleMs = now;
    if (!powerConnected) {
      powerConnected = powerMeter.tryConnect();
    } else if (!postureConnected) {
      postureConnected = posture.tryConnect();
    }
  }
#endif
}
