# Wi-Fi 网页实时数据服务方案

> 本文档描述 Aero Probe 通过 **Wi-Fi 提供网页服务**的方案：
> 在固定室内 / 室外跑道绕圈测试场景下，教练与数据采集人员
> 可通过电脑浏览器**远程实时察看与分析**数据，无需待在设备近旁。
>
> 与 [手机 APP 方案](./app-merge.md)（BLE 近场）互补：Wi-Fi 面向**多人、远距离、PC 端**。

---

## 目录

- [1. 场景与需求](#1-场景与需求)
- [2. 核心约束：射频共存](#2-核心约束射频共存)
- [3. 网络拓扑方案](#3-网络拓扑方案)
- [4. 后台服务架构](#4-后台服务架构)
- [5. Web 前端](#5-web-前端)
- [6. 接口协议](#6-接口协议)
- [7. 与现有模块的集成](#7-与现有模块的集成)
- [8. 落地 Checklist](#8-落地-checklist)
- [9. 待决策项](#9-待决策项)
- [10. 实现状态（已落地）](#10-实现状态已落地)
  - [10.7 使用与配置步骤](#107-使用与配置步骤)

---

## 1. 场景与需求

### 目标场景

| 场景             | 描述 |
| ---------------- | ---- |
| **室内骑行台**   | 固定踩踏/风洞式测试，教练在数米外 PC 上实时看数据 |
| **室外跑道绕圈** | 设备装车，教练/采集员在场地边用笔记本实时观察，不用靠近 |
| **多设备/多人**  | 一个教练组可能要同时看多个数据源，或投屏分析 |

### 功能需求

| 编号 | 功能 | 说明 |
| ---- | ---- | ---- |
| W1 | **实时数据 Web 展示** | 浏览器打开即看：功率、风速、CdA、踏频、车速、姿态等，1Hz 刷新 |
| W2 | **实时曲线** | 时序曲线（功率/CdA/风速），可缩放，长测时能看趋势 |
| W3 | **多客户端并发** | 至少 2–4 个浏览器同时连接不卡（教练 + 采集员 + 投屏） |
| W4 | **历史记录访问** | 列出 SD 卡记录，下载 CSV/FIT（复用 app-merge 的文件接口思路） |
| W5 | **控制/配置** | 远程开始/停止记录、切换功率源、查看设备状态 |
| W6 | **低延迟** | 实时性接近屏幕（延迟 < 1s 量级） |

### 设计原则

- **不影响现有功能**：屏幕 UI、BLE/ANT+ 采集、SD 日志照常运行。
- **可选开启**：Wi-Fi 是"按需启用"模式（绕圈/室内测试时打开），由设备设置页面上的开关手动启动。
- **渐进增强**：先做"只读实时看"，再做控制与文件下载。

---

## 2. 核心约束：射频共存

> **这是本方案最重要的技术点，先讲清楚再谈实现。**

ESP32-S3 有 **Wi-Fi（2.4GHz）与 BLE（2.4GHz）共用同一个射频前端**，硬件上不能真正并行收发，
需要在时域上分片（coexistence / time-division）。当前固件已经同时运行：

- **NimBLE Central**：连接 BLE 功率计 + 姿态传感器；
- **ANT+**（`lib/ant`）：借用 BLE 被动扫描窗口收包（coexist 模式）。

**再加 Wi-Fi 服务端后，变成三路争抢：**

```
        ┌───────────────────────────────────────────┐
        │         ESP32-S3 单射频 (2.4GHz)            │
        ├───────────┬──────────────┬────────────────┤
        │ NimBLE    │  ANT+ (coexist)│  Wi-Fi STA/AP  │
        │ Central   │                │                │
        └───────────┴──────────────┴────────────────┘
                   时域分片，互相影响
```

### 影响与对策

| 影响 | 说明 | 对策 |
| ---- | ---- | ---- |
| **BLE 连接周期性丢包/延迟** | Wi-Fi 收发包时 BLE 让步 | 提高连接间隔、容忍偶发丢包（现有 `disconnectCount` 监控） |
| **ANT+ 收包率下降** | ANT 借用的扫描窗口被 Wi-Fi 抢占 | ANT+ 与 Wi-Fi **不同时高负载**；或测试时只用其一 |
| **Wi-Fi 吞吐下降** | BLE 连接活跃时 Wi-Fi 带宽降低 | 实时数据 1Hz 很小，够用；大文件下载放低峰或暂停 BLE |
| **功耗上升** | Wi-Fi 常开功耗高 | 明确"测试时开启"模式，平时关闭 |

### 关键建议

1. **明确三种射频模式的取舍**（见 §3.3）：用户应知道"开 Wi-Fi 时 ANT+ 可能不稳"。
2. **实时数据走 WebSocket**，流量极小（1Hz × 几十字节），对射频压力可忽略。
3. **大批量文件下载（历史 CSV/FIT）**：可与实时 BLE 采集错开；下载时提示"此时 BLE 可能短暂受影响"。
4. **天线与布局**：设备装车/支架时，注意天线朝向，Wi-Fi 覆盖范围在跑道场景要实测。

> 如果绕圈测试时**只需要 Wi-Fi 看数据，不再需要 ANT+**，可以设计成
> "Wi-Fi 模式自动暂停 ANT+"，换取 Wi-Fi 稳定性。这应做成用户可选项（见 §7.2 与 §9）。

---

## 3. 网络拓扑方案

### 3.1 三种拓扑可选

| 拓扑 | 描述 | 适用 |
| ---- | ---- | ---- |
| **A. SoftAP（自建热点）** | ESP32 开热点，PC 直连 | **跑道/户外首选**：不依赖外部网络，随开随用 |
| **B. STA（连本地路由）** | ESP32 连现场 Wi-Fi，PC 同网段访问 | 室内骑行台/有固定网络时，覆盖更远 |
| **C. AP+STA（双模）** | 既开热点又连路由 | 灵活但实现复杂，暂缓 |

**推荐**：**A（SoftAP）为主，B 为可选**。跑道场景没有现成 Wi-Fi，SoftAP 最省事；
室内可选 STA 以获得更大覆盖和互联网（若要做云端上报）。

### 3.2 SoftAP 方案细节

```text
ESP32-S3 (SoftAP)
  SSID: "AeroProbe-XXXX"   (XXXX = MAC 后四位)
  Pass: 可配置（默认 8 位随机，显示在设备屏幕上）
  IP:   192.168.4.1
  DHCP: 给 PC/手机分配 192.168.4.2+
  mDNS: aeroprobe.local  (方便访问)
```

- 设备屏幕上显示 **SSID / 密码 / IP**（利用现有 LCD），方便现场人员连接。
- 支持 **mDNS**（`aeroprobe.local`），PC 输入域名即可，不用记 IP。
- 可选 **二维码**（LVGL 画一个含 `WIFI:` 配置串的二维码，手机扫码直连）——加分项。

### 3.3 射频模式配置

建议在固件加一个 **"无线模式"设置项**（放在现有 SETTINGS 面板里）：

| 模式 | BLE Central | ANT+ | Wi-Fi | 场景 |
| ---- | ----------- | ---- | ----- | ---- |
| `采集优先` | ✔ | ✔ | ✘ | 纯记录，不看实时 |
| `实时优先` | ✔ | ✘ | ✔ | 绕圈看数据（推荐） |
| `全开` | ✔ | ✔ | ✔ | 都想要（可能各有掉包，谨慎） |

---

## 4. 后台服务架构

### 4.1 技术选型

| 层 | 选型 | 理由 |
| -- | ---- | ---- |
| HTTP 服务 | **ESP-IDF `esp_http_server`** 或 **`ESPAsyncWebServer`** | 异步、非阻塞，适合与现有 loop 共存 |
| 实时推送 | **WebSocket**（`esp_http_server` 自带 / AsyncWebServer 支持） | 低延迟、双向、流量小 |
| 文件系统挂载 | **`SD` (FatFS)** + 网页静态资源放 LittleFS/SPIFFS | 前端页面存 Flash，记录文件存 SD |
| JSON | **ArduinoJson** | 序列化实时数据、文件列表 |
| mDNS | **`ESPmDNS`** | `aeroprobe.local` |
| 网络栈 | **WiFi** + **AsyncTCP** | 非阻塞 |

> **注意**：`ESPAsyncWebServer` 依赖 `AsyncTCP`，与 Arduino-ESP32 2.0.14 兼容良好，
> 但要核对与 NimBLE 共存的稳定性。若求稳，可直接用 IDF 的 `esp_http_server`（阻断更少依赖）。

### 4.2 新增模块

```
src/
├── wifi_web.{h,cpp}      ← 新增：Wi-Fi 连接/热点 + Web 服务器 + WebSocket
│   ├── begin(config Mode)          // SoftAP 或 STA
│   ├── tick()                      // 泵事件（非阻塞）
│   ├── broadcastTelemetry(...)     // 1Hz 推送实时数据
│   └── registerRoutes()            // 注册 HTTP / WS 路由
└── web_assets.h          ← 前端页面（gzip 后内嵌数组 或 存 LittleFS）
```

### 4.3 与 loop 的集成

现有 `loop()` 是单线程 `millis()` 调度。新增 Wi-Fi 后**不要让网络阻塞主循环**：

```cpp
void loop() {
  uint32_t now = millis();

  // ... 现有采集 / 日志 / UI 逻辑 ...

  // 新增：Wi-Fi 事件泵（非阻塞）
  wifiWeb.tick();

  // 新增：1Hz 广播实时数据到所有 WebSocket 客户端
  if (now - lastWebMs >= LOG_WINDOW_MS) {
    lastWebMs = now;
    wifiWeb.broadcastTelemetry(/* 与 lcd.update 相同的一组指标 */);
  }
}
```

- 实时数据在 §7 的同一处（`lastUiMs` 块）生成，复用 `lcd.update()` 的入参，避免重复计算。
- 文件下载等重操作走异步回调，不占用主循环。

### 4.4 多客户端

- WebSocket 维护客户端列表，`broadcastTelemetry` 向所有活跃连接发送。
- 限制最大连接数（如 8），超过拒绝并提示，防止内存耗尽（ESP32 内存有限）。
- 静态资源缓存（浏览器端 `Cache-Control`），减少重复传输。

---

## 5. Web 前端

### 5.1 技术选型

| 选项 | 说明 | 建议 |
| ---- | ---- | ---- |
| **原生 HTML/JS（单文件）** | 内嵌到固件，无构建 | ✅ 首选，体积小、零依赖 |
| Vue/React + 构建 | 功能强但产物大 | 二期可选 |
| 图表 | **Chart.js / uPlot（轻量）** | uPlot 更省资源，适合实时曲线 |

**推荐**：单文件 HTML + 原生 JS + **uPlot**（几十 KB），gzip 后内嵌或存 LittleFS。

### 5.2 页面设计

```
┌──────────────────────────────────────────────┐
│  AeroProbe 实时监控          ● 已连接 (WiFi)   │
├──────────────┬───────────────────────────────┤
│  数值卡片区    │  实时曲线区（功率 / CdA / 风速）│
│  Power 245 W  │  ╱╲    ╱╲    ╱╲               │
│  CdA  0.28 m² │ ╱  ╲__╱  ╲__╱  ╲              │
│  Wind 32 km/h │                               │
│  Cad   92 rpm │  [可缩放 / 暂停 / 截图导出]    │
│  Speed 34 km/h│                               │
│  Posture AERO │                               │
├──────────────┴───────────────────────────────┤
│  历史记录：ride_0007.csv  [下载] [导出FIT]     │
│  控制：[开始记录] [停止] [功率源: BLE ▼]       │
├──────────────────────────────────────────────┤
│  状态：BLE ✔  ANT+ ✔  SD ✔  ρ=1.225  运行 12:34│
└──────────────────────────────────────────────┘
```

- **响应式**：PC 大屏、平板、手机都能看（教练可能用平板）。
- **大字号**：现场远距离可读（投射到电视/大屏时更清晰）。
- **多视图**：数值模式 / 曲线模式切换。

### 5.3 前端数据流

```javascript
const ws = new WebSocket(`ws://${location.host}/ws`);
ws.onmessage = (ev) => {
  const d = JSON.parse(ev.data);   // 解 §6.2 的实时帧
  updateCards(d);
  updateCharts(d);
};
// 断线自动重连
```

---

## 6. 接口协议

### 6.1 HTTP 路由

| 方法 | 路径 | 用途 |
| ---- | ---- | ---- |
| GET | `/` | 实时监控主页 |
| GET | `/assets/*` | 静态资源（JS/CSS） |
| GET | `/api/status` | 设备状态（连接、模式、固件版本） |
| GET | `/api/records` | 记录文件列表（JSON） |
| GET | `/api/records/{name}` | 下载某记录（CSV/FIT） |
| POST | `/api/control` | 开始/停止记录、切功率源（JSON body） |
| GET | `/ws` | **WebSocket** 实时数据推送 |

### 6.2 WebSocket 实时帧（示例）

1Hz 推送，JSON 紧凑格式：

```json
{
  "t": 123456,          // t_ms
  "p": 245.0,           // power W
  "cad": 92,            // rpm
  "vg": 9.4,            // m/s
  "vair": 8.9,          // m/s
  "yaw": 3.2,           // deg
  "posture": "AERO",    // 姿态分区
  "cda": 0.281,         // m^2（在线估算）
  "rho": 1.225,         // kg/m^3
  "src": "BLE",         // 功率源
  "log": true           // 是否在记录
}
```

> **注意**：实时帧里的 `cda` 是**在线粗略估算**，存在模型问题（见项目分析 P0-1）。
> 前端应标注"趋势参考"，精确值以后处理为准。

### 6.3 与既有格式统一

- 下载的 CSV/FIT 与 SD 卡上的文件**完全一致**，复用 [`plan/README.md`](./README.md) 与
  [`plan/app-merge.md`](./app-merge.md) 中的 meta sidecar、文件命名规范。
- `/api/records` 返回的字段与 app-merge 的"记录列表"协议保持一致，便于将来复用前端逻辑。

---

## 7. 与现有模块的集成

### 7.1 复用点

| 现有模块 | 复用方式 |
| -------- | -------- |
| `lcd.update(...)` 入参 | 作为实时帧数据源，避免重复计算 |
| `CsvLogger` | 提供记录列表、下载（新增读取接口） |
| `logger.ok()` / `powerMeter.connected()` 等 | 直接映射到 `/api/status` |
| `BlePowerMeter` / `AntPower` | 提供功率源切换（`/api/control` 映射到现有 `powerMode`） |
| 屏幕 UI | 显示 SSID/密码/IP、无线模式 |

### 7.2 新增设置项（LCD 面板）

在现有 SETTINGS 面板增加：

- **无线模式**：采集优先 / 实时优先 / 全开（§3.3）
- **Wi-Fi 开关**：开 / 关
- **热点信息显示**：SSID、密码、IP、二维码

### 7.3 内存与稳定性

- ESP32-S3（无 PSRAM 型号）内存紧张：NimBLE + ANT + LVGL + GFX + Web 服务器共存需**实测内存水位**。
- 若内存不足：
  - 前端资源 gzip + 分块发送，或存 SD 卡；
  - 限制 WebSocket 客户端数与历史缓冲深度；
  - 考虑升级带 **PSRAM 的 ESP32-S3 模组**（若硬件允许）。
- 加**看门狗友好**设计：网络操作不能长时间阻塞 `loop()`。

### 7.4 安全（轻量）

- SoftAP 设 **WPA2 密码**（默认随机，屏幕显示）。
- 控制类接口（`/api/control`）可要求简单 token（防止同网段误操作）。
- 跑道场景一般封闭可信，**不追求企业级安全**，但至少不要裸奔。

---

## 8. 落地 Checklist

### 固件

- [ ] 新增 `src/wifi_web.{h,cpp}`：SoftAP/STA + Web 服务器 + WebSocket
- [ ] 无线模式设置项（采集优先 / 实时优先 / 全开）+ 屏幕显示 SSID/IP
- [ ] 实时数据广播（复用 `lcd.update` 入参）
- [ ] HTTP 路由：`/`、`/api/status`、`/api/records`、`/api/control`、`/ws`
- [ ] 文件下载（复用 `CsvLogger` 读取接口）
- [ ] mDNS（`aeroprobe.local`）
- [ ] （可选）二维码热点配置

### 前端

- [ ] 单文件 HTML + uPlot 实时曲线
- [ ] 数值卡片 + 状态区 + 历史记录区 + 控制区
- [ ] 响应式 / 大字号
- [ ] 断线自动重连

### 验证

- [ ] 多客户端并发（≥4）稳定性
- [ ] 射频共存实测：Wi-Fi + BLE + ANT+ 同时运行的丢包率
- [ ] 内存水位与长时间运行稳定性（数小时）
- [ ] 覆盖范围实测（跑道场景，天线朝向）

---

## 9. 待决策项

1. **拓扑**：SoftAP 为主（推荐）？还是也要 STA 连现场路由？是否需要 AP+STA 双模？
2. **射频取舍**：绕圈测试时 ANT+ 是否可暂停，以换 Wi-Fi 稳定？"采集优先/实时优先"模式是否够用？
3. **HTTP 框架**：`ESPAsyncWebServer`（开发快）还是 IDF `esp_http_server`（依赖少、更稳）？
4. **前端形态**：单文件原生 JS（推荐）还是构建式框架？
5. **硬件内存**：当前 ESP32-S3 是否带 PSRAM？不带的话 Web 服务可能吃紧，需实测。
6. **历史文件**：Web 端只做下载，还是也要"曲线回放"？

---

## 附：三份文档的关系

| 文档 | 面向 | 传输 | 场景 |
| ---- | ---- | ---- | ---- |
| [`plan/README.md`](./README.md) | 存储子系统 | SD 卡 | CSV/FIT 记录与导出 |
| [`plan/app-merge.md`](./app-merge.md) | 手机 APP | BLE（近场） | 实时查看 + 码表 FIT 合并 |
| **`plan/wifi-web.md`（本文）** | PC 网页 | Wi-Fi（远场） | 教练/采集员远程实时查看分析 |

三者共享：文件命名规范、meta sidecar、功率源切换、实时指标定义。

补充规划：

- **电池电量/充电状态**：加入 `/ws` 实时帧与 `/api/status`，见 [`plan/battery-power.md`](./battery-power.md) §7.3。
- **双 FS3000 视风**：`vCross` / `yaw` 依赖 I2C 分路，见 [`plan/i2c-bus.md`](./i2c-bus.md)。

---

## 10. 实现状态（已落地）

> 本节记录当前固件中的实际实现，与上文方案对应。

### 10.1 新增/改动的文件

| 文件 | 说明 |
| ---- | ---- |
| `src/wifi_web.h` / `src/wifi_web.cpp` | Wi-Fi 子系统：SmartConfig 配网 + STA 连接 + Web 服务 + WebSocket |
| `src/web_assets.h` | 网页前端（PROGMEM 内嵌单文件 HTML/JS） |
| `src/config.h` | 新增 `WIFI_WEB_ENABLE`、`WIFI_MDNS_HOST`、`WIFI_RECONNECT_MS`、`WIFI_SMART_CONFIG_TIMEOUT_MS` |
| `src/main.cpp` | `setup()` 调用 `wifiWeb.begin(true)`；`loop()` 调用 `wifiWeb.tick()` 与 `broadcastTelemetry()`，并消费控制请求 |
| `platformio.ini` | 新增依赖 `AsyncTCP-esphome`、`ESPAsyncWebServer-esphome` |

### 10.2 已实现的接口

| 路径 | 方法 | 说明 |
| ---- | ---- | ---- |
| `/` | GET | 实时监控主页（数值卡片 + WebSocket 刷新 + 记录列表） |
| `/ws` | WebSocket | 1Hz 实时数据推送 |
| `/api/status` | GET | 连接状态、SSID、IP、配网中标志、客户端数 |
| `/api/records` | GET | SD 卡 `.csv`/`.fit` 文件列表（JSON） |
| `/api/download?name=...` | GET | 下载某记录文件 |
| `/api/control?cmd=start\|stop\|source&src=N` | POST | 开始/停止记录、切换功率源；仅接受已知命令和功率源 |
| `/api/wifi/provision` | POST | 断开当前网络并启动 SmartConfig 重新配网 |

### 10.3 关键实现要点

- **SmartConfig（ESP-Touch）**：`WiFi.beginSmartConfig()`；配网成功后取 `WiFi.SSID()/psk()`，
  经 `Preferences("aero-wifi")` 持久化；后续开机后在 LCD 设置页打开 Wi-Fi 时自动 STA 连接。
  **带超时**（`WIFI_SMART_CONFIG_TIMEOUT_MS`，默认 120s）；首次无凭据时超时会在 5 秒后重新启动配网。
- **访问方式**：HTTP 页面、API 和 WebSocket 均不要求用户名或密码，连接到设备所在局域网后可直接访问。
  当前仅提供 HTTP，没有 TLS，也没有用户认证；仅应在可信局域网使用，勿转发到公网。
- **凭据管理**：`startSmartConfig()` 重新配网、`clearCredentials()` 清除 Wi-Fi 凭据。
- **mDNS**：`http://aeroprobe.local/`（`WIFI_MDNS_HOST`）。
- **WebSocket**：`AsyncWebSocket`，1Hz `textAll` 广播紧凑 JSON。
- **按需启停**：设备启动时 Wi-Fi 默认关闭；在 LCD 的 Settings 页面打开 Wi-Fi 开关后才启动网络连接与 Web 服务。关闭开关会断开 Wi-Fi 并关闭射频。
- **非阻塞**：SmartConfig 收到凭据后异步推进 STA 连接；`tick()` 负责轮询与断线重连。
- **控制解耦**：HTTP 回调只更新受临界区保护的请求标志，由 `main.cpp` 主循环消费。
- **SD 并发**：CSV logger 与网页记录列表/下载共用 FreeRTOS mutex；下载按块读取，每块持锁期间及时释放。
- **接口校验**：功率源限制为 0–3；下载只允许根目录 `.csv`/`.fit` 文件名，控制失败使用明确 HTTP 状态码。

### 10.4 射频共存提醒（实测待办）

ESP32-S3 单射频，**Wi-Fi + BLE(功率计/姿态) + ANT+ 三者共享**：

- 开启 Wi-Fi 后 BLE/ANT+ 可能出现丢包；绕圈测试若只看实时数据，建议暂停 ANT+。
- 整体可通过 `WIFI_WEB_ENABLE=0` 关闭 Wi-Fi 子系统（省射频与内存）。
- 真机需实测：Wi-Fi + BLE + ANT+ 同时运行的丢包率（见 §8 验证清单）。

### 10.5 编译结果

- `pio run -e esp32s3` **编译通过**（含网页认证、控制、SD 并发修复）。
- RAM 37.9%，Flash 48.0%（含 LVGL + NimBLE + ANT + AsyncWebServer）。
- 真实设备上的直接网页访问、WebSocket、下载并发和 SmartConfig 恢复仍需硬件联调。

### 10.6 前端说明

- 单文件原生 HTML/JS（`web_assets.h`），无构建、无外部依赖，内嵌 Flash。
- 数值卡片实时刷新；断线自动重连；每 10s 拉取一次记录列表。
- **待增强**：实时时序曲线（接入 uPlot，见 §5）；本项目页面暂以数值卡片 + 记录下载为主。

### 10.7 使用与配置步骤

#### A. 编译并烧录启用 Wi-Fi 的固件

1. 确认 `src/config.h` 中 `WIFI_WEB_ENABLE` 为 `1`。当前项目配置默认启用。
2. 在项目 `code/` 目录编译 ESP32-S3 固件：

   ```sh
   pio run -e esp32s3
   ```

3. 用 USB 将固件烧录到设备：

   ```sh
   pio run -e esp32s3 -t upload
   ```

4. 打开串口监视器，波特率设为 `115200`。设备启动后 Wi-Fi 默认关闭；需要在设备屏幕中进入 **SETTINGS** 并打开 **Wi-Fi** 开关，设备才会尝试连接已保存的 Wi-Fi 或启动 SmartConfig。

#### B. 首次连接 Wi-Fi（SmartConfig）

1. 确认要连接的路由器提供 **2.4 GHz Wi-Fi**，手机也连接到目标 2.4 GHz 网络。ESP32-S3 当前使用 2.4 GHz。
2. 在手机上打开兼容 Espressif ESP-Touch 的 SmartConfig 应用，选择目标 Wi-Fi 并输入该网络密码，然后启动配置。不要输入路由器管理密码。
3. 配网期间保持手机和设备靠近路由器，并等待串口出现 `SmartConfig received credentials`。成功后设备会异步加入该网络；串口出现 `reconnected. IP=...` 和/或 `mDNS ready` 表示已连接。LCD 设置页也会显示连接状态及访问地址。
4. SmartConfig 每轮最长等待 120 秒。首次没有已保存网络时，超时后设备会在 5 秒后再次进入配网，可重新从手机发送配置。

> SmartConfig 会将手机当前选择的 Wi-Fi 凭据发送给设备，不会让设备创建独立热点。若手机应用找不到网络或配网失败，确认手机当前连接的是 2.4 GHz 网络，重试时重新选择 SSID 并检查 Wi-Fi 密码。

#### C. 打开监控网页

1. 先确认 LCD 设置页中的 Wi-Fi 开关已打开且显示已连接，然后让运行浏览器的电脑/手机加入与设备相同的局域网。
2. 设置页会同时展示 mDNS 地址 `http://aeroprobe.local/` 和设备 IP 地址 `http://<设备IP>/`。优先使用 mDNS 地址；若 mDNS 在当前网络或操作系统上不可用，使用页面显示的 IP 地址，例如 `http://192.168.1.80/`。
3. 打开地址后网页直接加载，不需要输入用户名或密码。若浏览器仍显示此前缓存的登录提示，取消提示并重新打开地址或刷新页面。
4. 主页通过 WebSocket 显示实时数据；风速传感器读数失效时，风速和偏航角显示为 `--`。

#### D. 开始记录、下载数据和重新配网

1. 在主页的 **Control** 区域点击 **Start recording** 开始写入 SD 卡 CSV；点击 **Stop recording** 结束并关闭当前文件。确认设备已插入可用 SD 卡。
2. 在 **History** 区域查看记录文件并点击 **Download** 下载 CSV/FIT 文件。下载期间 SD 卡受互斥锁保护；大量下载仍可能增加 Wi-Fi/BLE/ANT+ 共存负载。
3. 点击 **Reprovision Wi-Fi** 并确认后，设备会断开当前网络并进入 SmartConfig；按照上面的首次配网步骤发送新的网络凭据。
4. 若设备当前仍连接旧网络但需要从网页发起重新配网，操作后浏览器会与设备断开；新网络配置完成后，使用新 IP 或 `http://aeroprobe.local/` 再次访问。
5. 不使用网页时，可在 LCD **SETTINGS** 页面关闭 Wi-Fi 开关，断开网络并停止 Wi-Fi 射频；再次使用时重新打开开关。

> 当前网页提供记录控制、记录下载和重新配网操作；功率源切换接口已实现，但网页尚未提供对应按钮。Web 服务使用未加密 HTTP 且不要求登录，局域网内其他设备均可访问和控制；只应在可信局域网使用，不要暴露到公网。
