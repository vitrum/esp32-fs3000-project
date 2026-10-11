# 绝对时间源方案（GPS / NTP / RTC / BLE）

> 本文档针对 Aero Probe 的**时间戳**需求，给出在 ESP32-S3 上获取**准确绝对时间**的
> 方案对比、硬件选型与接线，以及建议的统一时间源抽象。
>
> **时间戳是本项目多处功能的共同地基**：
> - FIT 文件（[`plan/README.md`](./README.md) §3）需要 UTC 时间戳；
> - 与码表 FIT 合并对齐（[`plan/app-merge.md`](./app-merge.md) §4）需要绝对时间锚点；
> - 网页实时/历史数据（[`plan/wifi-web.md`](./wifi-web.md)）需要可读的时间。

---

## 目录

- [1. 问题背景](#1-问题背景)
- [2. 时间源方案对比](#2-时间源方案对比)
- [3. 关键判断：Wi-Fi 与 GPS 各管一半](#3-关键判断wi-fi-与-gps-各管一半)
- [4. 为什么户外强烈建议 GPS](#4-为什么户外强烈建议-gps)
- [5. 硬件选型与接线](#5-硬件选型与接线)
- [6. 固件集成](#6-固件集成)
- [7. 统一时间源抽象](#7-统一时间源抽象)
- [8. 落地建议（分阶段）](#8-落地建议分阶段)
- [9. 待决策项](#9-待决策项)

---

## 1. 问题背景

当前固件**没有任何绝对时间源**：

- UI 上的时间是 `millis()` 格式化的相对时间；
- CSV 第一列 `t_ms` 是开机后的毫秒数（`millis()`），重启即归零；
- 因此 FIT 时间戳、与码表 FIT 的对齐都**无从建立**。

要"对齐时间点"（app-merge 的核心），必须给 ESP32 一个能映射到 **UTC** 的时间基准。

---

## 2. 时间源方案对比

| 方案 | 精度 | 成本 | 无需硬件？ | 断电后 | 适用场景 | 推荐度 |
| ---- | ---- | ---- | ---------- | ------ | -------- | ------ |
| **1. Wi-Fi NTP 对时** | ±10~100ms（有网时 ±ms 级） | **0 元** | ✔ | 每次开机重新对 | 室内 / 有 Wi-Fi | ⭐⭐⭐⭐⭐ |
| **2. GPS/GNSS 模块** | **±30~100ns（PPS）/ ±1ms（串口）** | ~15~40 元 | ✘ | 每次开机需定位 | **户外跑道（无网）** | ⭐⭐⭐⭐⭐ |
| **3. RTC 模块 + 首次对时** | ±2ppm（年差 ~1 分钟，可校准） | ~5~10 元 | ✘ | ✔ 掉电走时 | 任何场景的"守时"底座 | ⭐⭐⭐⭐ |
| **4. BLE 从码表/手机取时** | ±50~500ms | 0 元 | ✔ | 需连设备 | 配合手机 APP | ⭐⭐⭐ |
| **5. 码表 FIT 事后对齐** | 事后离线精对齐 | 0 元 | ✔ | 不依赖 | 与码表合并场景 | ★（兜底，非实时） |

> 说明：方案 5 不是"实时时间源"，而是 app-merge 中用**功率互相关**做事后精对齐的兜底手段；
> 它能把两段序列对齐，但**不能给 ESP32 自身**提供实时绝对时间。

---

## 3. 关键判断：Wi-Fi 与 GPS 各管一半

**本项目同时有 Wi-Fi 与（可选）GPS 两张牌，正好覆盖两类场景：**

- **室内骑行台**：Wi-Fi 连上即 NTP 自动对时，精度足够，**无需任何额外硬件**。
- **户外跑道绕圈（无 Wi-Fi）**：GPS 是主场——**既有时间，又顺带速度和轨迹**。

> 因此：**不要只押一个方案**，而是让两者互补（见 §7 统一抽象）。

---

## 4. 为什么户外强烈建议 GPS

户外测试的核心是 **CdA**，公式里的 `v_g`（地速）当前来自功率计轮速
（`powerMeter.speed()`），但很多功率计**不发轮速**，此时 `vg = 0`，CdA 直接失效。

GPS **一举多得**：

| 收益 | 说明 |
| ---- | ---- |
| ✅ **绝对 UTC 时间** | FIT 时间戳、与码表对齐的地基 |
| ✅ **地速 `v_g`** | CdA 公式必需，且独立于功率计，比轮速更可靠 |
| ✅ **GPS 轨迹** | 导出 FIT 到 Strava / Intervals.icu，轨迹完整 |
| ✅ **坡度** | 有高程可算 `P_grade`（后处理脚本 `--grade` 目前靠手填） |
| ✅ **1PPS 秒脉冲** | 可做高精度时间同步（进阶） |

**结论**：对"户外绕圈测试"这一主场景，GPS 是最优解。

---

## 5. 硬件选型与接线

### 5.1 推荐模块

| 模块 | 芯片 | 接口 | 特点 | 价格 |
| ---- | ---- | ---- | ---- | ---- |
| **ATGM336H / ATGM332D** | 中科微 AT6558 | UART + PPS | 便宜、国内好买、北斗+GPS+GLONASS | ~15~25 元 |
| **u-blox NEO-6M / 7M / M8N** | u-blox | UART + PPS | 经典，M8N 多星座更好 | ~25~50 元 |
| **Quectel L76K / L86** | Quectel | UART + PPS | 模块化、体积小 | ~30 元 |
| **u-blox MAX-M10S** | u-blox | UART/I2C/SPI + PPS | 现代多星座、低功耗、精度好 | ~60~100 元 |

**推荐**：**ATGM336H（性价比）** 或 **u-blox M10 / M8N（精度与生态更好）**。
均输出标准 **NMEA 0183**，解析简单。

### 5.2 接线（UART）

大多数 GPS 模块走 **UART（默认 9600 波特率，NMEA 语句）**。
ESP32-S3 有 3 个 UART：UART0 = 原生 USB 日志，**UART1/UART2 可用**。

```text
GPS 模块           ESP32-S3
  VCC ───────────── 3V3（注意模块电压，多数 3.3~5V）
  GND ───────────── GND
  TX  ───────────── GPIO43（ESP32 的 RX）
  RX  ───────────── GPIO44（ESP32 的 TX，用于配置波特率/语句，可选）
  PPS ───────────── GPIOxx（可选，高精度同步用）
```

> ⚠️ **接口电压**：GPS 模块多为 3.3V 逻辑，可直接接；若为 5V 模块，
> TX 到 ESP32 需电平保护（分压 / 电平转换），否则烧 RX 引脚。
>
> ⚠️ **天线**：陶瓷天线需朝上、远离金属 / LCD / 电源，否则定位慢、丢星。
> 跑道场景模块最好放车顶 / 支架高处。

> 说明：`config.h` 里已预留 **I2C2（GPIO15/18）**，但 GPS 一般走 UART。
> 具体 `PIN_GPS_RX / PIN_GPS_TX` 取哪个空闲 IO，需按 12 针排针可用引脚确定（见 §9）。

### 5.3 解析库

PlatformIO 直接使用现成库：

```ini
lib_deps =
  ; ... 现有依赖
  mikalhart/TinyGPSPlus@^1.0.3   ; 轻量 NMEA 解析（推荐）
```

`TinyGPSPlus` 可拿到：`date/time`（UTC）、`location`（lat/lon）、`speed.mps()`、
`altitude.m()`、`satellites`、`hdop`。PPS 可用中断做微秒级对齐（进阶）。

---

## 6. 固件集成

新增 `src/gps_time.{h,cpp}`：

```cpp
// gps_time.h —— GPS/GNSS 时间与地速
class GpsTime {
public:
  bool begin(int rxPin, int txPin, uint32_t baud = 9600);
  void tick();                          // 非阻塞喂 NMEA，主循环调用

  bool hasFix()      const;             // 是否已定位
  bool timeValid()   const;             // 时间是否可信（用于打戳）
  uint32_t unixSec() const;             // 绝对 UTC 秒
  uint32_t dateNum() const;             // 可选：日期
  float speedMps()   const;             // 地速（来自 GPS，替代功率计轮速）
  double lat(), lon();
  float altitudeM();
  uint8_t satellites(), hdop();

  // 统一时间接口：与 NTP / RTC 共用（见 §7）
  bool getTime(int64_t &unixSec, uint32_t &ageMs) const;
};
```

在 `main.cpp` 中集成（示意）：

```cpp
GpsTime gps;

void setup() {
  // ... 现有 ...
  gps.begin(PIN_GPS_RX, PIN_GPS_TX);   // 新增引脚宏
}

void loop() {
  gps.tick();   // 非阻塞，随时可调

  // ... 采样窗口 ...

  // 用 GPS 地速替代/补充功率计轮速
  float vg = gps.hasFix() ? gps.speedMps() : powerMeter.speed();

  // 记录行里加上绝对时间（需扩展 CsvLogger::log 签名与 CSV 列）
  logger.log(now, power, cadence, vg, vAir, yaw, post, rho, src, gps.unixSec());
}
```

> 与 [`plan/README.md`](./README.md) §2.5 呼应：新增 `unixMs` 列即由此处提供。

---

## 7. 统一时间源抽象

GPS、Wi-Fi NTP、BLE、RTC 四条潜在来源**各写各的会乱**，建议抽一层：

```cpp
// time_source.h —— 统一时间源，按优先级选择
enum class TimeSourceKind { None, Gps, Ntp, Ble, Rtc };

class TimeSource {
public:
  void update();                          // 各来源喂数据
  bool valid() const;                     // 是否有可用绝对时间
  uint32_t unixSec() const;
  TimeSourceKind active() const;          // 当前生效的来源
  uint32_t ageMs() const;                 // 距上次同步多久
};
```

**优先级建议**：

```text
GPS（有定位） > NTP（连上 Wi-Fi） > BLE（手机对时） > RTC（守时兜底）
```

- **RTC + GPS 组合**是最佳拍档：GPS 定位时校准 RTC；GPS 丢失期间 RTC 守时；
  RTC 还能保证**开机瞬间**就有大致正确时间（不用等 GPS 冷启动的 30s~数分钟）。
- **NTP 与 GPS 都无**时，RTC 是被动最稳的兜底。

---

## 8. 落地建议（分阶段）

| 阶段 | 做什么 | 解决 |
| ---- | ------ | ---- |
| **近期（0 成本）** | 用 **Wi-Fi NTP** 对时 | 室内场景 + 部分户外（手机热点亦可） |
| **中期** | 加 **GPS 模块**（ATGM336H / M10） | 户外跑道：时间 + 地速 + 轨迹 + 坡度 |
| **可选** | 加 **RTC（PCF8563 / DS3231）** | 开机即有近似时间，GPS/NTP 失效时守时 |
| **长期** | 抽 **TimeSource** 统一层 | 多源融合，代码整洁 |

### 对本项目最实际的两步

1. **先加 GPS 模块**（户外主场景刚需）——同时解决时间戳与 CdA 缺 `v_g` 的问题。
2. **室内用 Wi-Fi NTP**（已有 Wi-Fi，白送的时间源）。

RTC 可暂缓；但若希望**每次开机立刻**有正确时间且不想等 GPS 冷启动，
加个几块钱的 **PCF8563（挂已预留的 I2C2：GPIO15/18）**很值。

---

## 9. 待决策项

1. **主要测试场景**：室内为主还是户外跑道为主？（决定 GPS vs NTP 优先级）
2. **GPS 接哪个 UART/引脚**？12 针排针上还有哪些空闲 IO？
   —— 确定后可定义 `PIN_GPS_RX / PIN_GPS_TX` 宏。
3. **是否加 RTC** 做开机守时？
4. **GPS 地速**是否直接替代功率计轮速，还是仅作补充/校验？
5. **误差目标**：对齐精度要求到秒级（够用）还是亚秒/毫秒级（需 PPS）？

---

## 附：文档关系

| 文档 | 面向 | 时间戳用途 |
| ---- | ---- | ---------- |
| [`plan/README.md`](./README.md) | 存储子系统 | FIT 时间戳、CSV `unixMs` 列 |
| [`plan/app-merge.md`](./app-merge.md) | 手机 APP | 与码表 FIT 对齐的绝对时间锚点 |
| [`plan/wifi-web.md`](./wifi-web.md) | PC 网页 | 实时/历史数据的可读时间 |
| **`plan/time-source.md`（本文）** | 时间地基 | GPS / NTP / RTC / BLE 的选型与统一抽象 |
