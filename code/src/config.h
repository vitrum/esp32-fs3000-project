#pragma once

// ============================================================================
// 板卡：Waveshare ESP32-S3-Touch-LCD-2.8（V1/V2 均可）
// 引脚定义依据官方文档/wik 核对，若你的板子版本不同请对照原理图微调
// ============================================================================

// ---- ST7789 液晶屏 (SPI) ----
#define PIN_LCD_MOSI  45
#define PIN_LCD_SCLK  40
#define PIN_LCD_CS    42
#define PIN_LCD_DC    41
#define PIN_LCD_RST   39
#define PIN_LCD_BL    5

// ---- 板载触摸屏 (V1: CST328, I2C) ----
#define PIN_TOUCH_SDA  1
#define PIN_TOUCH_SCL  3
#define PIN_TOUCH_INT  4
#define PIN_TOUCH_RST  2

// ---- 外接 I2C #1 ----
// 原型阶段单 FS3000 与 BME280（可选）共用此总线
#define PIN_I2C1_SDA  11
#define PIN_I2C1_SCL  10

// ---- 外接 I2C #2（预留二期扩展，12 针排针 IO15/IO18）----
#define PIN_I2C2_SDA  15
#define PIN_I2C2_SCL  18

// ---- TF 卡（SDSPI）----
#define PIN_SD_SCLK   14
#define PIN_SD_MISO   16
#define PIN_SD_MOSI   17
#define PIN_SD_CS     21
// SDSPI 频率：该板走 SPI 且线较长，大容量 SDXC 卡在写后偶尔不响应 CMD13
// 状态查询（Card Failed! cmd:0x0d）。1MHz 最稳，写 CSV 数据量小、够用。
#define SD_SPI_FREQ_HZ 1000000

// ---- 电源开关（电池开关 / PWR Key）----
// 依据 Waveshare ESP32-S3-LCD-2.8 官方 PWR_Key 示例：
//   PWR_KEY_Input_PIN  = GPIO6  电池开关按键输入（低电平=按下）
//   PWR_Control_PIN    = GPIO7  电源保持控制（拉高=锁定供电，拉低=断电）
// 行为：上电后若检测到按键按下 → 拉高 GPIO7 保持开机；
//       长按 5 秒 → 拉低 GPIO7 断电关机。
#define PIN_PWR_KEY       6
#define PIN_PWR_CONTROL   7
#define PWR_LONG_PRESS_MS 5000   // 长按关机阈值(ms)

// ============================================================================
// 音频（PCM5101 I2S 外置 DAC + 板载喇叭）
// 依据 Waveshare ESP32-S3-LCD-2.8 官方 Audio_Driver/PCM5101 示例：
//   BCLK/SCLK = GPIO48, LRC/WS = GPIO38, DOUT = GPIO47, MCLK = 无
// 用途：开机/关机提示音（Wahoo 风格）+ 播放 SD 卡内 MP3
// ============================================================================
#define AUDIO_ENABLE      1      // 1=启用音频；0=关闭（省内存/引脚）
#define PIN_I2S_BCLK      48
#define PIN_I2S_LRC       38
#define PIN_I2S_DOUT      47
#define AUDIO_VOLUME      55     // 0~100 默认音量
#define AUDIO_SYSTEM_DIR  "/system"        // 系统音效目录（SD 卡内）
#define AUDIO_BOOT_WAV    "/system/boot.wav"
#define AUDIO_SHUTDOWN_WAV "/system/shutdown.wav"

// ============================================================================
// FS3000 空气流速传感器
// ============================================================================
#define FS3000_ADDR       0x28    // FS3000 固定 7bit 地址
#define FS3000_SAMPLE_MS  125     // 数据手册响应时间 125ms，采样间隔不小于它

