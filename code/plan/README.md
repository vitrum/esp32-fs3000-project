# SD 卡选型与日志 / FIT 文件写入方案

> 本文档针对 Aero Probe（ESP32-S3 骑行气动测试系统）的存储子系统，
> 给出 SD 卡选型建议、CSV 日志改进方案，以及 FIT 文件导出方案。

> **相关文档**
> - [BLE runtime architecture and verification](./ble-runtime.md)
> - [手机端 APP 与「ESP32 记录 × 码表 FIT」时间对齐合并方案](./app-merge.md)
>   —— BLE 对时 + 功率互相关，React Native APP。
> - [Wi-Fi 网页实时数据服务方案](./wifi-web.md)
>   —— 教练/采集员通过电脑浏览器远程实时察看分析。
> - [绝对时间源方案（GPS / NTP / RTC / BLE）](./time-source.md)
>   —— 为 FIT 时间戳、与码表对齐提供时间地基。
> - [I2C 总线分路方案（两片 FS3000 双总线）](./i2c-bus.md)
>   —— 同址 0x28 的电气隔离、引脚分配，明确不采用 AFS3000。
> - [电池电量和充电管理方案](./battery-power.md)
>   —— ADC 分压电量采样、充电状态、低电量保护与安全关机。

---

## 目录

