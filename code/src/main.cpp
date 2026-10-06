// ============================================================================
// ESP32-S3-Touch-LCD-2.8 骑行气动测试固件
//   双 FS3000-1015 V 形视风探针（风速 v_a + 偏航角 φ）
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
#include "wind_probe.h"
#include "ble_power.h"
#include "ble_posture.h"
#include "bme280_env.h"
#include "csv_logger.h"
#include "display_lcd.h"
#ifdef ANT_POWER_ENABLE
#include "ant_power.h"
#endif

WindProbe    probe;
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
#ifdef ANT_POWER_ENABLE
static bool     antStarted = false;   // ANT 节点已启动（此后 BLE 扫描不可用）
#endif
static WindVector wv;            // 最近一次探针读数（供显示）

// 当前功率数据源：0=无 1=BLE 2=ANT+（写进 CSV 供后处理区分）
static int pmSrc() {
#ifdef ANT_POWER_ENABLE
  if (antPower.started() && antPower.tracking()) return 2;
#endif
  return powerConnected ? 1 : 0;
}

// 有效功率/踏频（ANT 跟踪时优先用 ANT+，否则退回 BLE 功率计）
static float pmPower() {
#ifdef ANT_POWER_ENABLE
  if (antPower.started() && antPower.tracking()) return antPower.power();
#endif
  return powerMeter.power();
}
static float pmCadence() {
#ifdef ANT_POWER_ENABLE
  if (antPower.started() && antPower.tracking()) return antPower.cadence();
#endif
  return powerMeter.cadence();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n== Aero Probe (FS3000 x2 + BLE PM + ANT+ PM + BLE Posture) ==");

  // 两路 I2C：传感器 #1 在板载排针，传感器 #2 在 IO15/IO18
  Wire.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, 400000);
  Wire1.begin(PIN_I2C2_SDA, PIN_I2C2_SCL, 400000);

  probe.begin(Wire, Wire1);          // 左臂=Wire，#2右臂=Wire1
  Serial.printf("Probe  L:%s R:%s\n",
                probe.leftOk() ? "OK" : "FAIL", probe.rightOk() ? "OK" : "FAIL");

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
    if (probe.read(wv)) {
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
    lcd.update(wv, power, pmCadence(), vg, post, rho, src, logger.ok());

    // 串口同输出一行，便于现场监控
    Serial.printf("%lu P=%5.0fW(%s) cad=%4.0f vg=%4.2f vAir=%4.2f yaw=%+5.1f pos=%+5.1f rho=%.3f\n",
                  (unsigned long)now, power,
                  src == 2 ? "ANT" : (src == 1 ? "BLE" : "--"),
                  pmCadence(), vg, vAir, yaw, post, rho);

    win = Window();   // 清窗口
  }

  // 3) 时序控制：
  //    - ANT_POWER_START_DELAY_MS 之前：正常 BLE 扫描/连接（姿态、BLE 功率计）
  //    - 到点后启动 ANT+（借用 BLE 被动扫描窗口），此后 BLE 扫描暂停，
  //      不再尝试 BLE 重连（已建立的 BLE 连接不受影响）
#ifdef ANT_POWER_ENABLE
  if (!antStarted && now >= ANT_POWER_START_DELAY_MS) {
    antStarted = true;
    antPower.begin();
  }
  if (!antStarted && (!powerConnected || !postureConnected) &&
      now - lastBleMs >= BLE_RECONNECT_MS) {
    lastBleMs = now;
    if (!powerConnected) {
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
