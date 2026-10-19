# ESP32-FS3000 骑行气动测试系统

基于 **Waveshare ESP32-S3-Touch-LCD-2.8** 的骑行空气动力学现场测试装置。原型验证阶段使用 **单片 FS3000-1015** 测风速，降低传感器成本；通过板载触摸屏选择 **BLE 功率计或虚拟功率计**，实时查看风速、功率与踏频。本版本暂不支持 ANT+。后续二期再考虑双 FS3000 V 形视风探针，以测量视风速度与偏航角并提升 CdA 分析能力。可选 **BME280** 提供空气密度计算所需的温度/气压，**WT9011DCL-BT50** 提供骑行姿态记录。

```
CdA = (P − P_滚阻 − P_坡度 − P_加速) / (0.5 · ρ · v_a² · v_g · cos φ)
```

## 目录结构

```
esp32-fs3000-project/
├── README.md                 # 本文档
├── img/                      # 实物照片（IMG_0953~0956）+ 接线示意图 wiring-diagram.svg
├── docs/                     # 文档
│   ├── hardware-installation.md  # 硬件安装图文指南（接线/探针/装车/自检，含实物照片核对）
│   └── enclosure-analysis.md     # 外壳（结构件）方案分析：探针头 + 主机盒 ABC 方案
└── code/
    ├── platformio.ini        # PlatformIO 工程配置
    ├── lib/ant/              # 历史 ANT+ 库（Apache-2.0），保留源码但不参与编译/链接
    │   ├── include/          #   公开头（ant_node.h 为应用 API）
    │   ├── src/              #   协议引擎 + ANT+ profile 解码
    │   └── radio/            #   ESP32-S3 自带 BLE 射频的 ANT 物理层
    ├── src/                  # 原型固件（单 FS3000 + 彩色触摸 LCD）
    │   ├── main.cpp          # 主程序：传感器采样、功率源选择、日志与显示调度
    │   ├── config.h          # 引脚与参数配置（改这里）
    │   ├── fs3000.{h,cpp}    # FS3000 驱动：校验和 + 查表插值
    │   ├── wind_probe.{h,cpp}# 预留二期：双探针向量与偏航角解算
    │   ├── ble_power.{h,cpp} # BLE 功率计/骑行台（CPS 与 FTMS Indoor Bike Data）
    │   ├── ble_posture.{h,cpp}# BLE 姿态传感器客户端（WT9011DCL-BT50，0x55 帧）
    │   ├── bme280_env.{h,cpp}# 可选 BME280：空气密度 ρ
    │   ├── csv_logger.{h,cpp}# TF 卡 CSV 日志（1s 一行）
    │   ├── gps_time.{h,cpp} # UART2 后台接收/解析与主循环快照
    │   ├── sd_download.h    # 单路限速 SD 下载响应
    │   └── display_lcd.{h,cpp}# ST7789 屏状态显示
    ├── single-fs3000/        # ★ 单 FS3000 简化版（独立小工程，见下节）
    │   ├── single_fs3000.ino # 单文件固件：风速大字显示 + 原始计数/校验
    │   └── platformio.ini
    └── tools/
        └── cda_postprocess.py# PC 端后处理：CSV → CdA + 偏航角分箱
```

## 硬件

| 器件 | 说明 |
|---|---|
| 主板 | Waveshare ESP32-S3-Touch-LCD-2.8（240×320 ST7789 + 触摸） |
| 风速传感器（原型 ×1，二期可 ×2） | Renesas FS3000-1015（I2C 地址固定 0x28，0~15 m/s） |
| 功率计/骑行台 | 支持 **BLE** Cycling Power (CPS) 和 FTMS 智能骑行台 Indoor Bike Data。CSC (0x1816) 仅可提供踏频/速度，不能提供功率；本版本不支持 ANT+ |
| 姿态传感器（可选） | WitMotion **WT9011DCL-BT50**（BLE 5.0，9 轴，内置卡尔曼 0.2°，自供电 ~30h，9g，绑胸口/上背） |
| 环境传感器（可选） | BME280（计算空气密度 ρ） |
| TF 卡 | 板载卡槽，FAT32 格式，存 CSV 日志 |

