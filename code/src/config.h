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

// ---- BOOT 按钮（板上 BOOT 键，GPIO0）----
// 按下为低电平。本固件用它打开设置页（短按触发一次）。
#define PIN_BOOT_KEY      0

// ============================================================================
// 音频（PCM5101 I2S 外置 DAC + 板载喇叭）
// 依据 Waveshare ESP32-S3-LCD-2.8 官方 Audio_Driver/PCM5101 示例：
//   BCLK/SCLK = GPIO48, LRC/WS = GPIO38, DOUT = GPIO47, MCLK = 无
// 用途：播放 SD 卡内 MP3
// ============================================================================
#define AUDIO_ENABLE      0      // 1=启用音频；0=关闭（省内存/引脚）。
                                 // 当前阶段关闭全部音乐/提示音功能，避免音频占用
                                 // I2S/SD 并与 BLE 无线争用；蓝牙功能不受影响。
#define PIN_I2S_BCLK      48
#define PIN_I2S_LRC       38
#define PIN_I2S_DOUT      47
#define AUDIO_VOLUME      55     // 0~100 默认音量
#define AUDIO_SYSTEM_DIR  "/system"        // 系统音效目录（SD 卡内）

// ============================================================================
// GPS / 北斗双模模块（ATGM336H 等，中科微 AT6558 方案）
// 接口：UART（标准 NMEA 0183 语句），3.3V TTL 电平，直接连 ESP32-S3。
// 接线（板上 12PIN 排针的 UART 口：TXD=GPIO43 / RXD=GPIO44）：
//   GPS TX  -> ESP32 RXD (GPIO44)
//   GPS RX  -> ESP32 TXD (GPIO43)   [可选：用于发配置语句]
#define BLE_DEVICE_NAME      "AeroProbe"
#define BLE_WHEEL_CIRC_M     2.105f   // 轮周长(m)，700x25C 约 2.105，按实际外胎设置
#define BLE_RECONNECT_MS     5000     // 未连接时的重试间隔
#define BLE_POSTURE_RECONNECT_MS 30000 // 姿态发现限频，减少与 Wi-Fi 的射频争用
// 所有 BLE 操作（手动扫描 / 连接 / 姿态发现）由单一工作线程串行执行。
// 主循环只投递命令、取回结果，不再直接调用 NimBLE，避免两个任务争抢扫描器。
#define BLE_OP_QUEUE_LEN     4        // 命令队列深度（满则丢弃并提示 busy）
#define BLE_OP_TASK_STACK    8192     // 工作线程栈（GATT 发现 + 日志缓冲）
#define BLE_OP_SCAN_DURATION_MS   6000 // 手动扫描时长
#define BLE_OP_POSTURE_SCAN_MS    1500 // 姿态传感器扫描时长
#define BLE_UI_TOUCH_GUARD_MS 450     // BLE 相关按钮的触控缓冲（忽略连击/误触）
//   GPS VCC -> 3V3（若模块带 LDO 可 5V，先确认模块耐压）
//   GPS GND -> GND
// 注意：本版本不使用 PPS 秒脉冲（仅串口，精度到秒级）。
//      如需高精度授时，可将 PPS 单独接到一个空闲 GPIO（如 IO15）并启用下方宏。
// ============================================================================
#define GPS_ENABLE     1        // 1=启用 GPS；0=关闭（省串口/内存）
#define PIN_GPS_RX     44       // ESP32 收 <- GPS TX
#define PIN_GPS_TX     43       // ESP32 发 -> GPS RX（配置用，可选）
#define GPS_BAUD       9600     // ATGM336H 出厂默认 9600；如模块改为 38400 请同步
// 是否使用 GPS 地速 v_g 参与 CdA 计算（无功率计轮速时尤其有用）
#define GPS_USE_SPEED_FOR_VG 1
// PPS 秒脉冲（本版本不启用）：接空闲 GPIO 后置 1
#define GPS_USE_PPS    0
#define PIN_GPS_PPS    15       // 仅 GPS_USE_PPS=1 时生效

// ----------------------------------------------------------------------------
// 无 GPS 时间时的开机默认时间（测试阶段用）
// ----------------------------------------------------------------------------
// FIT 文件需要一个绝对 UTC 起点，否则无法生成合法的时间戳。若一直等不到
// GPS 授时，测试阶段允许退回一个固定的「开机默认 UTC 时间」：
//   - 每个样本的时间轴仍是「开机后经过的真实时长」，只是被平移到这个默认
//     起点上，因此 FIT 内部的相对时间（时长/速度/距离）依然正确；
//   - 绝对日期不正确，仅影响 FIT 显示的年月日。
// 值为 Unix 秒（自 1970-01-01 UTC）。默认 2024-01-01 00:00:00 UTC。
// 把它设为 0 即恢复旧行为：没有 GPS 授时不允许保存 FIT。
#define FIT_DEFAULT_UTC           1704067200u   // 2024-01-01 00:00:00 UTC
// GPS 授时优先：只要 GPS 时间有效就使用 GPS 时间，仅在不可用时才用默认值。

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
#define BLE_POSTURE_RECONNECT_MS 30000 // 姿态发现限频，减少与 Wi-Fi 的射频争用

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
// 可选环境传感器 BME280（计算空气密度 ρ = P/(287.05·T)）
// ============================================================================
#define BME280_ADDR          0x76     // BME280 默认地址，与板载地址不冲突

// ============================================================================
// Wi-Fi 网页实时数据服务（SmartConfig 配网 + Web 服务器）
// 见 plan/wifi-web.md
//
// Wi-Fi / BLE 共用单射频，由 ESP-IDF 软件共存机制仲裁。
// 采集时避免持续扫描；网页下载分块读取 SD，不持锁等待网络发送。
// ============================================================================
#define WIFI_WEB_ENABLE      1          // 1=启用 Wi-Fi 网页服务；0=关闭（省射频/内存）
#define WIFI_MDNS_HOST       "aeroprobe" // mDNS 主机名 → http://aeroprobe.local/
#define WIFI_RECONNECT_MS    10000       // STA 断线重连间隔(ms)
#define WIFI_SMART_CONFIG_TIMEOUT_MS 120000 // SmartConfig 无人配网时的超时(ms)
