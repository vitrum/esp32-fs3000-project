# 快速开始指南

## 最简单的方式（5分钟快速开始）

### 1️⃣ 准备CSV文件
将你的IMU CSV文件放在一个目录中，例如 `./imu_data/`

### 2️⃣ 运行脚本
```bash
cd 你的工作目录
python path/to/merge_imu_csv_to_bvh.py ./imu_data ./output
```

### 3️⃣ 查看结果
输出目录中会生成：
- `merged_imu.csv` - 合并后的CSV
- `merged_imu.bvh` - 运动捕捉文件
- `merged_imu.h5` - 元数据文件（可选）

---

## 实际例子

### 例子1：从下载目录合并数据
```bash
python merge_imu_csv_to_bvh.py ~/Downloads/IMU/2026-11-12 ~/output
```

### 例子2：自定义帧率和输出
```bash
python merge_imu_csv_to_bvh.py ./imu_data ./output \
  --output-prefix ./output/session_001 \
  --fps 30 \
  --force
```

### 例子3：仅生成BVH（跳过HDF5）
```bash
python merge_imu_csv_to_bvh.py ./imu_data ./output --no-hdf5
```

---

## 参数速查表

| 参数 | 说明 | 示例 |
|------|------|------|
| `--fps` | 帧率 | `--fps 30` |
| `--tau` | 滤波器时常 | `--tau 1.0` |
| `--no-hdf5` | 跳过HDF5 | 无需参数 |
| `--force` | 覆盖输出 | 无需参数 |

---

## 常见问题速答

**Q: 脚本找不到怎么办？**
A: 确保使用完整路径：`python /full/path/to/merge_imu_csv_to_bvh.py`

**Q: 需要安装依赖吗？**
A: 不需要，除非要生成HDF5文件。运行 `pip install h5py` 即可。

**Q: BVH文件怎么查看？**
A: 在线工具：https://www.bvhview.com/ 上传文件即可查看。

**Q: 支持哪些Python版本？**
A: Python 3.8+

---

## 获取帮助

```bash
python merge_imu_csv_to_bvh.py --help
```

详细文档见：`MERGE_IMU_TO_BVH_README.md`