## 接线

原型只接一片 FS3000 到 GPIO11/10。板载 V1 触摸控制器 CST328 使用 GPIO1/3 I2C 总线；GPIO15/18 保留给未来第二片 FS3000。两片 FS3000 地址同为 0x28，二期扩展时必须使用独立 I2C 总线。

| 器件 | SDA | SCL | VCC | GND |
|---|---|---|---|---|
| FS3000（原型） | **GPIO11**（板载排针 SDA） | **GPIO10**（板载排针 SCL） | 3V3 | GND |
| BME280（可选） | 与 #1 同总线（GPIO11） | 与 #1 同总线（GPIO10） | 3V3 | GND |
| 板载 V1 触摸屏 | GPIO1 | GPIO3 | 板载 | 板载 |

> 已按实物照片（`img/`、`photo/`，2026-10-02~03）核对：主板 12 针排针丝印完整为 IO15/IO18/RXD/TXD/SDA/SCL/**3V3**/GND/VBus/D+/D-/GND。FS3000 模块为 4 焊盘（SDA/SCL/VCC/GND），需焊接 2.54mm 公排针。**VBus 是 5V，严禁给 FS3000 供电（VDD≤3.6V）**。注意：**本板排针间距实测为 1.27mm（非 2.54mm 杜邦）**，杜邦母头插不进属正常，请使用「1.27mm 转 2.54mm」转接杜邦线（详见 [docs/hardware-installation.md](docs/hardware-installation.md)）。实物接线请以本节单传感器引脚表为准；`img/wiring-diagram.svg` 为双传感器二期示意图。

**二期 V 形探针机械安装**：两片传感器同点安装在车把前伸探杆末端，传感器轴各与车头成 ±45°（方向箭头朝前），两臂张开 5~8 cm 避免热膜互扰；探杆前伸 20~30 cm，确保传感器处于自由来流区（避开车手/头盔尾流）。

**姿态传感器（无线，免接线）**：WT9011DCL-BT50 充满电后绑在胸口（心率带位置）或上背部，开机自动广播，主机扫描设备名前缀 `WT` 自动连接。本系统只用它的 **Pitch（俯仰角）** 作为躯干姿态；其 Yaw 用磁力计、骑行中会被车架/路边金属干扰，不用于本系统。

## 编译与烧录

### PlatformIO（推荐）

1. VS Code 安装 PlatformIO 扩展，打开本仓库根目录（含 `code/platformio.ini`）。
2. 首次编译会自动下载 ESP32 工具链（较慢）。
3. 用 Type-C 连接主板：
   ```bash
   cd code
   pio run               # 编译
   pio run -t upload     # 烧录
   pio device monitor    # 串口监视（115200）
   ```

### Arduino IDE

1. 安装 ESP32 板包（espressif esp32，选择 ESP32S3 Dev Module，Flash Size 16MB）。
2. 安装库：`NimBLE-Arduino`、`GFX Library for Arduino`、`Adafruit BME280 Library`。
3. 把 `src/` 下所有 `.h/.cpp` 与 `main.cpp` 放进一个 sketch 文件夹（或将 `main.cpp` 改名 `main.ino` 并保留其余文件为 tab），编译上传。

## 无线与存储并发