// ============================================================================
// V 形视风探针（两片 FS3000 相对车头各 ±45° 张开、同点安装）
// ============================================================================
#define PROBE_HALF_ANGLE_DEG  45.0f   // 半夹角
#define PROBE_SAMPLE_MS       125     // 探针采样周期
#define LOG_WINDOW_MS         1000    // 日志输出周期（1s 一个窗口）

// ============================================================================
// BLE 功率计（Cycling Power Service 0x1818 / Measurement 0x2A63）
// ============================================================================
#define BLE_DEVICE_NAME      "AeroProbe"
#define BLE_WHEEL_CIRC_M     2.105f   // 轮周长(m)，700x25C 约 2.105，按实际外胎设置
#define BLE_RECONNECT_MS     5000     // 未连接时的重试间隔

// ---- 单传感器 CdA 粗略估算（平路、匀速、静风假设；仅用于趋势参考）----
#define CDA_TOTAL_MASS_KG          85.0f
#define CDA_ROLLING_RESISTANCE     0.004f
#define CDA_DRIVETRAIN_EFFICIENCY  0.975f
#define CDA_MIN_AIRSPEED_MPS       3.0f

// ============================================================================
// BLE 姿态传感器（WitMotion WT9011DCL-BT50，服务 0xFFE0 / 数据 0xFFE1）
// 无线免接线，绑胸口/上背，输出 Pitch（俯仰角）作为躯干姿态角
// ============================================================================
#define BLE_POSTURE_NAME_PREFIX  "WT"   // WitMotion 设备名前缀（扫描过滤用）

// ============================================================================
// ANT+ 功率计（esp32-ant 纯软件栈，见 code/lib/ant/，Apache-2.0）
// 原理：用 ESP32-S3 自己的 BLE 射频跑 ANT 协议（LE test mode / coexist），
//       无需外置 ANT 芯片。Bike Power profile 输出功率(W)+踏频(rpm)。
// 模式：coexist（与 NimBLE 共用射频）——BLE 已建立的连接（如姿态传感器）
//       保持不断，ANT 借用 NimBLE 被动扫描的射频窗口收包；代价是 ANT 运行
//       期间 BLE 的"扫描"暂停（所以 BLE 设备要先连接好，见 ANT_POWER_START_DELAY_MS）。
// 注意：ANT+ 只收不发；同型号功率计若同时支持 ANT+ 与 BLE，可任选一路。
// ============================================================================
#define ANT_POWER_ENABLE         1      // 1=启用 ANT+ 功率计接收；0=仅 BLE 功率计
#define ANT_POWER_START_DELAY_MS 20000  // 启动后延迟再开 ANT(ms)：先给 BLE 姿态/功率计连接窗口
#define ANT_POWER_PROXIMITY_RSSI -70    // 首次配对的距离门限 dBm（0=不限制，-70≈车上的设备）

// ============================================================================
// 可选环境传感器 BME280（计算空气密度 ρ = P/(287.05·T)）
// ============================================================================
#define BME280_ADDR          0x76     // BME280 默认地址，与板载地址不冲突

// ============================================================================
// Wi-Fi 网页实时数据服务（SmartConfig 配网 + Web 服务器）
// 见 plan/wifi-web.md
//
// 注意射频共存：ESP32-S3 单射频，Wi-Fi / BLE / ANT+ 三者共享。
// 开启 Wi-Fi 后 BLE/ANT+ 可能出现丢包；如绕圈测试只看实时数据，
// 可考虑暂停 ANT+（见 config.h 的 ANT_POWER_ENABLE）。
// ============================================================================
#define WIFI_WEB_ENABLE      1          // 1=启用 Wi-Fi 网页服务；0=关闭（省射频/内存）
#define WIFI_MDNS_HOST       "aeroprobe" // mDNS 主机名 → http://aeroprobe.local/
#define WIFI_RECONNECT_MS    10000       // STA 断线重连间隔(ms)
#define WIFI_SMART_CONFIG_TIMEOUT_MS 120000 // SmartConfig 无人配网时的超时(ms)
