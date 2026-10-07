# ESP32-FS3000 骑行气动测试系统

基于 **Waveshare ESP32-S3-Touch-LCD-2.8** 的骑行空气动力学现场测试装置。原型验证阶段使用 **单片 FS3000-1015** 测风速，降低传感器成本；通过板载触摸屏选择 **BLE 或 ANT+ 功率计**，实时查看风速、温度、功率与踏频。后续二期再考虑双 FS3000 V 形视风探针，以测量视风速度与偏航角并提升 CdA 分析能力。可选 **BME280** 提供环境温度/气压，**WT9011DCL-BT50** 提供骑行姿态记录。

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
    ├── lib/ant/              # ★ ANT+ 协议栈（esp32-ant，Apache-2.0，本地库免网络拉取）
    │   ├── include/          #   公开头（ant_node.h 为应用 API）
    │   ├── src/              #   协议引擎 + ANT+ profile 解码
    │   └── radio/            #   ESP32-S3 自带 BLE 射频的 ANT 物理层
    ├── src/                  # 原型固件（单 FS3000 + 彩色触摸 LCD）
    │   ├── main.cpp          # 主程序：传感器采样、功率源选择、日志与显示调度
    │   ├── config.h          # 引脚与参数配置（改这里）
    │   ├── fs3000.{h,cpp}    # FS3000 驱动：校验和 + 查表插值
    │   ├── wind_probe.{h,cpp}# 预留二期：双探针向量与偏航角解算
    │   ├── ble_power.{h,cpp} # BLE 功率计客户端（CPS 0x1818/0x2A63）
    │   ├── ant_power.{h,cpp} # ★ ANT+ 功率计接收（esp32-ant coexist 模式）
    │   ├── ble_posture.{h,cpp}# BLE 姿态传感器客户端（WT9011DCL-BT50，0x55 帧）
    │   ├── bme280_env.{h,cpp}# 可选 BME280：空气密度 ρ
    │   ├── csv_logger.{h,cpp}# TF 卡 CSV 日志（1s 一行）
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
| 功率计 | 支持 **BLE 或 ANT+** 的功率计（Assioma、4iiii、Stages、Garmin Vector 等支持 ANT+；纯 BLE 如迈金等走 BLE 通道） |
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

## ANT+ 功率计说明

**原理**：`code/lib/ant/` 是 esp32-ant（RaemondBW，Apache-2.0）——纯软件 ANT/ANT+ 协议栈，把 ESP32-S3 自带的 BLE 射频当通用 GFSK 调制解调器跑 ANT 协议，**不需要外置 ANT 芯片/接收器**。库随工程内置，`pio run` 无需联网拉取。

**工作模式（coexist，与 BLE 共用射频）**：

- 主机 NimBLE 保持运行（已建立的 BLE 连接不断），ANT+ 借用 NimBLE 被动扫描的射频窗口收包；
- 代价：ANT+ 运行期间 **BLE 的扫描暂停**（已建立的 BLE 连接不受影响）；需要手动扫描/更换 BLE 功率计时，从屏幕选择 BLE 扫描，固件会先停止 ANT+；
- ANT+ 通道**只收不发**（协议如此，无需任何配置）。

**配对与数据**：

- AUTO/ANT+ 模式首次搜索会配对并记忆（NVS 持久化，重启自动重连同一功率计）；距离门限 `ANT_POWER_PROXIMITY_RSSI=-70`；
- 输出 **瞬时功率 W + 瞬时踏频 rpm**（标准 power-only page 0x10），串口显示 ANT 设备号及接收数据；
- 数据源切换：ANT+ 跟踪时优先用 ANT+，否则退回 BLE 功率计；CSV `powerSrc` 列记录实际来源，后处理自动兼容。

## 单 FS3000 简化版

如果只需要**验证模块/简单风速监测**，可以用独立单传感器版本，不接功率计、不测偏航角：

- 位置：`code/single-fs3000/single_fs3000.ino`（**单独一个文件**，驱动/查表/显示全内置）
- 接线：FS3000 一片，SDA→GPIO11、SCL→GPIO10、3V3/GND
- 功能：屏幕大字显示风速 m/s + 原始计数 + 校验错误计数，串口同步输出
- 编译：PlatformIO 进入 `code/single-fs3000` 目录 `pio run -t upload`；或 Arduino IDE 直接打开该 .ino（需装 ESP32 板包 + `GFX Library for Arduino`）

与主工程的差异：主工程增加彩色触摸 UI、BLE/ANT+ 功率计及日志；单文件版只用于快速验证单片传感器，代码更短、更易读。

## 浏览器 UI 模拟器

无硬件时可直接打开 [code/simulator/index.html](code/simulator/index.html) 预览 240×320 仪表 UI，并调整模拟风速（界面单位 km/h，CdA 内部换算为 m/s）、环境温度、功率、踏频及 BLE/ANT+ 数据源。提供采样推进，用来检查读数、3 秒平均功率和 ZONE 显示；该模拟器验证的是 UI 与估算逻辑，不模拟 ESP32、FS3000 电气行为或真实 BLE/ANT+ 射频连接。

模拟 CdA 假设静风、平路、匀速，以单传感器视风速度近似地速，并扣除固定滚阻和传动损耗。单 FS3000 无法单独测得地速、风向或坡度，ZONE（1=直立，3=低趴，4–6=更激进，7=顶级 TT）为便于演示的启发式参考区间，不代表真实骑行测量结果。

