# IMU Device Position Visualization

两个可视化脚本用于展示 ESP32 主板和可穿戴 IMU 设备的位置、朝向和运动方向。

## 脚本说明

### 1. `visualize_imu_setup.py` (Matplotlib 图形版)
创建专业的图形化可视化，包含两个子图：
- **左图**：俯视图显示设备位置和平均朝向（带箭头表示车头方向）
- **右图**：朝向角度（Yaw）随时间的变化曲线

**输出**：PNG 图像文件

**用法**：
```bash
# 直接显示在窗口
python3 tools/visualize_imu_setup.py <merged_imu.csv>

# 保存到文件
python3 tools/visualize_imu_setup.py <merged_imu.csv> -o output.png
```

**示例**：
```bash
cd code
python3 tools/visualize_imu_setup.py /Users/vitrum/Downloads/IMU/output/merged_imu.csv -o imu_visualization.png
```

### 2. `visualize_imu_setup_ascii.py` (ASCII 文本版)
在终端中创建美观的 ASCII 艺术可视化，可直接查看或记录到日志。

**特点**：
- 自包含，无需 matplotlib
- 展示设备俯视图布局
- 方向指示器和方位缩写 (N, NE, E, SE, S, SW, W, NW)
- 前 50 个样本的朝向时间线
- 彩色 Unicode 符号和进度条

**用法**：
```bash
python3 tools/visualize_imu_setup_ascii.py <merged_imu.csv>
```

**示例**：
```bash
cd code
python3 tools/visualize_imu_setup_ascii.py /Users/vitrum/Downloads/IMU/output/merged_imu.csv
```

## 功能

两个脚本都能：
- ✓ 加载已合并的 IMU CSV 数据（使用 `merge_imu_csv_to_bvh.py` 生成）
- ✓ 验证配对且有效的传感器数据
- ✓ 计算统计信息（样本数、平均朝向角、标准差）
- ✓ 展示 ESP32 主板和可穿戴 IMU 的相对位置
- ✓ 用箭头表示各设备的朝向（车头方向）
- ✓ 显示朝向随时间的变化

## 设备位置设置

根据项目的硬件布局定义：
- **ESP32 主板**：在骑手胸部，靠近中线向右 5cm
- **可穿戴 IMU**：在右前臂，距离车轴约 15cm，距中线向右 10cm

## 依赖项

### Matplotlib 版本
- Python 3.6+
- matplotlib
- numpy

安装依赖：
```bash
python -m pip install matplotlib numpy
```

### ASCII 版本
- Python 3.6+（无其他依赖）

## 输出示例

**Matplotlib PNG**：
- 尺寸：2084 × 884 像素
- 清晰展示两个时间轴的设备数据

**ASCII 输出**（终端）：
- 带有 Unicode 边框和方向箭头的格式化布局
- 进度条表示相对朝向角度
- 方向标签和样本统计信息

## 与 BVH 生成的集成

这些可视化脚本配合使用 `merge_imu_csv_to_bvh.py` 工作流：

1. **合并 CSV**：`merge_imu_csv_to_bvh.py` 合并多个设备的 IMU 数据
2. **可视化**：本脚本验证并展示数据的空间关系
3. **验证**：检查朝向数据的完整性和一致性
4. **BVH 生成**：使用相同数据生成人体模型的 BVH 动画

## 故障排除

### "No valid paired sensor data found"
- 检查 CSV 是否来自正确的合并流程
- 验证 `paired` 列为 1 且 `board_valid` 和 `wt_valid` 均为 1
- 确保数据不为空或格式损坏

### matplotlib 导入错误
- 运行：`python -m pip install matplotlib numpy`

### 输出看不到或乱码
- Matplotlib 版本：检查是否已正确安装显示后端
- ASCII 版本：确保终端支持 UTF-8 编码

## 许可证和归属

这些脚本是项目的一部分，与 `merge_imu_csv_to_bvh.py` 和 `imu_bvh_generator.py` 共享 BVH 生成逻辑。
