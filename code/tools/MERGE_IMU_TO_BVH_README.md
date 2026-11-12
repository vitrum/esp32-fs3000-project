# IMU CSV 合并与 BVH 生成工具

这是一个独立的Python脚本，可以在任何目录运行，用于：

1. **合并多个IMU CSV文件** - 将来自多个设备记录会话的CSV文件合并为一个
2. **生成BVH和HDF5输出** - 将合并后的CSV转换为运动捕捉BVH格式和HDF5元数据格式

## 功能特性

- ✅ **完全独立** - 可脱离项目在任何目录单独运行
- ✅ **自动时间戳调整** - 合并多个CSV文件时自动处理时间戳同步
- ✅ **灵活的输出选项** - 支持BVH、HDF5或仅BVH输出
- ✅ **可配置的参数** - 支持自定义帧率、滤波时常、插值间隙等参数
- ✅ **详细的错误信息** - 清晰的错误提示帮助调试数据问题

## 安装依赖

### 基本依赖（只生成BVH）
```bash
# 无额外依赖，Python 3.8+ 即可
```

### 完整依赖（包括HDF5）
```bash
python -m pip install -r code/tools/requirements-imu-convert.txt
# 或单独安装：
python -m pip install h5py>=3.11,<4
```

## 使用方法

### 基本用法

```bash
python merge_imu_csv_to_bvh.py INPUT_DIR OUTPUT_DIR [OPTIONS]
```

### 参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `INPUT_DIR` | 包含IMU CSV文件的目录 | 当前目录 |
| `OUTPUT_DIR` | 输出文件目录 | INPUT_DIR |
| `-o, --output-prefix` | 自定义输出文件名前缀 | `OUTPUT_DIR/merged_imu` |
| `--fps` | BVH帧率 (1-200) | 10 |
| `--tau` | 互补滤波器时常(秒) | 0.5 |
| `--max-interpolation-gap-ms` | 最大插值间隙(ms) | 250 |
| `--no-hdf5` | 跳过HDF5生成，仅生成BVH | 否 |
| `--force` | 覆盖现有输出文件 | 否 |

### 使用示例

#### 1. 合并当前目录的CSV文件，输出到output目录
```bash
python merge_imu_csv_to_bvh.py . ./output
```

#### 2. 合并/data/imu目录的CSV文件到/output/results
```bash
python merge_imu_csv_to_bvh.py /data/imu /output/results
```

#### 3. 自定义输出前缀和帧率
```bash
python merge_imu_csv_to_bvh.py ./imu_data ./output \
  --output-prefix ./output/my_session \
  --fps 30 \
  --force
```

#### 4. 仅生成BVH（不生成HDF5）
```bash
python merge_imu_csv_to_bvh.py ./imu_data ./output --no-hdf5
```

#### 5. 使用自定义互补滤波器参数
```bash
python merge_imu_csv_to_bvh.py ./imu_data ./output \
  --tau 1.0 \
  --max-interpolation-gap-ms 500
```

## 输出文件

脚本生成以下文件：

### 1. `merged_imu.csv` - 合并后的CSV文件
- 包含所有CSV文件中的所有行
- 时间戳已调整为单调递增
- 保留原始CSV格式和所有列

### 2. `merged_imu.bvh` - BVH运动捕捉文件
- 标准BVH格式，可用于运动捕捉软件
- 包含人体骨架（仅右前臂由IMU驱动）
- 固定的骑行姿态（髋部和腿部静止，躯干固定-35°前倾）
- 可在BVHView等工具中查看

### 3. `merged_imu.h5` - HDF5元数据文件（可选）
- 包含所有中间处理数据
- 详细的传感器数据和方向估计
- 便于进一步分析和研究

## CSV文件格式要求

输入CSV文件必须包含以下列（至少）：

```
t_ms, board_t_ms, wt_t_ms, board_seq, wt_seq,
board_valid, wt_valid, paired, skew_ms,
board_ax_g, board_ay_g, board_az_g,
board_gx_dps, board_gy_dps, board_gz_dps,
wt_ax_g, wt_ay_g, wt_az_g,
wt_gx_dps, wt_gy_dps, wt_gz_dps,
wt_roll_deg, wt_pitch_deg, wt_yaw_deg
```

## 原理说明

### 合并策略
- 脚本按字母顺序扫描目录中的所有CSV文件
- 时间戳自动调整以确保单调递增（解决多个设备记录的时间戳重叠问题）
- 所有与时间相关的列（t_ms, board_t_ms, wt_t_ms）都会相应调整

### BVH生成
1. **方向估计** - 使用互补滤波器估计每个IMU的方向（roll/pitch/yaw）
2. **重采样** - 将数据重采样为统一的帧率
3. **骨架映射** - 将IMU数据映射到固定的人体骨架
4. **插值** - 在样本间隙处进行线性插值

### 骨架结构
- **固定部分** - 髋部、双腿、左臂保持静止
- **动态部分** - 躯干（固定前倾）和右前臂（由穿戴式IMU驱动）
- **坐标系** - 以足部为原点的全身坐标系（用于BVHView显示）

## 常见问题

### Q: 脚本可以在其他项目中使用吗？
**A:** 是的！脚本完全独立，只需Python 3.8+环境即可运行。

### Q: 如何处理时间戳不连续的情况？
**A:** 脚本会自动检测并调整时间戳，使其单调递增。可以使用`--max-interpolation-gap-ms`参数控制插值行为。

### Q: HDF5文件为空？
**A:** 说明h5py未安装。使用`--no-hdf5`参数跳过HDF5生成，或安装h5py依赖。

### Q: 可以只合并CSV文件，不生成BVH吗？
**A:** 不可以。脚本的主要目的是生成BVH。如果只想合并CSV，可以手动在Excel或pandas中合并。

### Q: 生成的BVH文件如何查看？
**A:** 可以使用以下工具查看：
- BVHView - https://www.bvhview.com/
- Blender - 安装BVH导入插件
- MotionBuilder - 原生支持BVH
- 其他支持BVH的运动捕捉软件

## 技术细节

### 时间戳调整算法
```python
if merged_rows:
    last_t_ms = int(merged_rows[-1]["t_ms"])
    first_t_ms_this_file = int(file_rows[0]["t_ms"])
    time_offset = last_t_ms - first_t_ms_this_file + 10
else:
    time_offset = 0

new_val = (val + time_offset) & UINT32_MASK
```

### 互补滤波器公式
```
alpha = tau / (tau + dt)
roll[i] = alpha * gyro_roll + (1 - alpha) * accel_roll
pitch[i] = alpha * gyro_pitch + (1 - alpha) * accel_pitch
```

### 数据验证
- 检查所有必需列是否存在
- 验证时间戳的单调性和uint32范围
- 验证配对的传感器时间戳一致性（允许偏差 ≤ 250ms）
- 确保存在至少一个有效的同步样本对

## 性能考虑

- 脚本按流处理CSV文件，内存效率高
- 大型CSV文件（>100MB）也能快速处理
- BVH生成速度取决于帧率和样本数量（通常<1秒）

## 测试验证

脚本已通过以下测试：
- ✅ 多个CSV文件合并
- ✅ 时间戳调整和验证
- ✅ BVH生成和格式验证
- ✅ 大型数据集处理（>1000行）
- ✅ 错误处理和用户提示

## 相关文件

- `imu_to_bvh_hdf5.py` - 原始单文件转换脚本
- `requirements-imu-convert.txt` - Python依赖清单
- `test_imu_to_bvh_hdf5.py` - 单元测试

## 许可证

与项目一致
