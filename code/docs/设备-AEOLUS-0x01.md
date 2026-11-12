# 设备 AEOLUS-0x01（第一台 Prototype）

> 本文件记录本项目**第一台原型设备**的命名、身份与配置快照。
> 设备命名：**AEOLUS-0x01**

---

## 一、命名由来

| 项目 | 内容 |
|------|------|
| 设备代号 | **AEOLUS-0x01** |
| 名字来源 | 希腊神话 **Aeolus（埃俄罗斯）**，荷马史诗中的**风之王**，统领八方来风 |
| 后缀含义 | `0x01` 十六进制"第一号"，标识这是本项目的**第一台原型** |
| 与项目关联 | 本设备核心是 FS3000 风速/气动测量，风神管风，专业对口 |

### "威武又带小缺陷"的设定

风神 Aeolus 虽贵为众风之王，但他的**风袋是用皮囊银线扎口的**——扎不紧就会漏风。
这个神话里的小缺陷，正好隐喻本项目气动测量的真正软肋：

> **王者之风，败在接头** —— 只要气动管路/测风探头有一个接头没拧紧，`vAir` 读数就会飘。

这正是 AEOLUS-0x01 的人设：一个威严的风之王，却总在跟"漏风"这件小事较劲。

---

## 二、身份标识

| 字段 | 值 | 备注 |
|------|-----|------|
| 设备代号 | `AEOLUS-0x01` | 内部唯一标识 |
| 设备名（BLE 广播 / mDNS） | `AeroProbe` | 见 `src/config.h`：`BLE_DEVICE_NAME`、`WIFI_MDNS_HOST="aeroprobe"` |
| 主控 | Waveshare ESP32-S3-Touch-LCD-2.8 | V1/V2 均可 |
| SoC | ESP32-S3 | MAC: `d8:3b:da:9b:6f:68` |

> 说明：固件层面的广播名当前仍为通用的 `AeroProbe`。若需让广播名直接体现
> `AEOLUS-0x01`，需在 `src/config.h` 的 `BLE_DEVICE_NAME` 中修改（见第四节）。

---

## 三、当前配置快照

| 配置项 | 值 |
|--------|-----|
| 框架 | Arduino-ESP32 2.0.14（ESP-IDF 4.4.6），PlatformIO espressif32@6.5.0 |
| USB | 内置 USB-Serial-JTAG（烧录）+ TinyUSB CDC（运行） |
| 上传方式 | `pio run -t upload`（`before_reset=default_reset`） |
| 传感器 | FS3000-1015 风速 ×1（I2C 0x28）；BME280 可选（0x76） |
| 显示 | ST7789 240×320 + CST328 触摸 |
| 存储 | microSD（SPI 1 MHz） |
| 分区 | 3MB APP 单分区（无 OTA） |
| Flash | 16MB，QIO OPI + PSRAM OPI |

启动日志基线（正常空载）：
```
P=    0W(--) cad=   0 vg=0.00 vAir=0.00 ... fs=ACK err=OK ... sd=OFF gps=NOFIX
```

---

## 四、如需将设备名落地为 AEOLUS-0x01

当前 `src/config.h` 中存在**两处重复的** `BLE_DEVICE_NAME "AeroProbe"` 定义（第 96 行附近与第 128 行附近），
修改前应先消除重复，避免宏重定义告警。建议：

1. 删除后面重复的一组 `BLE_DEVICE_NAME` / `BLE_WHEEL_CIRC_M` / `BLE_RECONNECT_MS` / `BLE_POSTURE_RECONNECT_MS`。
2. 将保留的 `BLE_DEVICE_NAME` 改为 `"AEOLUS-0x01"`。
3. 如需 mDNS 同步，可将 `WIFI_MDNS_HOST` 改为 `"aeolus-0x01"`（→ `http://aeolus-0x01.local/`）。

---

## 五、后续设备命名规划（参考）

| 序号 | 代号 | 神话来源 | 备注 |
|------|------|----------|------|
| 第一台 | **AEOLUS-0x01** | 希腊风神 | 本项目首台原型（本文件） |
| 第二台 | （待定） | 如 BOREAS（北风神）/ ZEPHYR（西风神） | 后续原型 |

---

_最后更新：随第一台原型 AEOLUS-0x01 定型而建立。_