- GPS 使用 UART2（RX=44、TX=43），独立任务接收并解析 NMEA，接收缓存为 4096 字节。主循环仅获取受锁保护的快照，SD 初始化、写盘或界面操作不再直接阻塞 GPS 解析。串口溢出、校验失败会以 `[GPS]` 计数日志报告。
- SD 使用 SPI2（CLK=14、MISO=16、MOSI=17、CS=21），LCD 使用 SPI3，两个主控不混用。CSV 后台批量写入、目录访问、网页下载和音频文件访问使用同一 SD 互斥锁，不持有锁等待网络发送。
- 网页下载同时仅允许一路，每次最多读 1024 字节，两次成功读之间至少间隔 20 ms（应用读取上限约 50 KiB/s，实际网络吞吐通常更低）。SD 短暂繁忙使用异步重试；连续取锁失败 5 秒，或文件打开/定位/读取失败时，打印原因并中断连接，而不是伪装成完整下载。
- Wi-Fi 与 BLE 使用 ESP-IDF 软件射频共存机制；当前固定工具链的 SDK 启用了 `CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE`。BLE 扫描窗口/间隔为 30/100 ms；手动扫描最多 6 秒，后台姿态发现每 30 秒尝试一次、扫描 1.5 秒，同一时间只执行一个 BLE 扫描/连接操作。
- 音频播放仍沿用原有的释放 BLE 栈策略，不承诺音频与 BLE 持续并行。播放期间 CSV 暂缓提交新批次，已提交批次仍会完成；内存缓存满时会丢样并在后续成功写入后报告。
- 本版本移除 ANT 功率计适配和界面入口；`lib/ant/` 仅保留历史源码，并通过 `lib_ignore` 排除。CSV 来源编号保持兼容：0=无，1=BLE，2=历史 ANT+ 保留值（不再产生），3=虚拟功率计；HTTP 功率源请求仅接受 0、1、3。

### 并发回归验证

先运行一次 `pio run` 准备项目现有的 TinyGPSPlus 依赖，然后使用本机 C++ 编译器执行主机回归测试，无需额外安装测试库：

```bash
cd code
sh test/host/run.sh
```

测试使用真实 GPS 解析代码及 TinyGPSPlus，模拟 UART/FreeRTOS/文件系统，覆盖主循环停止取快照时的后台接收、定位过期、重新启动、错误统计，以及 SD 下载的限块、节流、繁忙重试、超时和读取失败。它不能代替实机射频及供电验证。

实机建议连续记录至少 30 分钟，同时开启 BLE 通知、GPS 和 Wi-Fi 遥测，下载已关闭的记录文件；检查 `[GPS]` 溢出、BLE 断连、`writer backlog dropped` 及下载文件哈希。再测试断网重连、BLE 扫描、无 GPS 模块和 GPS 手动重启；不能仅凭编译成功认定零丢包。

## 单 FS3000 简化版

如果只需要**验证模块/简单风速监测**，可以用独立单传感器版本，不接功率计、不测偏航角：

- 位置：`code/single-fs3000/single_fs3000.ino`（**单独一个文件**，驱动/查表/显示全内置）
- 接线：FS3000 一片，SDA→GPIO11、SCL→GPIO10、3V3/GND
- 功能：屏幕大字显示风速 m/s + 原始计数 + 校验错误计数，串口同步输出
- 编译：PlatformIO 进入 `code/single-fs3000` 目录 `pio run -t upload`；或 Arduino IDE 直接打开该 .ino（需装 ESP32 板包 + `GFX Library for Arduino`）

与主工程的差异：主工程增加彩色触摸 UI、BLE 功率计及日志；单文件版只用于快速验证单片传感器，代码更短、更易读。

## 浏览器 UI 模拟器

无硬件时直接打开 [code/simulator/index.html](code/simulator/index.html)，无需安装依赖或启动服务器。模拟器按当前 BLE-only 固件同步：**SET / BOOT** 打开六宫格设置页，包含 POWER、LANG、WI-FI、SOUND、SD/MUSIC、GPS。当前固件因渲染问题暂时禁用中文选择，模拟器也保持英文并给出提示，而不展示尚未启用的切换效果。

各级 BACK 立即切换画面，但共用返回防连击保护：实机在返回后屏蔽旧触摸，至少经过 250 ms 且连续松手 120 ms 后才接受新触摸，防止第三层返回穿透到 SETTINGS 的返回按钮；无需阻塞或延迟渲染。模拟器对应屏蔽 250 ms 内的重复返回，真实触摸抖动仍需实机验证。

