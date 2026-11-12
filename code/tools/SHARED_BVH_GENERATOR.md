# Shared BVH Generator Module

## Overview

The `imu_bvh_generator.py` module provides shared, reusable BVH (Biovision Hierarchical Data) file generation logic for all IMU-to-BVH conversion tools in this project.

## Purpose

This module ensures **consistent BVH output** across multiple conversion tools:
- `imu_to_bvh_hdf5.py` - Original dual-IMU CSV to BVH/HDF5 converter
- `merge_imu_csv_to_bvh.py` - Merged CSV to BVH converter (standalone tool)
- Future converters that need BVH output

## Module Components

### Constants

- `TORSO_LEAN_DEG` - Fixed torso lean angle (-35°)
- `BVH_SKELETON` - Hierarchical skeleton structure defining the human pose model

### Public Functions

#### `bvh_joint_text(joint, depth: int) -> Tuple[List[str], List[str]]`
Generates BVH JOINT block text and corresponding channel names for a skeleton joint and its children.

#### `bvh_channel_rows(frame_rows: Sequence[Sequence[float]]) -> List[Tuple[float, ...]]`
Converts frame orientation data (board roll/pitch/yaw, arm roll/pitch/yaw) to BVH channel values.

#### `bvh_joint_names() -> List[str]`
Returns the ordered list of all joint names in the BVH skeleton.

#### `bvh_channel_names() -> List[str]`
Returns the ordered list of all channel names that appear in the BVH file.

#### `bvh_text(frame_rows: Sequence[Sequence[float]], frame_time: float) -> str`
Generates complete BVH file content as a string.

## Usage in Your Tools

### In `merge_imu_csv_to_bvh.py`:

```python
import imu_bvh_generator as bvh_gen

# Generate BVH file
bvh_content = bvh_gen.bvh_text(frame_rows, frame_time)
path.write_text(bvh_content, encoding="utf-8")

# Use in HDF5 metadata
bvh_group.attrs["joint_names"] = ",".join(bvh_gen.bvh_joint_names())
_dataset(bvh_group, "frame_channels", bvh_gen.bvh_channel_rows(frame_rows), ...)
```

### In `imu_to_bvh_hdf5.py`:

```python
import imu_bvh_generator as bvh_gen

# Generate BVH file
_atomic_write(bvh_path, lambda path: path.write_text(
    bvh_gen.bvh_text(frame_rows, frame_time), 
    encoding="utf-8"
))

# Use in HDF5 metadata
bvh_group.attrs["joint_names"] = ",".join(bvh_gen.bvh_joint_names())
```

## Skeleton Model

The BVH skeleton represents a human in a **static cycling pose**:

- **Hips**: Root joint at 95cm height above ground
- **Legs**: Fixed hip/knee/ankle angles (not driven by IMU data)
- **Torso/Spine**: Fixed -35° forward lean
- **Arms**: Static cycling pose except right forearm
- **Right Forearm**: Follows wearable-minus-board relative roll/pitch from dual-IMU

## Channel Organization

- **Root (Ground)**: 6 channels - position (X, Y, Z) + rotation (Z, X, Y)
- **Hips**: 3 channels - rotation (Z, X, Y)
- **All other joints**: 3 channels each - rotation (Z, X, Y)
- **Total**: 84 channels (6 root + 78 joint rotations)

## Testing

Run the test suite:

```bash
cd /path/to/tools
python3 << 'EOF'
import imu_bvh_generator as bvh_gen

# Verify module loads correctly
print(f"Skeleton joints: {len(bvh_gen.BVH_SKELETON)}")
print(f"Channel names: {len(bvh_gen.bvh_channel_names())}")

# Test BVH generation
test_frames = [(0, 0, 0, 0, 0, 0), (1, 2, 3, 4, 5, 6)]
bvh = bvh_gen.bvh_text(test_frames, 0.1)
print(f"Generated BVH: {len(bvh)} bytes")
EOF
```

## Migration History

- **Before**: BVH generation logic was duplicated in both `imu_to_bvh_hdf5.py` and `merge_imu_csv_to_bvh.py`
- **After**: Both tools import and use the single `imu_bvh_generator` module
- **Benefit**: Single source of truth ensures consistent output format and behavior