固件仪表也按相同假设显示粗略 CdA：使用 3 秒平均功率、当前空气密度、视风速度近似地速；质量、滚阻系数和传动效率默认为 85 kg、0.004 和 97.5%，可在 `code/src/config.h` 调整。无功率数据、风速低于 3 m/s 或有效气动功率不足时显示 `--`。单传感器不能测地速、风向、坡度或加速度，因此该数值仅适合静风、平路、匀速条件下的趋势对比，不应视为准确的绝对 CdA。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/hardware-installation.md](docs/hardware-installation.md) | 硬件安装图文指南：单传感器接线、触摸屏、整车安装、通电自检清单；另含二期 V 形探针说明 |
| [docs/enclosure-analysis.md](docs/enclosure-analysis.md) | 外壳（结构件）方案分析：探针头支架 + 主机盒各 3 个 ABC 方案、材料建议 |

## 使用流程

1. 上电后 LCD 显示仪表页面。触摸底部功率源按钮，选择 **SCAN BLE DEVICES** 后等待扫描完成；点选功率计，在确认页点击 **CONNECT** 才会连接。列表优先显示广播了 Cycling Power 服务的设备，也会列出其它可连接 BLE 设备；选择非功率计设备时，固件会报告缺少的 GATT 服务/特征。**ANT+ POWER METER** 会搜索并自动记忆 Bike Power 设备；**AUTO SELECT** 在启动延迟后启动 ANT+ 自动搜索。BLE 手动扫描可随时重新打开，但会先停止 ANT+ 射频接收。自动模式的前 20 秒会尝试连接姿态传感器，不会自动连接 BLE 功率计。
2. PlatformIO 串口监视器使用 115200 波特率（`pio device monitor`）。设备扫描会输出名称、地址和 RSSI；成功连接后，`[BLE-DATA]` 显示 BLE 功率/踏频/车速，`[BLE-POSTURE]` 显示姿态角，`[ANT-DATA]` 显示 ANT+ 设备号、功率和踏频；每秒还会输出汇总行。
3. 仪表实时显示风速（km/h）、环境温度（°C）、3 秒平均功率、踏频及功率源状态。当前单风速计原型不测偏航角。
4. TF 卡生成 `fs_<时间>.csv`，每秒一行：
   `t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc`
   （`powerSrc`：0=无 1=BLE 2=ANT+；未接入姿态传感器时 postureDeg 缺省，后处理自动兼容）
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
| `ANT_POWER_ENABLE` | 1 | 1=启用 ANT+ 功率计接收；0=仅 BLE 功率计 |
| `ANT_POWER_START_DELAY_MS` | 20000 | 启动后延迟开 ANT 的毫秒数：先给 BLE 姿态/功率计连接窗口 |
| `ANT_POWER_PROXIMITY_RSSI` | -70 | ANT+ 首次配对距离门限 dBm（0=不限制，-70≈车上的设备） |
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

- **扫描不到 BLE 功率计**：确认功率计支持 BLE 并已唤醒（转几圈曲柄）；从功率源菜单点 **SCAN BLE DEVICES** 后等待 6 秒。若 ANT+ 已启动，固件会先停止 ANT+ 再扫描；检查串口 `[BLE] scan complete` 和发现列表中的 RSSI。选中设备后需在确认页再次点 **CONNECT**。
- **ANT+ 功率计连不上**：确认功率计有 ANT+ 广播（多数功率计默认开着）；首次配对在车旁进行（`ANT_POWER_PROXIMITY_RSSI=-70` 只认近距离设备）；配对记忆存在 NVS，重启自动重连；若换了功率计，用 `ant_node_pair()` 或 `ant_node_forget_device()` 重新配对（代码在 `ant_power.cpp`，可加按钮触发）。
- **ANT+ 与 BLE 同时用注意**：esp32-ant 为 coexist 模式——BLE 已建立的连接（姿态传感器）保持不断，但 BLE"扫描"被 ANT 占用（暂停），所以启动窗口内先连好 BLE 设备；ANT+ 通道**只收不发**，属协议正常。
- **扫描不到姿态传感器**：确认 WT9011DCL 已开机（指示灯亮）；先用维特智能手机 app 连一次唤醒并确认设备名以 WT 开头。ANT+ 射频运行时 BLE 扫描暂停；切回 BLE 或在 ANT+ 启动前让姿态设备完成连接。
- **姿态角度不变化/为 0**：检查是否连上（屏幕 Pos 行）；确认传感器绑紧无松动；Yaw 漂移属正常（磁力计受金属干扰），只看 Pitch。
- **LCD 不亮**：核对 `config.h` 里 LCD 引脚与你板子版本（V1/V2 相同）；确认背光 GPIO5 上电。
- **二期双传感器读数异常**：确认两片在不同总线；检查 V 形安装角度与箭头方向。当前原型只接一片 FS3000，不显示偏航角。
- **排针插不进杜邦头**：本板排针间距实测 **1.27mm**（非 2.54mm），用「1.27mm 转 2.54mm」转接杜邦线，或直接焊线。
- **TF 卡初始化失败**：格式化为 FAT32；部分卡需上电前插入。

## 参考

- Renesas FS3000 数据手册（地址 0x28、校验和、1015 典型曲线）
- Waveshare ESP32-S3-Touch-LCD-2.8 官方文档（引脚定义）
- 蓝牙 SIG Cycling Power Service（0x1818 / 0x2A63）规范
- WitMotion WT9011DCL-BT50 数据手册与 BLE 协议（服务 0xFFE0 / 数据 0xFFE1，0x55 帧）
- esp32-ant（RaemondBW，Apache-2.0）：纯软件 ANT/ANT+ 栈，驱动 ESP32-S3 自带 BLE 射频（库源码已随工程放在 `code/lib/ant/`，官方仓库 https://github.com/RaemondBW/esp32-ant ）

> 免责：本系统用于业余/科研现场对比测试，绝对 CdA 精度受自然风湍流与车手扰流影响，建议以"同条件下姿势间相对差异"为准。