| 功能 | 模拟器行为 |
|---|---|
| 功率来源 | BLE 扫描 6 秒、设备列表、连接确认、失败/空列表/取消；CPS 与 FTMS 可提供功率，CSC-only 提示无功率支持；AUTO 使用已连接的 BLE，不自动配对功率计；虚拟功率/踏频与 BLE 输入独立，无 ANT 入口 |
| 仪表 | 240×320；显示瞬时功率、踏频、风速；CdA 使用最近 3 次 1 Hz 功率样本平均及固件相同公式/阈值；风速无效时显示占位符 |
| Wi-Fi | 与语言选择相同样式的 OFF/ON 按钮切换，高亮当前状态，不使用单选框或开关组件；关闭、SmartConfig 等待、STA 连接中/已连接状态及演示地址；右侧环境选择器注入配网结果，不会连接真实网络 |
| GPS | 未找到模块提示、搜星/定位/定位过期状态、UTC/UTC+8/经纬度/卫星/HDOP/海拔/地速及手动重启；右侧选择器注入模块状态，演示坐标和时间不是实时定位 |
| 声音与 SD | 音量使用与虚拟功率相同的滑块，范围 0–100%、步进 1%，拖动时显示数值、松手后保存；目录浏览、上级目录、音频播放/停止状态。播放时暂停 BLE，并暂缓提交 CSV 新批次；不开启浏览器声音，提示音与固件一样关闭 |
| 日志与下载 | 自动采样与录制独立；通过右侧“模拟网页命令”开始/停止录制，需模拟 Wi-Fi 已连接。每秒一行、60 条批量写入、缓冲满丢样计数；下载已落盘部分的九列 CSV，保留 0/1/3 来源编号；单路下载、1 KiB 分块/20 ms 节流、SD 繁忙重试与 5 秒超时 |

右侧控制区是测试工具，不是主板新增的屏幕按钮。**Advance 1/60 Second** 推进模拟时间；**Start Auto Sampling** 启动每秒采样，但不自动开始录制；**Reset Averages** 仅清空仪表历史，不清除日志或重置运行时间。停止录制会将剩余缓存写入演示文件，再点击 **Download CSV** 可保存至电脑。演示文件与记录只在当前页面内存中存在，刷新后清空；音量偏好保留。

浏览器回归入口：[code/simulator/index.html?test=1](code/simulator/index.html?test=1)。测试结果显示在页面底部，覆盖设置入口、BLE、CdA、GPS、录制、音频与下载；测试会临时替代文件下载动作，不实际保存测试 CSV。

FS3000 不测温，仪表不显示温度；模拟器使用默认空气密度 1.225 kg/m³。它验证 UI 与应用状态流转，不模拟 ESP32 调度、中断、真实射频共存、SD 电气时序或供电，也不能用于证明实机零丢包。详情逻辑见 [code/simulator/simulator.js](code/simulator/simulator.js)，浏览器回归见 [code/simulator/regression.js](code/simulator/regression.js)。

模拟 CdA 假设静风、平路、匀速，以单传感器视风速度近似地速，并扣除固定滚阻和传动损耗。单 FS3000 无法单独测得地速、风向或坡度，ZONE（1=直立，3=低趴，4–6=更激进，7=顶级 TT）为便于演示的启发式参考区间，不代表真实骑行测量结果。

固件仪表也按相同假设显示粗略 CdA：使用 3 秒平均功率、当前空气密度、视风速度近似地速；质量、滚阻系数和传动效率默认为 85 kg、0.004 和 97.5%，可在 `code/src/config.h` 调整。无功率数据、风速低于 3 m/s 或有效气动功率不足时显示 `--`。单传感器不能测地速、风向、坡度或加速度，因此该数值仅适合静风、平路、匀速条件下的趋势对比，不应视为准确的绝对 CdA。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/hardware-installation.md](docs/hardware-installation.md) | 硬件安装图文指南：单传感器接线、触摸屏、整车安装、通电自检清单；另含二期 V 形探针说明 |
| [docs/enclosure-analysis.md](docs/enclosure-analysis.md) | 外壳（结构件）方案分析：探针头支架 + 主机盒各 3 个 ABC 方案、材料建议 |

## 使用流程