- [1. SD 卡类型选择](#1-sd-卡类型选择)
- [2. 日志写入方案（CSV）](#2-日志写入方案csv)
- [3. FIT 文件方案](#3-fit-文件方案)
- [4. 推荐架构](#4-推荐架构)
- [5. 落地 Checklist](#5-落地-checklist)
- [6. 待决策项](#6-待决策项)

---

## 1. SD 卡类型选择

### 1.1 当前硬件约束

| 项目    | 现状                                                                 |
| ------- | -------------------------------------------------------------------- |
| 主控    | ESP32-S3（Waveshare ESP32-S3-Touch-LCD-2.8）                          |
| 总线    | **SPI 模式**（`SD.begin(PIN_SD_CS)` + `SPI.begin(...)`），**非 SDMMC** |
| 引脚    | SCLK=14, MISO=16, MOSI=17, CS=21（见 `src/config.h`）                 |
| 卡槽    | 板载 TF（microSD）卡槽                                                |
| 用途    | 1 行/秒 CSV，长时间骑行记录（数小时）                                 |

**关键判断**：当前使用 **SPI 模式**挂载，不是 4-bit SDMMC。这决定了选型上限与注意事项——
真正瓶颈在 MCU 与 SPI 时钟，而非卡本身的速度等级。

### 1.2 推荐卡类型

| 维度       | 推荐                              | 说明 |
| ---------- | --------------------------------- | ---- |
| **尺寸**   | **microSD (TF)**                  | 板载卡槽只有 TF |
| **标准**   | **microSDHC（4GB–32GB）**         | SPI 模式下 SDHC 兼容性最好；SDXC（>32GB）偶有兼容问题 |
| **容量**   | **8GB–16GB**                      | 数据量极小：1 行/秒 ≈ 每行 ~60 字节 ≈ **5MB/天**；8GB 已够用数十年，买大容量浪费 |
| **速度等级** | **Class 10 / U1（≥10MB/s）**    | **不需要 U3/V30**。SPI 模式下实际吞吐仅几百 KB/s~2MB/s，瓶颈在 MCU |
| **品牌**   | **SanDisk / Samsung / Kingston**  | 避免杂牌白卡——CRC 错误与假容量最常见 |
| **接口**   | **UHS-I**                         | 兼容，SPI 模式下自动降速协商 |
| **温度**   | 工业级/宽温（可选）               | 户外暴晒/低温环境更耐，非必须 |

> **结论**：买 **16GB Class10 的 SanDisk/Samsung microSDHC** 即可，不要贪大贪快。
> SPI 模式下关键不是速度等级，而是**兼容性**与**文件系统格式**。

### 1.3 比选卡更重要：文件系统

- **必须格式化为 FAT32**（`SD.h` 基于 FatFS，对 FAT32 支持最稳）。
- 32GB 以上默认 exFAT，Arduino `SD.h` **不支持 exFAT**——这是推荐 ≤32GB 并手动格式化的原因。
- **macOS 操作提示**：磁盘工具对 32GB 卡会默认选 exFAT，**需手动选 "MS-DOS (FAT)"**。
- **分配单元大小用 32KB**（默认可能 512B，导致 1 行/秒的小文件碎片多、写入变慢）。

### 1.4 性能与寿命注意事项

1. **SPI 时钟**：`SPI.begin()` 默认偏保守。可在 `SD.begin(CS, SPI, 40000000)` 显式给
   **20–40MHz**（走线短、卡好则 40MHz 通常稳；不稳退 20MHz）。
2. **掉电保护**：现有 `flush()` 每 5s 一次是**正确做法**（TF 热插拔/断电会丢最后几秒数据，
   甚至损坏 FAT）。建议再加一层：骑行结束前主动 `_f.close()`（见 §2.4）。
3. **写放大**：1 行/秒 + FAT 元数据更新，TF 卡寿命不受影响（数十年量级），无需 wear-leveling。
4. **散热**：SD 卡槽靠近 LCD 背光/ESP32 发热区时，长时间录制注意散热（可选散热片或工业级卡）。

---

## 2. 日志写入方案（CSV）

### 2.1 现状问题（先修）

1. **文件命名会覆盖**
   `csv_logger.cpp` 用 `millis() % 1000000` 命名，每 ~16.6 分钟循环一次，
   早期重启会**覆盖同名文件**。
2. **缺少标定元数据**
   CSV 只有列名，缺少固件版本、轮周长、总质量、Crr 等**后处理必需参数**，
   导致 `cda_postprocess.py` 每次需手动传 `--mass`、`--crr`、`--wheel-circ`，容易遗忘。
3. **无骑行分段/编号**，多圈数据无法区分。

### 2.2 文件名：自增序号（防覆盖）

```cpp
bool CsvLogger::begin() {
  SPI.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI);
  // 显式提高 SPI 时钟（走线短可用 40MHz，不稳退 20MHz）
  if (!SD.begin(PIN_SD_CS, SPI, 20000000)) {
    Serial.println("SD card init failed");
    _ok = false;
    return _ok;
  }

  // 自增序号：找到第一个不存在的 ride_XXXX.csv
  for (uint16_t i = 1; i < 9999; ++i) {
    snprintf(_name, sizeof(_name), "/ride_%04u.csv", i);
    if (!SD.exists(_name)) break;
  }
  // ... 打开文件并写表头 ...
}
```

### 2.3 元数据 sidecar（供后处理自动读参）

每个 `ride_XXXX.csv` 配套一个 `ride_XXXX.meta`，或在 CSV 顶部写 `#` 注释行：

```text
# aero-probe meta v1
firmware=0.1.0
wheel_circ_m=2.105
total_mass_kg=85.0
crr=0.004
start_epoch=1730000000        # 若无 RTC/网络填 0，后处理用文件名或相对时间
sample_period_ms=1000
columns=t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc
```

`cda_postprocess.py` 增加 meta 读取，缺省参数自动填充：

```python
def load_meta(csv_path):
    meta = {}
    with open(csv_path) as f:
        for line in f:
            if not line.startswith('#'):
                break
            k, _, v = line.lstrip('# ').strip().partition('=')
            if k:
                meta[k.strip()] = v.strip()
    return meta
# --mass / --crr / --wheel-circ 未显式传入时，从 meta 取值
```

### 2.4 缓冲与安全关闭

- 保持**每 5s flush**（现有逻辑），数据量小，无需重度缓冲。
- 增加 `CsvLogger::stop()`：写入完成后 `_f.close()`，UI 提供"结束记录 / 安全保存"入口。
- 若需更低功耗，可攒到每 10–30s flush（掉电只丢几十秒数据）。

### 2.5 建议增加列

当前列：

```text
t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc
```

**建议补充**：

| 列名        | 说明 |
| ----------- | ---- |
| `unixMs`    | 有 RTC/GPS 时的绝对时间 |
| `vCrossMps` | 侧风分量（双传感器探针才有意义） |
| `windValid` | 0/1，标记该窗口风速是否可信 |
| `rollDeg`   | WT9011DCL 同时输出 Roll/Yaw，目前只取了 Pitch |

> 注意：新增列会**影响 `cda_postprocess.py` 的旧数据兼容**。
> 脚本用 `DictReader` 按列名读取，**新增列安全**，但需保证列名不重复、顺序明确。
> 现有脚本已把 `postureDeg` / `powerSrc` 设计为可选列，扩展友好。

---

## 3. FIT 文件方案

### 3.1 为什么要 FIT

FIT（Flexible & Interoperable Data Transfer，Garmin 协议）是骑行圈通用格式，
Garmin / Wahoo / Karoo / Strava / GoldenCheetah / Intervals.icu 均可直接导入。
相比 CSV：

- 结构化二进制，含 session / lap / record 层级、时间戳、明确单位。
- 可直接装载 **功率、心率、速度、GPS、开发者自定义字段**。
- 平台自动出图，无需后处理脚本。

**定位**：
- **FIT** = 面向用户/分享的产物（可导入码表平台）。
- **CSV** = 面向科研后处理的原始数据。

两者并行写即可（数据量都极小）。

### 3.2 方案 A（推荐做 B，见 3.3）：固件运行时生写

在 PlatformIO 加入轻量 FIT 编码器：

| 库                         | 特点                          | 适合度 |
| -------------------------- | ----------------------------- | ------ |
| `libfit` / 官方 fit-sdk    | 功能全但重                    | 偏重   |
| **自写极简编码器**         | 只需 record/session，约 200–300 行 | ✅ 推荐 |
| `garmin/fit-cpp`           | 以解码为主，编码不友好        | 一般   |

**FIT 编码本身很简单**：
`14 字节 header` + 若干 `definition message` + `data message` 序列 + 末尾 `CRC`。
对于固定 schema（每秒一条 record），definition 只写一次，之后每条 record 固定 ~20 多字节。

最小骨架：

```cpp
// fit_writer.h —— 固定 schema 的极简 FIT 编码器
#pragma once
#include <Arduino.h>
#include <FS.h>

// FIT record message (global msg num 20) 需要的字段：
//   timestamp (uint32, field 253, unix - FIT_EPOCH)
//   position_lat/long (sint32, field 0/1) —— 有 GPS 才写
//   speed (uint32, field 6, scale 1000 → m/s)
//   power (uint16, field 7)
//   cadence (uint8, field 4)
//   developer fields: CdA, yawDeg, vAirMps, postureDeg

class FitWriter {
public:
  bool begin(fs::FS &fs, const char *path);
  bool writeRecord(uint32_t unixSec, float speedMps, uint16_t powerW,
                   uint8_t cadence, float cda, float yawDeg,
                   float vAirMps, float postureDeg);
  bool finish();   // 写 session + CRC，关闭文件
private:
  File _f;
  uint32_t _crc = 0;
  void writeHeader();
  void defineRecord();
  void writeDefWithDeveloperFields();
};
```

**FIT 时间戳**：`uint32` 秒，基准 `FIT_EPOCH = 631065600`（1989-12-31 00:00:00 UTC），
即 `fitTime = unix - 631065600`。**需要绝对时间源**——见 §3.4。

**CdA / yaw / vAir / posture** 属非标准量，使用 FIT 的
**Developer Data Fields**：在 `developer_data_id` + `field_description` message 中声明
（含 `field_name`、`units`、`native_field_num`），再在 record 中引用。
Garmin Connect、Intervals.icu 可显示；**Strava 不显示开发者字段**（但不会报错）。

### 3.3 方案 B：先存 CSV，离线转 FIT（更省事，建议先做）

1. 固件继续写 CSV（+ meta）。
2. 在 `tools/` 新增 `csv_to_fit.py`，使用 **`fit_tool`** 或 **Python `garmin-fit-sdk`** 转换。
3. 流程：骑行 → 拔卡 → `python csv_to_fit.py ride_0001.csv` → 得到 `.fit` → 导入平台。

**优点**：
- 固件无需引入 FIT 依赖，不占 Flash/RAM，无 CRC 风险。
- 转换脚本可复用 `cda_postprocess.py` 的模型，顺便把 CdA 作为开发者字段写入 FIT。
- 调试期不锁死固定资产 schema。

**缺点**：需要电脑一步转换。

```python
# tools/csv_to_fit.py 骨架
from fit_tool.fit_file_builder import FitFileBuilder
from fit_tool.profile.messages.record_message import RecordMessage
# ... 读 CSV，逐行构造 RecordMessage，
#     CdA / yaw 等用 DeveloperField 写入
```

### 3.4 时间同步（FIT 的前提）

当前**没有 RTC，也没有网络时间**（UI 的时间是 `millis()` 格式化的相对时间）。
FIT 依赖绝对时间戳，必须解决：

| 方案                              | 说明 |
| --------------------------------- | ---- |
| **BLE 取手机/码表时间**           | 部分码表提供 Current Time Service (0x1805)，连接后同步 |
| **加 GNSS 模块（ATGM336H 等）**   | 有星即有 UTC，附带速度与位置，FIT 更完整（推荐二期） |
| **加 RTC（DS3231 / PCF8563）**    | 便宜简单，挂 I2C2 即可；需开机对时 |
| **降级：用启动时间当相对时间**    | FIT 用 `FIT_EPOCH + startup_unix`，可导入出图（不精确但可用） |

> **建议**：二期加 **PCF8563 RTC（走 I2C2，GPIO15/18 已预留）**，几块钱，同时解决
> FIT 时间戳与 CSV 的 `unixMs` 列；或直接上 GNSS 模块，一箭三雕（时间 + 速度 + 轨迹）。

---

## 4. 推荐架构

```
固件运行时
├── ride_0001.csv        ← 原始科研数据（现有列 + meta）
└── ride_0001.meta       ← 标定参数

           ↓ 电脑后处理
tools/
├── cda_postprocess.py   ← 现有：算 CdA、偏航/姿态分箱
└── csv_to_fit.py        ← 新增：CSV → FIT（含 CdA 开发者字段）
           ↓
       ride_0001.fit       ← 导入 Strava / Garmin / Intervals.icu
```

若坚持**固件直出 FIT**：采用 §3.2 极简自写编码器，只写 `record` + `session`，
不写 GPS（除非接入 GNSS 模块）。

---

## 5. 落地 Checklist

### 硬件

- [ ] 16GB Class10 microSDHC（SanDisk / Samsung），格式化为 **FAT32**，簇 32KB
- [ ] （二期）PCF8563 RTC 接 I2C2，或 GNSS 模块

### CSV 日志（近期）

- [ ] 文件名改自增序号，消除覆盖
- [ ] 新增 `.meta` sidecar（固件版本、轮周长、质量、Crr）
- [ ] `cda_postprocess.py` 支持读取 meta 自动填参
- [ ] 可选：增加 `windValid` / `vCrossMps` / `rollDeg` 列
- [ ] 增加"结束记录 / 安全关闭"入口

### FIT（中期）

- [ ] 确定方案 A（固件直写）或 B（离线转换）——**建议先做 B**
- [ ] 解决绝对时间源（RTC / GNSS / BLE）
- [ ] CdA / yaw / vAir / posture 使用 Developer Field 声明

---

## 6. 待决策项

1. **FIT 是固件直出（方案 A）还是电脑转换（方案 B）？**
   ——建议 B，除非要做到"插卡即用、无需电脑"。
2. **是否加 RTC / GNSS？**
   ——决定 FIT 能否有正确时间戳，也影响未来功能。
3. **SD 卡 SPI 时钟**：走线多长？可将 `SD.begin` 改为显式 20MHz 并加超时处理。
