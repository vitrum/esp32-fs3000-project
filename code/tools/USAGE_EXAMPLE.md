# 使用示例 - 处理Downloads中的IMU数据

## 场景：处理 `/Users/vitrum/Downloads/IMU/2026-11-12/` 中的数据

### 方案1：最简单的方式（推荐）

```bash
# 在任意目录运行
cd ~/Desktop  # 或你的工作目录

# 合并所有CSV文件并生成BVH
python ~/Development/AI/esp32-fs3000-project/code/tools/merge_imu_csv_to_bvh.py \
  ~/Downloads/IMU/2026-11-12 \
  ./imu_output
```

**输出文件在：** `~/Desktop/imu_output/`

---

### 方案2：自定义输出位置和参数

```bash
# 使用自定义帧率和输出前缀
python ~/Development/AI/esp32-fs3000-project/code/tools/merge_imu_csv_to_bvh.py \
  ~/Downloads/IMU/2026-11-12 \
  ~/Output \
  --output-prefix ~/Output/2026-11-12-session \
  --fps 30 \
  --force
```

**输出文件在：** `~/Output/2026-11-12-session.bvh` 和 `~/Output/2026-11-12-session.csv`

---

### 方案3：仅生成BVH，跳过HDF5

```bash
python ~/Development/AI/esp32-fs3000-project/code/tools/merge_imu_csv_to_bvh.py \
  ~/Downloads/IMU/2026-11-12 \
  ~/Output \
  --no-hdf5
```

**优点：** 更快，输出文件更小

---

## 处理结果

### 生成的文件

| 文件 | 说明 | 大小 |
|------|------|------|
| `merged_imu.csv` | 所有CSV合并后的数据 | 取决于数据量 |
| `merged_imu.bvh` | 运动捕捉动画文件 | 取决于帧率 |
| `merged_imu.h5` | 元数据和中间数据 | 可选 |

### 查看BVH动画

1. **在线查看**（推荐，无需安装）
   - 打开 https://www.bvhview.com/
   - 上传 `merged_imu.bvh` 文件
   - 点击播放

2. **在Blender中查看**
   - 打开Blender
   - File → Import → BVH (.bvh)
   - 选择生成的BVH文件

3. **其他工具**
   - MotionBuilder（原生支持）
   - Maya（需要插件）
   - 其他运动捕捉软件

---

## 常见参数配置

### 不同帧率的设置

```bash
# 10 FPS（默认，低细节）
--fps 10

# 30 FPS（标准）
--fps 30

# 60 FPS（高细节，文件大）
--fps 60
```

### 不同的滤波器参数

```bash
# 快速响应（低延迟）
--tau 0.1

# 标准平衡（默认）
--tau 0.5

# 平滑稳定（高延迟）
--tau 2.0
```

### 插值间隙设置

```bash
# 较小的间隙（严格）
--max-interpolation-gap-ms 100

# 默认（标准）
--max-interpolation-gap-ms 250

# 较大的间隙（宽松）
--max-interpolation-gap-ms 500
```

---

## 完整工作流示例

### 场景：分析多个记录会话

```bash
#!/bin/bash

# 创建输出目录
mkdir -p ~/IMU_Analysis/2026-11-12

# 处理第一个会话
python ~/Development/AI/esp32-fs3000-project/code/tools/merge_imu_csv_to_bvh.py \
  ~/Downloads/IMU/2026-11-12 \
  ~/IMU_Analysis/2026-11-12/session-001 \
  --fps 30 \
  --force

# 处理第二个会话
python ~/Development/AI/esp32-fs3000-project/code/tools/merge_imu_csv_to_bvh.py \
  ~/Downloads/IMU/2026-11-13 \
  ~/IMU_Analysis/2026-11-13/session-001 \
  --fps 30 \
  --force

echo "✓ 所有会话处理完成"
```

---

## 数据验证

处理完成后检查输出：

```bash
# 检查合并的CSV数据
head -5 ~/imu_output/merged_imu.csv

# 查看BVH文件信息
head -20 ~/imu_output/merged_imu.bvh

# 统计数据行数
wc -l ~/imu_output/merged_imu.csv
```

---

## 故障排查

### 问题：脚本找不到
**解决：** 使用完整路径
```bash
python /Users/vitrum/Development/AI/esp32-fs3000-project/code/tools/merge_imu_csv_to_bvh.py ...
```

### 问题：找不到CSV文件
**解决：** 确保目录中有.csv文件且文件名可识别
```bash
ls ~/Downloads/IMU/2026-11-12/*.csv
```

### 问题：时间戳不对
**解决：** 这是正常的！脚本会自动调整以确保连续性

### 问题：HDF5文件为空
**解决：** 这是正常的！只需使用 `--no-hdf5` 跳过它

---

## 快速命令模板

复制并修改以下命令：

```bash
# 基础版本
python ~/Development/AI/esp32-fs3000-project/code/tools/merge_imu_csv_to_bvh.py INPUT_DIR OUTPUT_DIR

# 完整版本
python ~/Development/AI/esp32-fs3000-project/code/tools/merge_imu_csv_to_bvh.py \
  ~/Downloads/IMU/2026-11-12 \
  ~/Output \
  --fps 30 \
  --tau 0.5 \
  --max-interpolation-gap-ms 250 \
  --force
```

---

## 获取帮助

```bash
python ~/Development/AI/esp32-fs3000-project/code/tools/merge_imu_csv_to_bvh.py --help
```

详细文档见：`MERGE_IMU_TO_BVH_README.md`
