# 手机端 APP 与「ESP32 记录 × 码表 FIT」时间对齐合并方案

> 本文档描述 Aero Probe 配套的手机 APP 设计，核心功能：
> 1. BLE 实时查看数据；
> 2. 选择设备上某次记录；
> 3. 将该记录与码表导出的 FIT 文件合并，**并对齐时间点**。
>
> 主线技术选型：**ESP32 侧 BLE 对时 + 功率序列互相关精对齐**；**APP 框架 React Native**。

---

## 目录

- [1. 需求与总体架构](#1-需求与总体架构)
- [2. 时间问题的本质](#2-时间问题的本质)
- [3. ESP32 侧改造：GATT Server](#3-esp32-侧改造gatt-server)
- [4. 时间对齐方案（两阶段）](#4-时间对齐方案两阶段)
- [5. APP 功能设计（React Native）](#5-app-功能设计react-native)
- [6. 合并算法](#6-合并算法)
- [7. 数据格式与协议](#7-数据格式与协议)
- [8. 落地 Checklist](#8-落地-checklist)
- [9. 待决策项](#9-待决策项)

---

## 1. 需求与总体架构

### 功能需求

| 编号 | 功能 | 说明 |
| ---- | ---- | ---- |
| F1 | **实时数据查看** | 手机通过 BLE 连接 ESP32，实时显示风速、功率、踏频、车速、CdA、姿态角、ρ 等 |
| F2 | **历史记录列表** | APP 读取 ESP32 SD 上的记录文件（`ride_XXXX.csv` / FIT），列出供选择 |
| F3 | **记录选择与下载** | 选中某次记录，从 ESP32 传输到手机 |
| F4 | **码表 FIT 导入** | 用户从手机/云盘选取码表导出的 `.fit` 文件 |
| F5 | **时间对齐合并** | 将 ESP32 记录与码表 FIT 按时间轴对齐、合并为统一数据 |
| F6 | **导出 / 分享** | 合并结果导出为 FIT / CSV，可分享到 Strava、Intervals.icu 等 |

### 物理拓扑

```
   码表（Garmin/Wahoo/…）                手机 APP (React Native)
      │  FIT 文件（事后导出）                 │
      │                                     │ BLE (GATT)
      └──────────► 手机 ◄────────────────── ESP32-S3
                  ▲                            │
                  │                            ├─ 风速 FS3000
            码表数据（绝对UTC）                 ├─ 功率计/姿态（GATT client）
                                              └─ SD 卡记录（相对 t_ms）
```

**关键矛盾**：

- 码表 FIT 时间戳 = **绝对 UTC**（高精度）。
- ESP32 记录时间戳 = **`millis()` 相对时间**（开机为 0）。
- 要合并，必须建立 `ESP32 相对时间 ↔ 绝对 UTC` 的映射。

---

## 2. 时间问题的本质

记：

- ESP32 第 `i` 个采样点：相对时间 `t_i`（ms，`millis()`）。
- 码表第 `j` 个采样点：绝对时间 `T_j`（UTC 秒，FIT 格式）。

目标：求 f，使 `T ≈ f(t)`。由于两台设备的晶振频率略有差异（ppm 级），
不能只用偏移，应是**仿射模型**：

```
T(t) = a · t + b
```

- `b` = 时间偏移（时钟零点差）—— 由 **BLE 对时** 粗定。
- `a` = 时钟频率比（晶振偏差 + 采样抖动）—— 由 **功率互相关** 精修。

两阶段：

1. **阶段一：BLE 对时**——APP 连接时下发手机绝对时间，ESP32 记录「对时锚点」，得到 `b` 的粗估（对时误差通常在 ±50ms~±500ms，取决于 BLE 连接间隔与链路延迟）。
2. **阶段二：功率互相关**——利用 ESP32 记录的功率序列与码表 FIT 的功率序列做互相关，找最佳时移与缩放，精修 `a`、`b` 到采样级（±1s 内）。

> 之所以能用功率对齐：码表自己记录的功率（来自同一功率计）与 ESP32 从同一功率计收到的功率**同源**，
> 序列高度相关，是理想的对齐信号。车速、踏频可作为备用/交叉校验信号。

---

## 3. ESP32 侧改造：GATT Server

### 3.1 现状与新增

- **现状**：ESP32 是 **GATT Central**（`BlePowerMeter` / `BlePosture` 连接功率计、姿态传感器）。
- **新增**：需要一个 **GATT Peripheral（Server）** 让手机连接。

**NimBLE 支持 Central + Peripheral 并存**，可复用同一个 `NimBLEDevice::init()`。
但注意与现有 ANT+ coexist 的射频资源竞争（见 §3.4）。

### 3.2 建议的服务/特征设计

采用**自定义 128-bit UUID**，或结合标准服务 + 自定义服务：

| 服务 | UUID | 用途 |
| ---- | ---- | ---- |
| **实时数据通知** | 自定义 `0xA000`（示例） | Notify：打包实时指标，1Hz |
| **记录列表** | 自定义 `0xA001` | Read/Write：分页返回文件清单 |
| **文件传输** | 自定义 `0xA002` | 请求 + 分块数据（见 §7.2） |
| **控制** | 自定义 `0xA003` | Write：开始/停止记录、删除文件、对时触发 |

实时数据 Notify 载荷（示意，二进制小端）：

```text
[0]     version
[1..4]  t_ms (uint32)                 ESP32 相对时间
[5..6]  powerW (uint16)
[7]     cadenceRpm (uint8)
[8..9]  speedMps * 100 (uint16)
[10..11] vAirMps * 100 (uint16)
[12..13] yawDeg * 10 (int16)
[14..15] postureDeg * 10 (int16)
[16..17] cda * 1000 (uint16)          在线估算（示意）
[18..19] rho * 1000 (uint16)
[20]    powerSrc (uint8)
[21]    flags (windValid 等)
```

### 3.3 代码结构建议

新增 `src/ble_server.{h,cpp}`：

```cpp
// ble_server.h —— 面向手机 APP 的 GATT Server
class BleServer {
public:
  void begin(const char *deviceName);        // 广播 + 建立服务
  bool hasClient() const;                    // 手机是否已连接
  void notifyTelemetry(...);                 // 1Hz 推送实时数据

  // 对时：APP 写入手机 unix 秒 + 本机 t_ms，两者配对存为锚点
  bool getTimeSyncAnchor(int64_t &unixSec, uint32_t &tMs) const;

  // 文件接口（供 APP 请求列表 / 分块读取）
  // ...
private:
  static void onTimeSyncWrite(NimBLECharacteristic *, NimBLEConnInfo &);
  // ... 其余特征回调 ...
};
```

在 `main.cpp` `setup()` 中，于 `powerMeter.begin()` 之后调用 `bleServer.begin(BLE_DEVICE_NAME)`，
并在 `loop()` 中 1Hz 推送遥测。

### 3.4 射频共存注意事项（重要）

ESP32-S3 单射频，当前已运行：

- NimBLE Central（连功率计/姿态）
- **ANT+ coexist**（`lib/ant`，在 BLE 被动扫描窗口内收包）

再叠加 **GATT Server** 后，射频调度更紧张。建议：

- **APP 连接时暂停 ANT+**（`antPower.stop()`），或缩短 ANT 窗口；
- 实时 Notify 频率控制在 **1Hz**，不要高频刷；
- 文件传输是**突发大数据**，传输前可临时降级/暂停实时推送，以保证吞吐。

---

## 4. 时间对齐方案（两阶段）

### 4.1 阶段一：BLE 对时——确定偏移 `b`

**触发**：APP 连接成功后立即执行一次；开始记录时再执行一次。

**协议（示意）**：

```text
APP  → ESP32（Write 到 0xA003 控制特征）
   payload: cmd=TIME_SYNC, appUnixMs=1730000000123, appTick=... 
ESP32 处理：
   1. 立刻读取本机 t_now = millis()
   2. 保存锚点 { appUnixMs, t_now }（可补偿写入链路延迟的估计值）
   3. 回写 ACK 到 Notify，携带 t_now 供 APP 计算往返延迟
```

**精度增强（可选，推荐）**：仿照 NTP 的四时间戳法。

```text
T1 = APP 发送时刻（APP 本地 ms）
T2 = ESP32 收到时刻（转成 APP 时基需先粗估，或用 t_ms）
T3 = ESP32 回复时刻（t_ms）
T4 = APP 收到时刻（APP 本地 ms）

往返延迟 RTT = (T4 - T1) - (T3 - T2)
单程估计   = RTT / 2
偏移 b     ≈ 用 (T2 + RTT/2) 对齐 appUnixMs 与 t_ms
```

重复 3–5 次取中位数，抵消 BLE 调度抖动。

**结果**：一个锚点 `(unixSec_b, t_ms_b)`，即 `b = unixSec_b - t_ms_b/1000`。

### 4.2 阶段二：功率互相关——精修 `a`、`b`

**前提**：ESP32 记录里包含**功率列**（现有 CSV 已有 `powerW`），码表 FIT 也含功率。

**步骤**：

1. **重采样到统一网格**：将两段功率序列按各自的估计时间轴，重采样到 1Hz。
2. **粗对齐**：用 `b` 把 ESP32 序列平移到绝对时间轴，取重叠区间。
3. **互相关搜索**：在 `±Δ`（例如 ±60s）范围内滑动，计算归一化互相关，
   找峰值 → 最佳时移 `shift`。
4. **频率比估计 `a`**：用「分段互相关」——把重叠区切成若干段（如 5 分钟），
   各自求局部时移，对 (段中心时间, 时移) 做线性回归，**斜率即频差**，修正 `a`。
5. **迭代**：用修正后的 `a` 重采样，再互相关，直到收敛（通常 2–3 轮）。

```python
# 伪代码
def align(esp_power, esp_t, fit_power, fit_t, b_est):
    a = 1.0
    for _ in range(3):
        t_est = a * esp_t + b_est          # 映射到绝对时间
        shift = xcorr_best_shift(t_est, esp_power, fit_t, fit_power, max_lag=60)
        b_est += shift
        # 分段回归修正 a
        a += estimate_freq_offset(t_est, esp_power, fit_t, fit_power)
    return a, b_est
```

**对齐质量评估**：

- 输出互相关峰值（0~1）与对齐后**功率残差**（RMSE）。
- 峰值过低（如 <0.6）或残差过大 → 提示用户「对齐可疑」，建议重新对时或改用手动锚点。

### 4.3 兜底与人工干预

- **手动锚点**：APP 提供 UI，用户在两设备上同时触发一个「标记」（例如同时按下码表的 Lap 键），
  APP 记录该瞬间的 `t_ms`，作为硬约束偏移。
- **速度/踏频交叉校验**：用第二路信号验证对齐结果。

---

## 5. APP 功能设计（React Native）

### 5.1 技术栈

| 层 | 选型 | 说明 |
| -- | ---- | ---- |
| 框架 | **React Native** | 跨平台 iOS/Android |
| BLE | **`react-native-ble-plx`** | 成熟、支持 Notify/Read/Write 与 MTU 协商 |
| 文件 | **`react-native-fs`** | 读写本地 FIT/CSV |
| 解析 FIT | **JS FIT 解析库** 或 WebAssembly 版 | 解析码表 FIT；或自写轻量解析器 |
| 图表 | **`victory-native` / `react-native-svg`** | 实时曲线与合并对比图 |
| 状态管理 | Redux Toolkit / Zustand | 会话与数据流 |
| 后台 | 尽量前台运行（BLE + 大数据传输） | 后台 BLE 限制多，传输期建议保持前台 |

### 5.2 页面结构

```
┌─────────────┐
│  连接/扫描   │  BLE 设备列表，连接 Aero Probe
├─────────────┤
│  实时数据    │  F1：仪表盘（功率/风速/CdA/踏频/姿态），1Hz 刷新
│             │  含记录开始/停止按钮
├─────────────┤
│  记录列表    │  F2：从 ESP32 拉取 ride_XXXX 列表（大小/时长/日期）
├─────────────┤
│  记录详情    │  选中后下载，预览图表；显示对时锚点与精度
├─────────────┤
│  合并页      │  F4/F5：导入码表 FIT → 自动对齐 → 展示双序列叠加
│             │  显示对齐质量（峰值/残差）；支持手动微调偏移
├─────────────┤
│  导出/分享    │  F6：导出 FIT / CSV，分享
└─────────────┘
```

### 5.3 关键交互流程

```
连接 ESP32
  └─ 自动 TIME_SYNC（NTP 四时间戳法，取中位数）
      └─ 写入锚点 b

实时页 ──[开始记录]──► ESP32 记录 + 打锚点
        ──[停止记录]──► ESP32 关闭文件，回传文件名

记录页 ──[选择 ride_0007]──► 下载到手机
        ──[导入码表 ride.fit]──►
            ├─ 阶段一：用锚点 b 平移
            ├─ 阶段二：功率互相关精对齐
            ├─ 质量评估：峰值/残差
            └─ 展示：双功率曲线叠加 + 其余指标对齐
        ──[手动微调]──► 拖动偏移量
        ──[导出]──► 合并 FIT / CSV
```

### 5.4 实时数据处理

- BLE Notify 载荷按 §3.2 解析为结构化对象。
- 前端以环形缓冲保存最近 N 秒，供实时曲线。
- 记录会话：APP 侧可选记录「对时锚点历史」和「本地镜像」，便于与日后下载的 ESP32 文件核对。

---

## 6. 合并算法

### 6.1 合并目标

在统一绝对时间轴上，产出每 1Hz 一行、包含两侧全部指标的合并表：

```text
unix_sec, powerW, cadenceRpm, speedMps, vAirMps, yawDeg, postureDeg,
rhoKgM3, cda_m2, source(esp|fit|both), align_offset_ms, align_quality
```

### 6.2 对齐与重采样流程

```
输入：
  ESP 记录：[(t_ms_i, power_i, cad_i, vAir_i, yaw_i, posture_i, rho_i)]
  FIT 记录：[(unix_j, power_j, cad_j, speed_j, lat_j, lon_j, ...)]
  锚点： (unix_b, t_ms_b)

1. T_i = a·(t_ms_i/1000) + b           // a,b 来自 §4
2. 取重叠时间区间 [T_start, T_end]
3. 生成绝对时间网格：G = [T_start, T_start+1, ..., T_end]（1Hz）
4. 对每侧序列按 G 做插值（线性/最近邻）：
     esp_on(G), fit_on(G)
5. 对同为功率/踏频的字段，可取「ESP 值」为主（含气动专用量），
   FIT 补充 ESP 缺失的（GPS、心率、档位等）。
6. 输出合并表
```

### 6.3 冲突处理

| 场景 | 处理 |
| ---- | ---- |
| 两序列功率差异大 | 标记 `align_quality`，保留两侧原值便于诊断 |
| ESP 与 FIT 采样率不同 | 统一重采样到 1Hz（FIT 也可能是 1Hz，或更高） |
| 有 GPS/心率仅 FIT 有 | 直接采用 FIT 值 |
| CdA/风速仅 ESP 有 | 采用 ESP 值 |
| 对齐区间外 | 分别保留单侧数据，标 `source` |

### 6.4 输出

- **合并 CSV**：便于科研后处理（可并入 `tools/cda_postprocess.py`）。
- **合并 FIT**：把 ESP 的 CdA/风速/姿态作为 Developer Field，叠加码表的 GPS/心率/功率，
  得到一份「带气动数据」的完整骑行文件，可上传 Strava/Intervals.icu。

---

## 7. 数据格式与协议

### 7.1 锚点元数据（ESP32 侧持久化）

ESP32 每次对时后，把锚点写入记录的 meta（配合 `plan/README.md` §2.3 的 meta sidecar）：

```text
# aero-probe meta v1
firmware=0.1.0
time_sync_unix_ms=1730000000123     # 对时时刻的绝对时间
time_sync_t_ms=12345                # 同刻的 millis()
time_sync_rtt_ms=42                 # 对时往返延迟（质量指示）
time_sync_method=ble_ntp4           # 方法标记
```

### 7.2 文件传输协议（BLE 分块，示意）

BLE MTU 有限（NimBLE 可协商到 ~247B），大文件必须分块传输。建议：

```text
APP  → Write(0xA002): { cmd=OPEN, path="/ride_0007.csv" }
ESP  → Notify(0xA002): { len=123456, crc32=0x... }
APP  → Write(0xA002): { cmd=READ, offset=0, size=512 }
ESP  → Notify(0xA002): { offset=0, data=[...] }     // 分片，直到传完
APP  → Write(0xA002): { cmd=CLOSE }
APP 校验 CRC32
```

增强建议：

- 传输前 **协商 MTU**（`react-native-ble-plx` 的 `requestMTU`）。
- 分片顺序确认 + 重传（简单 ARQ，或依赖 BLE 的可靠链路层 + 每片确认）。
- 大文件考虑 **压缩**（CSV 可 gzip）或**只传合并所需列**。
- 传输期间**暂停实时 Notify**，避免抢占带宽。

---

## 8. 落地 Checklist

### ESP32 固件

- [ ] 新增 `src/ble_server.{h,cpp}`：GATT Server（实时 Notify + 记录列表 + 文件传输 + 对时）
- [ ] 对时锚点写入 meta（与 `plan/README.md` §2.3 统一）
- [ ] 处理射频共存：APP 连接时暂停/降级 ANT+
- [ ] 文件分块读取接口 + CRC32
- [ ] 1Hz 遥测 Notify 载荷定义

### APP（React Native）

- [ ] BLE 连接与配对流程（`react-native-ble-plx`）
- [ ] NTP 四时间戳对时实现 + 中位数
- [ ] 实时数据仪表盘
- [ ] 记录列表 / 下载
- [ ] 码表 FIT 导入与解析
- [ ] 功率互相关对齐 + 频差回归
- [ ] 对齐质量可视化 + 手动微调
- [ ] 合并导出（FIT / CSV）
- [ ] 分享

### 验证

- [ ] 单元测试：互相关与回归（离线，用录制的两序列）
- [ ] 端到端：同一次骑行，ESP + 码表分别记录 → 对齐 → 误差 < 1s
- [ ] 异常场景：风速丢包、BLE 断连、时钟漂移大

---

## 9. 待决策项

1. **ESP32 是否需要 RTC/GNSS** 提供关机后的绝对时间？
   有则对时锚点可跨重启复用；无则每次开机都要重新对时。
   （与 `plan/README.md` §3.4 时间源问题同源）
2. **FIT 解析放在哪端**？APP 端解析，还是与 `tools/csv_to_fit.py` 共用 Python 逻辑？
3. **合并产物主格式**：FIT 还是 CSV 优先？
4. **实时 Notify 载荷**是否需要包含 CdA（在线估算尚存模型问题，见项目分析 P0-1）？
5. **文件传输协议**：简单分片确认，还是引入轻量 ARQ / 压缩？

---

## 附：与既有文档的关系

- 存储与 CSV/FIT 基础方案见 [`plan/README.md`](./README.md)。
- 本文档的 meta sidecar（§7.1）扩展了 `README.md` §2.3。
- 时间源问题（RTC/GNSS）在 `README.md` §3.4 与本文档 §9 中均需解决。
