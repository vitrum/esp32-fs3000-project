# IMU 日志查看器 - 设备位置可视化增强

## 概述

`imu_log_viewer.html` 现已集成**设备位置与朝向可视化**功能，提供对 ESP32 主板和可穿戴 IMU 在自行车上的相对位置和朝向方向的直观展示。

## 新增功能

### 1. 设备位置俯视图
**Canvas: `devicePosition`**

- 展示 ESP32 主板和可穿戴 IMU 在自行车上的相对位置
- 俯视视角，北方指向上方
- 颜色编码：
  - 🔴 红色（#FF6B6B）：ESP32 主板 - 位于骑手胸部
  - 🔵 青色（#4ECDC4）：可穿戴 IMU - 位于右前臂

**特点：**
- 清晰标记各设备位置
- 北方指示器（虚线）
- 设备标签

### 2. 朝向变化时间线
**Canvas: `deviceTimeline`**

- 展示可穿戴 IMU 的 Yaw 角（朝向）随时间的变化
- Y 轴：朝向角度（度数）
- X 轴：时间轴（秒）
- 青色曲线：实时朝向数据
- 游标指示当前选中样本的朝向值

**特点：**
- 网格线和刻度标记
- 自动范围调整
- 交互式游标显示

### 3. 设备统计信息
**UI Cards:**

| 卡片 | 说明 |
|------|------|
| `boardYaw` | 当前选中样本的朝向角 |
| `boardYawRange` | 整个记录中朝向角的范围 |
| `wearableYaw` | 可穿戴设备的当前朝向（与 boardYaw 同步） |
| `wearableYawRange` | 可穿戴设备朝向的范围 |

## 技术实现

### 新增 JavaScript 函数

#### `drawDeviceLayout(samples)`
```javascript
/**
 * 绘制设备位置俯视图
 * @param {Array} samples - 所有样本数据
 */
```
- 在 `devicePosition` canvas 上绘制
- 相对位置基于项目硬件定义
- 自适应画布大小

#### `drawDeviceTimeline(samples, cursorIndex)`
```javascript
/**
 * 绘制朝向变化时间线
 * @param {Array} samples - 所有样本数据
 * @param {number} cursorIndex - 当前选中样本的索引
 */
```
- 在 `deviceTimeline` canvas 上绘制
- 自动处理缺失的朝向数据
- 显示实时游标

#### `updateDeviceStats(samples)`
```javascript
/**
 * 更新设备统计信息
 * @param {Array} samples - 所有样本数据
 */
```
- 计算朝向统计数据
- 更新 UI 统计卡片
- 处理无效数据

### 集成点

这些函数被集成到 `renderSelection()` 中：

```javascript
function renderSelection(index) {
  // ... 现有代码 ...
  drawDeviceLayout(samples);          // 新增
  drawDeviceTimeline(samples, index); // 新增
  updateDeviceStats(samples);         // 新增
}
```

## 数据需求

查看器需要以下 CSV 列来完整显示设备可视化：

| 列名 | 用途 |
|------|------|
| `wt_yaw_deg` | 可穿戴 IMU 的偏航角（度数） |
| `t_ms` | 时间戳（毫秒） |
| `paired` | 数据配对标记 |

**注意：** 如果 CSV 中缺少 `wt_yaw_deg` 列，时间线将显示"朝向数据不可用"提示。

## 样式定义

### CSS 新增

```css
.device-layout {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
  margin-top: 12px;
}

.device-layout canvas {
  height: 280px;
}

@media (max-width: 760px) {
  .device-layout {
    grid-template-columns: 1fr;
  }
}
```

- 桌面布局：设备位置和时间线并排显示
- 移动布局：堆叠显示

## 用户交互

### 播放和暂停
- 点击"播放"按钮自动播放整个记录
- 设备可视化实时更新以匹配当前样本

### 滑块选择
- 拖动时间滑块选择特定样本
- 设备可视化和统计信息实时更新

### 上传 CSV
- 拖入或选择 CSV 文件
- 查看器自动解析并显示设备数据
- 如果格式不正确，显示错误提示

## 显示示例

### 俯视图
```
┌─────────────────────────────────┐
│      设备位置与朝向              │
│     ESP32 主板 | 可穿戴 IMU      │
│  俯视图 · 北方 ↑ | 朝向时间线    │
│                 |               │
│    ○ ESP32  |  ○ Device Yaw    │
│    ○ 可穿戴  |  [线条图形]      │
│             |                 │
└─────────────────────────────────┘
```

## 兼容性

### 浏览器支持
- Chrome/Chromium 60+
- Firefox 55+
- Safari 12+
- Edge 79+

### 功能检测
- Canvas 2D 上下文
- `devicePixelRatio` 支持
- 文件 API 和 FileReader

## 故障排除

### 设备位置不显示
- ✓ 检查 CSV 是否有有效的配对数据
- ✓ 确保浏览器支持 Canvas API

### 时间线为空
- ✓ CSV 中可能缺少 `wt_yaw_deg` 列
- ✓ 所有朝向数据可能是 null 或无效

### 统计数据显示为"—"
- ✓ 加载 CSV 后再查看
- ✓ 检查数据有效性

## 与 Python 可视化的关系

这个 HTML 查看器中的设备位置可视化与独立的 Python 脚本互补：

| 工具 | 用途 | 优势 |
|------|------|------|
| `visualize_imu_setup.py` | 独立命令行脚本 | 快速生成 PNG；无需浏览器 |
| `imu_log_viewer.html` | 交互式 Web 应用 | 实时探索数据；集成播放控制 |

## 开发说明

### 添加新的可视化
要在查看器中添加新的可视化：

1. 在 HTML 中添加新的 `<canvas>` 元素
2. 在 `elements` 对象中注册该元素
3. 创建对应的 `draw*()` 函数
4. 在 `renderSelection()` 中调用新函数
5. 在 `drawEmpty()` 中添加初始化代码

### 颜色方案
查看器使用固定的深色主题：
- 背景：`#0b1220`
- 文本：`#e7eef9`
- 网格：`#26364b`
- 主板：`#7aa2ff`（蓝色）
- 前臂：`#4de0bd`（青色）

## 性能考虑

- Canvas 绘制在每个样本变化时进行
- 采用 `setupCanvas()` 处理高 DPI 屏幕
- 大型数据集（>100K 样本）可能需要 1-2 秒更新

## 更新历史

### 版本 2.0（2024-11-12）
- ✨ 新增设备位置俯视图
- ✨ 新增朝向变化时间线
- ✨ 新增统计信息卡片
- 🔧 优化响应式设计

### 版本 1.0（原始版本）
- 双 IMU 姿态对齐和展示
- 俯仰和横滚变化曲线
- 侧视和俯视姿态示意

## 许可和贡献

此增强功能是项目的一部分，遵循项目的许可证条款。
