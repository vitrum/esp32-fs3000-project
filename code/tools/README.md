# IMU 日志查看器工具集

完整的 ESP32 双 IMU（主板 + 可穿戴）姿态数据可视化和分析工具包。包含 Web 交互式查看器、Python 独立脚本和测试工具。

## 🎯 快速开始

### 使用 Web 查看器（推荐）

```bash
# 1. 启动本地服务器
cd /path/to/code/tools
python3 -m http.server 8765

# 2. 打开浏览器
http://localhost:8765/imu_log_viewer.html

# 3. 点击"选择 CSV 文件"或拖放上传
```

### 使用 Python 脚本

```bash
# Matplotlib 可视化（需要 matplotlib）
python3 visualize_imu_setup.py merged_imu.csv

# ASCII 文本可视化（无依赖）
python3 visualize_imu_setup_ascii.py merged_imu.csv
```

## 📁 文件说明

### 核心文件
| 文件 | 说明 | 大小 |
|------|------|------|
| `imu_log_viewer.html` | 主 Web 界面 | 44K |
| `visualize_imu_setup.py` | Matplotlib 可视化 | 9K |
| `visualize_imu_setup_ascii.py` | ASCII 可视化 | 7K |

### 文档和工具
| 文件 | 说明 |
|------|------|
| `TESTING_GUIDE.md` | 详细使用指南 |
| `TESTING_REPORT.md` | 功能测试报告 |
| `VISUALIZATION_README.md` | Python 脚本说明 |
| `test_loader.html` | CSV 加载测试工具 |
| `imu_log_viewer_debug.html` | 调试辅助工具 |

### 测试数据
| 文件 | 说明 |
|------|------|
| `test_data.csv` | 示例 CSV 数据（12 MB） |

## ✨ 核心功能

### Web 查看器 (imu_log_viewer.html)

1. **CSV 上传与解析**
   - 支持拖放上传
   - 自动数据验证和格式检查
   - 实时统计显示

2. **姿态变化可视化**
   - 俯仰和横滚变化曲线
   - 主机板 vs 前臂对比
   - 相对变化差异分析

3. **设备位置与朝向（新增）**
   - 俯视图显示 ESP32 和可穿戴 IMU 位置
   - 箭头指示车头方向（Yaw 角）
   - 朝向随时间的变化曲线
   - 多项统计信息

4. **交互控制**
   - 播放/暂停自动动画
   - 时间轴滑块选择任意时刻
   - 实时数据显示

### Python 脚本

1. **visualize_imu_setup.py**
   - 生成高质量 PNG 图表
   - Matplotlib 专业排版
   - 设备位置和朝向可视化

2. **visualize_imu_setup_ascii.py**
   - 终端直接输出
   - 不需要图形库
   - 快速预览数据

## 📊 支持的数据格式

### 必需列（最少 13 列）
```
t_ms, board_t_ms, wt_t_ms, board_valid, wt_valid, paired, skew_ms,
board_ax_g, board_ay_g, board_az_g,
wt_ax_g, wt_ay_g, wt_az_g
```

### 可选但推荐列
```
wt_yaw_deg, grip_position, board_gx_dps, board_gy_dps, board_gz_dps, ...
```

### 格式要求
- CSV 文本格式，逗号分隔
- 首行为列名
- 至少 2 对有效同步数据
- 支持 UTF-8 BOM

## 🔧 技术特点

- **无外部依赖** - 纯 HTML5 + JavaScript（Web 版）
- **离线工作** - 数据完全在本地浏览器处理
- **高 DPI 适配** - 自动检测和适配 Retina 屏幕
- **响应式设计** - 适配各种屏幕尺寸
- **实时更新** - 滑块交互即时渲染

## 🚀 常见用法

### 合并多个 CSV 文件
使用相关的 ESP-IDF 工具或自定义脚本将多个单设备日志合并。

### 导出数据报表
使用 Python 脚本生成 PNG 报表：
```bash
python3 visualize_imu_setup.py data.csv output.png
```

### 批量处理
用 Python 脚本处理多个文件：
```bash
for file in *.csv; do
  python3 visualize_imu_setup_ascii.py "$file"
done
```

## ⚠️ 注意事项

1. **必须使用 HTTP 服务器**  
   Web 页面不能直接用 `file://` 打开，需要本地 HTTP 服务器

2. **文件大小限制**  
   超过 50 MB 的文件可能导致 UI 卡顿

3. **浏览器兼容性**  
   需要支持 Canvas 2D API、FileReader API 的现代浏览器

4. **设备位置固定**  
   当前 ESP32 和可穿戴 IMU 位置硬编码，自定义需修改代码

## 📚 详细文档

- **[TESTING_GUIDE.md](TESTING_GUIDE.md)** - 详细使用指南和常见问题
- **[TESTING_REPORT.md](TESTING_REPORT.md)** - 功能测试报告和验证结果
- **[VISUALIZATION_README.md](VISUALIZATION_README.md)** - Python 脚本专门文档
- **[IMU_VIEWER_ENHANCEMENTS.md](IMU_VIEWER_ENHANCEMENTS.md)** - 技术实现细节

## 🐛 调试

在浏览器开发者工具（F12）查看控制台：
```javascript
// 查找所有 [imu_viewer] 标签的日志
console.log("[imu_viewer] 页面已加载");
```

## 📞 支持

遇到问题请检查：
1. 是否通过 HTTP 访问（不是 file://）
2. CSV 文件格式是否正确
3. 浏览器控制台是否有错误信息
4. 查看 TESTING_GUIDE.md 中的常见问题部分

## 📜 许可

这些工具是 ESP32-FS3000 项目的一部分。

---

**最后更新**: 2026-11-12  
**版本**: 1.0.0  
**状态**: ✅ 生产级就绪