1. 上电后 LCD 显示仪表页面。触摸底部 **SET** 按钮打开设置页；选择 **Select power meter** 后，可选择 **SCAN BLE DEVICES**、**VIRTUAL POWER METER** 或 **AUTO SELECT**。虚拟功率计页面的功率与踏频滑块会立即更新仪表和日志；页面左上角 **BACK** 返回上一级。BLE 扫描会立即显示 LOADING；扫描与连接期间界面仍可响应触摸，左上角 **BACK** 可取消当前操作。点选设备后，在确认页点击 **CONNECT** 才会连接。连接后会主动发现并输出全部 GATT 服务/特征 UUID（`[BLE-GATT]`），不只依赖广播包中的服务 UUID。CPS 解析 0x1818/0x2A63；FTMS 骑行台解析 Indoor Bike Data **0x1826/0x2AD2**（0x2ACC 才是 Fitness Machine Feature，0x2AD1 是 Rower Data）的即时功率、踏频和速度。CSC 0x1816/0x2A5B 可提供踏频/速度，但**不提供功率**。若服务发现为空、需要配对或设备采用厂商私有协议，失败信息和 `[BLE-GATT]` 日志用于确认根因；可返回设备列表重选。**AUTO SELECT** 使用已连接的 BLE 功率计，不自动配对功率计；后台每 30 秒尝试发现姿态传感器。后台发现占用 BLE 时，手动扫描请求会明确提示稍后重试，不并行操作同一扫描器。
2. PlatformIO 串口监视器使用 115200 波特率（`pio device monitor`）。设备扫描会输出名称、地址、RSSI 和广播 CPS/FTMS/CSC 类型；连接后 `[BLE-GATT]` 列出 GATT 服务和特征 UUID/通知属性；`[BLE-DATA]` 显示功率/踏频/车速，`[BLE-FTMS]` 额外输出 FTMS Flags 及数据包是否包含即时功率/踏频/速度字段，便于判断设备能力与通知数据；`[BLE-POSTURE]` 显示姿态角；每秒还会输出汇总行。
3. 仪表实时显示风速（km/h）、实时功率、踏频及功率源状态。FS3000 不含温度测量功能；可选 BME280 的环境温度仅用于空气密度计算。当前单风速计原型不测偏航角。
4. TF 卡生成 `fs_<时间>.csv`，每秒一行：
   `t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc`
   （`powerSrc`：0=无 1=BLE 2=历史 ANT+ 保留值 3=虚拟功率计；本版本不会产生来源 2）
5. **姿态标定（每次佩戴后做一次）**：上身直立坐在车上，记录此时 Pitch 为基准（平放时读数约 0°）；前倾为负、挺直为正（以安装方向为准，可在后处理里取反）。
6. 测试协议（重要）：
   - 选**低风时段**（环境风速 < 3 m/s）测同一路段；
   - 姿势 A/B 各骑 2~3 圈，保持同功率/同路段；
   - 有风时做**往返配对**抵消顺逆风；
   - 偏航角 > ±45° 的数据不可信，后处理会剔除。
6. 拔出 TF 卡，PC 端后处理：
   ```bash
   python3 code/tools/cda_postprocess.py fs_000001.csv --mass 75 --crr 0.004 --grade 0
   ```
   输出窗口级 CdA（`cda_windowed.csv`）、按偏航角分箱的中位数（`cda_yaw_bins.csv`），以及**按姿态分箱的对比**（`cda_posture_bins.csv`：`postureDeg < 阈值`=气动姿势 vs `≥ 阈值`=直立姿势，阈值用 `--posture-threshold` 调，默认 15°），即可直接看"姿势变化带来多少 CdA 收益"。

## 参数配置（config.h）

| 参数 | 默认 | 说明 |
|---|---|---|
| `PROBE_HALF_ANGLE_DEG` | 45.0 | 二期双探针 V 形半夹角参数，单传感器原型不使用 |
| `BLE_WHEEL_CIRC_M` | 2.105 | 轮周长(m)，700×25C 约 2.105，按你的外胎改 |
| `BLE_POSTURE_NAME_PREFIX` | "WT" | 姿态传感器设备名前缀（扫描过滤用） |
| `BLE_POSTURE_RECONNECT_MS` | 30000 | 后台姿态传感器发现的尝试间隔，避免持续扫描争用射频 |
| `LOG_WINDOW_MS` | 1000 | 日志窗口，建议保持 1000 |
| `PROBE_SAMPLE_MS` | 125 | 探针采样周期，**不要小于 125**（FS3000 响应时间） |
| `CDA_TOTAL_MASS_KG` | 85.0 | CdA 粗略估算用的骑手+车辆总质量(kg) |
| `CDA_ROLLING_RESISTANCE` | 0.004 | CdA 粗略估算用的滚阻系数 |
| `CDA_DRIVETRAIN_EFFICIENCY` | 0.975 | CdA 粗略估算用的传动效率 |
| `CDA_MIN_AIRSPEED_MPS` | 3.0 | 低于此风速时不显示粗略 CdA |

