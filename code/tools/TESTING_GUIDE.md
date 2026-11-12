# IMU 查看器使用与测试指南

## 🔧 启动本地服务器

由于浏览器安全限制，**必须通过 HTTP 服务器访问**，而不能直接用 `file://` 协议打开 HTML 文件。

```bash
cd /Users/vitrum/Development/AI/esp32-fs3000-project/code/tools
python3 -m http.server 8765
```

然后在浏览器打开：
```
http://localhost:8765/imu_log_viewer.html
```

## 📥 上传 CSV 文件

### 方法 1: 使用"选择 CSV 文件"按钮
1. 点击 "选择 CSV 文件" 按钮
2. 选择一个 IMU CSV 文件（如 `merged_imu.csv`）
3. 文件会自动加载并显示统计信息和可视化

### 方法 2: 拖放上传
1. 找一个 CSV 文件（如 `merged_imu.csv`）
2. 拖入页面上的 "选择或拖入 IMU CSV" 区域
3. 文件会自动处理

### 方法 3: 自动加载(URL参数)
```
http://localhost:8765/imu_log_viewer.html?file=test_data.csv
```

如果 `test_data.csv` 在 tools 目录中，页面会自动加载它。

**注意**: 由于浏览器自动化限制，URL 参数方式可能不在所有环境中可用。优先使用方法 1 或 2。

## ✅ 验证加载成功

文件加载成功时会显示:
- ✓ 绿色成功消息
- 表格中的数据统计（记录行数、有效对数等）
- 启用"播放"按钮和时间轴滑块
- 三个可视化面板中的数据

## 📊 可视化内容

### 1. 车架与前臂关系示意
- **侧视图**: 显示主机板和前臂相对于自行车的俯仰角
- **俯视图**: 显示设备在自行车上的相对位置

### 2. 姿态变化时间线
- **曲线图**: 显示主机和前臂的俯仰/横滚变化
- **颜色说明**:
  - 蓝色: 主机板载 IMU
  - 青色: 前臂 BLE IMU  
  - 黄色: 前臂变化 - 主机变化

### 3. 设备位置与朝向（新增）
- **俯视图**: ESP32 主板和可穿戴 IMU 的位置，箭头显示车头方向
- **朝向时间线**: 显示两个设备的 Yaw 角随时间的变化
- **统计信息**: 主板和可穿戴设备的朝向范围

## 🎮 交互控制

- **播放按钮**: 开始/暂停自动播放
- **时间轴滑块**: 手动选择查看的时刻
- **右手抓握位置**: 显示读取的抓握状态（通常为手变）

## 🐛 测试文件

推荐使用以下测试文件:
- `/Users/vitrum/Downloads/IMU/output/merged_imu.csv` (12 MB, 50137 行)
- `/Users/vitrum/Downloads/IMU/2026-11-12/I47AE1CE0-0.csv` 等原始日志文件

## ⚠️ 常见问题

### 问题: "文件选择器无响应"
**原因**: 使用了 `file://` 协议而不是 HTTP
**解决**: 按照上述指南启动 HTTP 服务器

### 问题: 加载后没有数据显示
**原因**: CSV 格式不符合要求
**检查**: 
- 确保有所有必需的列: `t_ms`, `board_t_ms`, `wt_t_ms`, `board_valid`, `wt_valid`, `paired`, `skew_ms`, `board_ax_g`, `board_ay_g`, `board_az_g`, `wt_ax_g`, `wt_ay_g`, `wt_az_g`
- CSV 中至少有 2 对有效的配对数据

### 问题: "姿态数据不可用"
**原因**: CSV 缺少 `wt_yaw_deg` 列
**解决**: 使用包含该列的完整合并 CSV 文件

## 🔍 调试

打开浏览器开发者工具（F12）查看控制台信息：
- 搜索 `[imu_viewer]` 前缀的日志
- 查看任何 JavaScript 错误
- 检查网络请求是否成功

## 📝 文件清单

- `imu_log_viewer.html` - 主页面
- `imu_log_viewer_debug.html` - 调试和测试辅助页面
- `test_loader.html` - 文件加载测试页面
- `test_data.csv` - 测试数据副本（如果存在）
- `visualize_imu_setup.py` - Python Matplotlib 可视化脚本
- `visualize_imu_setup_ascii.py` - Python ASCII 可视化脚本
- `VISUALIZATION_README.md` - Python 脚本文档

