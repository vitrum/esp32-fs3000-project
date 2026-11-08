# 绝对时间源方案与当前决策（NTP / GPS / RTC / BLE）

> 本文档针对 Aero Probe 的**时间戳**需求，给出在 ESP32-S3 上获取**准确绝对时间**的
> 方案对比及当前生产固件的授时行为。GPS 硬件集成内容仅作为备选方案记录，
> 不是当前生产固件配置。
>
> **时间戳是本项目多处功能的共同地基**：
> - FIT 文件（[`plan/README.md`](./README.md) §3）需要 UTC 时间戳；
> - 与码表 FIT 合并对齐（[`plan/app-merge.md`](./app-merge.md) §4）需要绝对时间锚点；
> - 网页实时/历史数据（[`plan/wifi-web.md`](./wifi-web.md)）需要可读的时间。

---

## 目录

- [1. 问题背景](#1-问题背景)
- [2. 时间源方案对比](#2-时间源方案对比)
- [3. 当前决策：Wi-Fi SNTP 为唯一生产授时源](#3-当前决策wi-fi-sntp-为唯一生产授时源)
- [4. 移除 GPS 后的能力边界](#4-移除-gps-后的能力边界)
- [5. 硬件选型与接线](#5-硬件选型与接线)
- [6. 固件集成](#6-固件集成)
- [7. 统一时间源抽象](#7-统一时间源抽象)
- [8. 落地建议（分阶段）](#8-落地建议分阶段)
- [9. 待决策项](#9-待决策项)

---

## 1. 问题背景

### 当前实现（2026-11）

- 生产固件在 Wi-Fi 联网后使用 SNTP/NTP，并将同步间隔设为 300000 ms（5 分钟）。
- CSV `t_ms` 仍是开机后的毫秒相对时间；有效网络 UTC 作为 FIT 时间锚点。
- 若设备离线且未曾授时，保留固定 UTC FIT 兜底值。兜底只保证文件有时间基准，不能代表实际日期；时间页显示未授时时不得把它标成 NTP 已同步。
- GPS 接收器已从生产 PlatformIO 构建和运行路径移除。GPS 仍在本文件后续章节作为可能的未来备选，而非当前功能。

NTP 只在可联网时提供可信的绝对时间；五分钟同步周期减少长时间运行中的本机时钟偏差，但不保证每次同步成功或给出固定误差界。断网期间 ESP32 只能依赖本机时钟保持。

---

## 2. 时间源方案对比

| 方案 | 精度 | 成本 | 无需硬件？ | 断电后 | 适用场景 | 推荐度 |
| ---- | ---- | ---- | ---------- | ------ | -------- | ------ |
| **1. Wi-Fi NTP 对时** | 网络、服务器和本机时钟相关；需实测 | **0 元** | ✔ | 断电后需重新对时 | 有 Wi-Fi/互联网 | **当前采用** |
| **2. GPS/GNSS 模块** | 接收与 PPS 配置相关；需实测 | ~15~40 元 | ✘ | 每次开机需定位 | 户外无网 | 已从当前生产固件移除 |
| **3. RTC 模块 + 首次对时** | ±2ppm（年差 ~1 分钟，可校准） | ~5~10 元 | ✘ | ✔ 掉电走时 | 任何场景的"守时"底座 | ⭐⭐⭐⭐ |
| **4. BLE 从码表/手机取时** | ±50~500ms | 0 元 | ✔ | 需连设备 | 配合手机 APP | ⭐⭐⭐ |
| **5. 码表 FIT 事后对齐** | 事后离线精对齐 | 0 元 | ✔ | 不依赖 | 与码表合并场景 | ★（兜底，非实时） |

> 说明：方案 5 不是"实时时间源"，而是 app-merge 中用**功率互相关**做事后精对齐的兜底手段；
> 它能把两段序列对齐，但**不能给 ESP32 自身**提供实时绝对时间。

---

## 3. 当前决策：Wi-Fi SNTP 为唯一生产授时源

当前实现按用户确认采用 Wi-Fi SNTP，不与 GPS 自动切换或融合：

- Wi-Fi 建立连接时配置时间服务器并请求 NTP 对时；之后 SNTP 每 5 分钟重同步。
- 断开 Wi-Fi 后保留已同步系统时钟作为 holdover；断电冷启动且无网络则无真实 UTC。
- 当前保留固定 UTC 默认日期兜底用于 FIT 导出。该值会令 FIT 绝对日期不真实，离线记录应在后处理时按实际日期修正。
- 时间页面和设备页展示联网/授时状态；不显示 GPS 卫星或定位状态。

用户希望每 5 分钟校时，固件测试断言 SNTP 间隔为 300000 ms；实体网络环境下的真实同步准确度仍待验证。

---

## 4. 移除 GPS 后的能力边界

户外测试的核心是 **CdA**，公式里的 `v_g`（地速）当前来自功率计轮速
（`powerMeter.speed()`）。功率计是否广播速度取决于设备；不广播时地速缺失，速度相关 CdA 计算必须标记无效，不得用默认值伪造。

GPS 移除后当前固件不提供：

| 移除的 GPS 能力 | 当前替代/影响 |
| ---- | ---- |
| UTC 授时 | 有网时由 NTP 提供；离线冷启动用固定 UTC 兜底，日期可能错误 |
| 地速 `v_g` | 仅依赖 BLE 功率计速度广播；缺失时 CdA 不可计算 |
| 位置、轨迹、海拔和坡度 | 当前不采集、不导出；FIT 轨迹不完整 |
| PPS 硬件同步 | 不提供；NTP 的真实误差需实测 |

若后续户外无网场景必须取得真实日期或独立地速，再单独评估 GNSS 回归；这不属于当前需求实现。

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
| **当前** | Wi-Fi SNTP 每 5 分钟同步，保留固定 UTC FIT 兜底 | 有网授时；离线记录需纠正绝对日期 |
| **验证** | 实测 Wi-Fi 断连、重连、重复 NTP 同步与功率计速度广播 | 确认 holdover、重连及 CdA 输入有效性 |
| **可选未来方案** | 按需重新评估 GNSS 或 RTC | GNSS 可增加离线时间/速度/轨迹；RTC 可断电守时，均非当前固件功能 |

### 对本项目最实际的两步

当前不加 GPS 或 RTC。恢复其中任何一项前，应先明确户外无网的真实需求，并验证成本、安装和功耗收益。

---

## 9. 待决策项

1. **实测 NTP 重同步**：联网状态下每 5 分钟的同步成功率和绝对时间偏差是多少？
2. **功率计速度支持**：目标设备是否广播速度；不支持时本期 CdA 必须呈现为不可用。
3. **离线 FIT 兜底**：固定 UTC 日期产生的记录由哪一流程修正真实日期？
4. **未来硬件**：若需要无网时间、地速或轨迹，再重新讨论 GNSS；若要断电守时，再讨论 RTC。

---

## 附：文档关系

| 文档 | 面向 | 时间戳用途 |
| ---- | ---- | ---------- |
| [`plan/README.md`](./README.md) | 存储子系统 | FIT 时间戳、CSV `unixMs` 列 |
| [`plan/app-merge.md`](./app-merge.md) | 手机 APP | 与码表 FIT 对齐的绝对时间锚点 |
| [`plan/wifi-web.md`](./wifi-web.md) | PC 网页 | 实时/历史数据的可读时间 |
| **`plan/time-source.md`（本文）** | 时间地基 | GPS / NTP / RTC / BLE 的选型与统一抽象 |