## 标定建议

- **角度响应标定**：装车前用固定风扇在 0/15/30/45° 各吹一次，检查两片读数是否接近余弦关系；差异明显时在 `wind_probe.cpp` 里加灵敏度修正。
- **零风标定**：静止无风时两片应读 ~0 m/s，记录 offset 扣除。
- **姿态基准标定**：每次佩戴后直立坐车记录 Pitch 基准；胸口呼吸波动 ±1~2°，1s 窗口平均已消除。
- **滚动阻力标定**：可先做一次 Coast-down（滑行减速）测试标定 Crr，再代入后处理。

## 常见问题

- **连接成功但报没有功率服务**：查看串口 `[BLE-GATT]` 的服务/特征列表。CPS 功率需 `0x1818/0x2A63`；智能骑行台 FTMS 功率需 Indoor Bike Data `0x1826/0x2AD2`（`0x2ACC` 是 Feature，`0x2AD1` 是 Rower Data）；CSC `0x1816/0x2A5B` 只有踏频/速度。若 GATT 服务列表为空，先断开手机/码表等占用设备、唤醒骑行台并重新扫描；部分设备需先通过厂商 App 配对/解锁，若列表只有厂商私有 UUID，则需按该设备协议增加解析，标准 CPS/FTMS 无法读取。
- **扫描不到 BLE 功率计/骑行台**：确认设备已唤醒并广播 CPS、FTMS 或 CSC；从功率源菜单点 **SCAN BLE DEVICES** 后等待扫描完成，检查串口 `[BLE] scan complete` 和设备 RSSI。选中设备后需在确认页再次点 **CONNECT**。
- **只有 ANT+ 的功率计**：本版本暂不支持；双模功率计请选择其 BLE 通道。
- **扫描不到姿态传感器**：确认 WT9011DCL 已开机（指示灯亮）；先用维特智能手机 app 连一次唤醒并确认设备名以 WT 开头。后台每 30 秒进行一次有限扫描，手动配对或播放音频期间暂停。
- **姿态角度不变化/为 0**：检查是否连上（屏幕 Pos 行）；确认传感器绑紧无松动；Yaw 漂移属正常（磁力计受金属干扰），只看 Pitch。
- **LCD 不亮**：核对 `config.h` 里 LCD 引脚与你板子版本（V1/V2 相同）；确认背光 GPIO5 上电。
- **二期双传感器读数异常**：确认两片在不同总线；检查 V 形安装角度与箭头方向。当前原型只接一片 FS3000，不显示偏航角。
- **排针插不进杜邦头**：本板排针间距实测 **1.27mm**（非 2.54mm），用「1.27mm 转 2.54mm」转接杜邦线，或直接焊线。
- **TF 卡初始化失败**：格式化为 FAT32；部分卡需上电前插入。

## 参考

- Renesas FS3000 数据手册（地址 0x28、校验和、1015 典型曲线）https://www.renesas.com/en/products/fs3000?tab=documentation
- Waveshare ESP32-S3-Touch-LCD-2.8 （[官方文档](https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8)）
- 蓝牙 SIG Cycling Power Service（0x1818 / 0x2A63）规范
- WitMotion WT9011DCL-BT50 数据手册与 BLE 协议（服务 0xFFE0 / 数据 0xFFE1，0x55 帧）
- esp32-ant（RaemondBW，Apache-2.0）：纯软件 ANT/ANT+ 栈，驱动 ESP32-S3 自带 BLE 射频（库源码已随工程放在 `code/lib/ant/`，官方仓库 https://github.com/RaemondBW/esp32-ant ）

> 免责：本系统用于业余/科研现场对比测试，绝对 CdA 精度受自然风湍流与车手扰流影响，建议以"同条件下姿势间相对差异"为准。
