# 屏蔽 PWR Key（Reset 按钮）硬件故障修复指南

## 问题描述
ESP32-S3 测试主板的 Reset 按钮（PWR_KEY，GPIO6）硬件故障，经常误触导致系统意外关机。

## 解决方案
通过代码层面禁用 PWR Key 功能，使硬件按钮不再影响系统运行。

## 实现方法

### 1. 配置开关
在 [`src/config.h`](./code/src/config.h) 中修改以下配置：

```c
// ---- PWR_KEY 功能开关 ----
// 设为 0 屏蔽 Reset 按钮：GPIO6 不响应任何操作，系统无法通过 PWR_KEY 关机。
// 用于修复硬件故障（按钮卡住、误触等）。
#define POWER_KEY_ENABLED 1   // 改为 0 来禁用
```

**修改方式：**
- 启用 Power Key：`#define POWER_KEY_ENABLED 1`（默认，正常使用）
- 禁用 Power Key：`#define POWER_KEY_ENABLED 0`（硬件故障时使用）

### 2. 代码实现

修改了两个文件：

#### `src/power_key.cpp` - PowerKey::begin()
```cpp
void PowerKey::begin() {
#if POWER_KEY_ENABLED
  // 正常的 GPIO 初始化和电源保持逻辑...
#else
  AppLog.println("[PWR] POWER_KEY_ENABLED=0 -> power key disabled");
#endif
}
```

#### `src/power_key.cpp` - PowerKey::tick()
```cpp
void PowerKey::tick() {
#if !POWER_KEY_ENABLED
  return;  // Power key disabled
#endif
  // 正常的按键扫描逻辑...
}
```

## 功能特性

### 禁用后的行为
- ✅ GPIO6（PWR_KEY）不再被监听或响应
- ✅ GPIO7（PWR_CONTROL）不会因按键误触而被拉低
- ✅ 系统不会意外关机
- ✅ 启动日志会输出：`[PWR] POWER_KEY_ENABLED=0 -> power key disabled`

### 其他关机方式
禁用 PWR Key 后，仍然可以通过以下方式关闭系统：
- 💻 通过 Web 界面菜单关机
- 📱 通过 BLE 连接的设备远程关机（如果应用支持）
- 🔌 物理断开电源/USB

### 重新启用
如需恢复 PWR Key 功能：
1. 修改 `src/config.h`：`#define POWER_KEY_ENABLED 1`
2. 重新编译上传固件

## 编译上传

### 使用 PlatformIO
```bash
cd code/
pio run --target=upload  # 使用默认环境
# 或指定特定环境
pio run -e esp32s3 --target=upload
```

### 烧录固件
根据你的环境选择适当的烧录方式：
- 正常 bootloader：`pio run --target=upload`
- 无 BOOT/RESET 按钮修复（TinyUSB）：使用 [`esp32-buttonless-flash` 技能](./skill.md)

## 日志输出示例

### 禁用时的启动日志
```
[PWR] POWER_KEY_ENABLED=0 -> power key disabled
```

### 启用时的启动日志（对比）
```
[PWR] boot without key press -> power NOT latched
```

## 安全考虑

1. **意外关机保护**：此修改完全消除了因 PWR_KEY 硬件故障导致的误触关机
2. **误操作风险**：如果误切换开关，可能导致期望的关机功能失效
3. **恢复方式**：修改配置后重新烧录即可恢复，无不可逆操作

## 故障排查

### 修改后仍然误触关机
1. ✔️ 确认已重新烧录固件（不是旧版本）
2. ✔️ 检查启动日志中是否出现 `POWER_KEY_ENABLED=0` 提示
3. ✔️ 检查是否是其他硬件问题（如 GPIO7 接触不良）

### 需要临时启用 PWR Key
1. 修改配置为 `#define POWER_KEY_ENABLED 1`
2. 重新编译上传
3. 测试完成后改回 `#define POWER_KEY_ENABLED 0`

## 相关文件
- 配置：[`src/config.h`](./code/src/config.h) 第 53 行
- 实现：[`src/power_key.h`](./code/src/power_key.h)
- 实现：[`src/power_key.cpp`](./code/src/power_key.cpp)
- 使用：[`src/main.cpp`](./code/src/main.cpp) 第 203, 283-301 行
