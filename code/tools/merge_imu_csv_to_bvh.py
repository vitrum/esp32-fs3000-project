#!/usr/bin/env python3
"""Merge multiple IMU CSV files and generate BVH output.

This standalone script can be run from any directory to:
1. Merge multiple IMU CSV files from a directory into one combined CSV
2. Generate BVH and HDF5 files from the merged CSV

Usage:
    python merge_imu_csv_to_bvh.py [OPTIONS] INPUT_DIR OUTPUT_DIR

Example:
    python merge_imu_csv_to_bvh.py ./imu_data ./output
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

try:
    import h5py
except ImportError:
    h5py = None

import imu_bvh_generator as bvh_gen


UINT32_MASK = 0xFFFFFFFF
UINT32_HALF = 0x80000000
REQUIRED_COLUMNS = (
    "t_ms", "board_t_ms", "wt_t_ms", "board_seq", "wt_seq",
    "board_valid", "wt_valid", "paired", "skew_ms",
    *(f"{sensor}_a{axis}_g" for sensor in ("board", "wt") for axis in "xyz"),
    *(f"{sensor}_g{axis}_dps" for sensor in ("board", "wt") for axis in "xyz"),
    "wt_roll_deg", "wt_pitch_deg", "wt_yaw_deg",
)


@dataclass
class Sample:
    row_number: int
    values: Dict[str, float]
    board_valid: bool
    wearable_valid: bool
    paired: bool
    accepted_pair: bool
    time_ms: int
    elapsed_ms: int
    board_time_ms: int
    wearable_time_ms: int
    board_sequence: int
    wearable_sequence: int
    skew_ms: int
    board_orientation: Optional[Tuple[float, float, float]] = None
    wearable_orientation: Optional[Tuple[float, float, float]] = None
    board_relative: Optional[Tuple[float, float, float]] = None
    wearable_relative: Optional[Tuple[float, float, float]] = None


def merge_csv_files(input_dir: Path, output_path: Path) -> Path:
    """Merge all CSV files in input_dir into a single CSV.
    
    Adjusts all t_ms, board_t_ms, and wt_t_ms timestamps to be monotonically 
    increasing across files by adding offsets so the next file starts after 
    the previous one ends.
    """
    csv_files = sorted(input_dir.glob("*.csv"))
    if not csv_files:
        raise FileNotFoundError(f"No CSV files found in {input_dir}")
    
    print(f"Found {len(csv_files)} CSV files to merge")
    
    merged_rows = []
    header = None
    time_offset = 0
    
    for csv_file in csv_files:
        print(f"  Reading: {csv_file.name}")
        try:
            with open(csv_file, 'r', encoding='utf-8-sig') as f:
                reader = csv.DictReader(f)
                if reader.fieldnames is None:
                    raise ValueError(f"{csv_file.name}: empty file")
                
                if header is None:
                    header = reader.fieldnames
                elif reader.fieldnames != header:
                    raise ValueError(f"{csv_file.name}: column headers don't match the first file")
                
                file_rows = list(reader)
                if not file_rows:
                    continue
                
                if merged_rows:
                    last_t_ms = int(merged_rows[-1]["t_ms"])
                    first_t_ms_this_file = int(file_rows[0]["t_ms"])
                    time_offset = last_t_ms - first_t_ms_this_file + 10
                else:
                    time_offset = 0
                
                for row in file_rows:
                    new_row = dict(row)
                    for time_key in ("t_ms", "board_t_ms", "wt_t_ms"):
                        if time_key in new_row:
                            val = int(new_row[time_key])
                            new_val = (val + time_offset) & UINT32_MASK
                            new_row[time_key] = str(new_val)
                    merged_rows.append(new_row)
        except Exception as e:
            raise ValueError(f"Error reading {csv_file.name}: {e}") from e
    
    if not merged_rows:
        raise ValueError("No data rows found in any CSV file")
    
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=header)
        writer.writeheader()
        writer.writerows(merged_rows)
    
    print(f"Merged {len(merged_rows)} rows to {output_path.name}")
    return output_path


def _integer(row: Dict[str, str], key: str, row_number: int) -> int:
    try:
        value = int(row[key])
    except (ValueError, TypeError) as error:
        raise ValueError(f"row {row_number}: {key} must be an integer") from error
    if not 0 <= value <= UINT32_MASK:
        raise ValueError(f"row {row_number}: {key} is outside uint32 range")
    return value


def _signed_integer(row: Dict[str, str], key: str, row_number: int) -> int:
    try:
        return int(row[key])
    except (ValueError, TypeError) as error:
        raise ValueError(f"row {row_number}: {key} must be an integer") from error


def _boolean(row: Dict[str, str], key: str, row_number: int) -> bool:
    value = row.get(key, "")
    if value not in ("0", "1"):
        raise ValueError(f"row {row_number}: {key} must be 0 or 1")
    return value == "1"


def _number(row: Dict[str, str], key: str, row_number: int,
            required: bool) -> float:
    try:
        value = float(row[key])
    except (ValueError, TypeError) as error:
        if not required and row.get(key, "").lower() == "nan":
            return math.nan
        raise ValueError(f"row {row_number}: {key} must be numeric") from error
    if required and not math.isfinite(value):
        raise ValueError(f"row {row_number}: {key} must be finite for a valid sensor")
    return value


def _forward_delta(previous: int, current: int, row_number: int, label: str) -> int:
    delta = (current - previous) & UINT32_MASK
    if delta >= UINT32_HALF:
        raise ValueError(f"row {row_number}: {label} moved backwards (not a uint32 wrap)")
    return delta


def load_csv(path: Path) -> Tuple[List[str], List[Sample], bytes]:
    raw_bytes = path.read_bytes()
    try:
        text = raw_bytes.decode("utf-8-sig")
    except UnicodeDecodeError as error:
        raise ValueError("CSV must be UTF-8 encoded") from error

    reader = csv.DictReader(text.splitlines())
    columns = reader.fieldnames or []
    if len(set(columns)) != len(columns):
        raise ValueError("CSV header contains duplicate columns")
    missing = [key for key in REQUIRED_COLUMNS if key not in columns]
    if missing:
        raise ValueError("CSV is missing required columns: " + ", ".join(missing))

    samples: List[Sample] = []
    previous_time: Optional[int] = None
    elapsed_ms = 0
    for row_number, row in enumerate(reader, start=2):
        if None in row or any(value is None for value in row.values()):
            raise ValueError(f"row {row_number}: column count does not match the header")

        time_ms = _integer(row, "t_ms", row_number)
        if previous_time is not None:
            elapsed_ms += _forward_delta(previous_time, time_ms, row_number, "t_ms")
        previous_time = time_ms

        board_valid = _boolean(row, "board_valid", row_number)
        wearable_valid = _boolean(row, "wt_valid", row_number)
        paired = _boolean(row, "paired", row_number)
        board_time = _integer(row, "board_t_ms", row_number)
        wearable_time = _integer(row, "wt_t_ms", row_number)
        board_sequence = _integer(row, "board_seq", row_number)
        wearable_sequence = _integer(row, "wt_seq", row_number)
        skew = _signed_integer(row, "skew_ms", row_number)

        values: Dict[str, float] = {}
        for sensor, valid in (("board", board_valid), ("wt", wearable_valid)):
            for prefix, unit in (("a", "g"), ("g", "dps")):
                for axis in "xyz":
                    key = f"{sensor}_{prefix}{axis}_{unit}"
                    values[key] = _number(row, key, row_number, required=valid)
        for key in ("wt_roll_deg", "wt_pitch_deg", "wt_yaw_deg"):
            values[key] = _number(row, key, row_number, required=wearable_valid)

        accepted = paired and board_valid and wearable_valid
        if accepted:
            board_age = (time_ms - board_time) & UINT32_MASK
            wearable_age = (time_ms - wearable_time) & UINT32_MASK
            if max(board_age, wearable_age) > 250 or abs(skew) > 100 or skew != wearable_age - board_age:
                raise ValueError(f"row {row_number}: paired sensor timestamps are stale or inconsistent")

        samples.append(Sample(
            row_number=row_number,
            values=values,
            board_valid=board_valid,
            wearable_valid=wearable_valid,
            paired=paired,
            accepted_pair=accepted,
            time_ms=time_ms,
            elapsed_ms=elapsed_ms,
            board_time_ms=board_time,
            wearable_time_ms=wearable_time,
            board_sequence=board_sequence,
            wearable_sequence=wearable_sequence,
            skew_ms=skew,
        ))

    if not samples:
        raise ValueError("CSV contains no data rows")
    if not any(sample.accepted_pair for sample in samples):
        raise ValueError("CSV contains no valid synchronized sensor pairs")
    return columns, samples, raw_bytes


def _acceleration_angles(x: float, y: float, z: float) -> Tuple[float, float]:
    norm = math.hypot(x, y, z)
    if norm < 0.05:
        return 0.0, 0.0
    roll = math.degrees(math.atan2(y, z))
    pitch = math.degrees(math.atan2(-x, math.hypot(y, z)))
    return roll, pitch


def _wrap_near(angle: float, reference: float) -> float:
    return reference + (angle - reference + 180.0) % 360.0 - 180.0


def estimate_orientations(samples: Sequence[Sample], tau_seconds: float) -> None:
    for sensor, time_attr, output_attr in (
        ("board", "board_time_ms", "board_orientation"),
        ("wt", "wearable_time_ms", "wearable_orientation"),
    ):
        valid_attr = "board_valid" if sensor == "board" else "wearable_valid"
        previous_time: Optional[int] = None
        orientation: Optional[List[float]] = None

        for sample in samples:
            if not getattr(sample, valid_attr):
                continue
            values = sample.values
            acceleration = [values[f"{sensor}_a{axis}_g"] for axis in "xyz"]
            gyro = [values[f"{sensor}_g{axis}_dps"] for axis in "xyz"]
            measured_roll, measured_pitch = _acceleration_angles(*acceleration)
            timestamp = getattr(sample, time_attr)

            if previous_time is None:
                orientation = [measured_roll, measured_pitch, 0.0]
            else:
                dt_ms = (timestamp - previous_time) & UINT32_MASK
                if dt_ms == 0 or dt_ms >= UINT32_HALF:
                    setattr(sample, output_attr, tuple(orientation))
                    continue
                dt = dt_ms / 1000.0
                if dt <= 0.0 or dt > 1.0:
                    orientation[0] = measured_roll
                    orientation[1] = measured_pitch
                else:
                    alpha = tau_seconds / (tau_seconds + dt)
                    predicted_roll = orientation[0] + gyro[0] * dt
                    predicted_pitch = orientation[1] + gyro[1] * dt
                    orientation[0] = alpha * predicted_roll + (1.0 - alpha) * _wrap_near(
                        measured_roll, predicted_roll)
                    orientation[1] = alpha * predicted_pitch + (1.0 - alpha) * _wrap_near(
                        measured_pitch, predicted_pitch)
                    orientation[2] += gyro[2] * dt
            previous_time = timestamp
            setattr(sample, output_attr, tuple(orientation))

    reference = next(sample for sample in samples if sample.accepted_pair)
    if reference.board_orientation is None or reference.wearable_orientation is None:
        raise ValueError("first valid synchronized pair has no orientation estimate")
    for sample in samples:
        for sensor, attr in (("board", "board_orientation"),
                             ("wearable", "wearable_orientation")):
            orientation = getattr(sample, attr)
            baseline = getattr(reference, attr)
            if orientation is not None and baseline is not None:
                setattr(sample, f"{sensor}_relative", tuple(
                    orientation[axis] - baseline[axis] for axis in range(3)
                ))


def resample_for_bvh(samples: Sequence[Sample], fps: float,
                     max_interpolation_gap_ms: int) -> Tuple[List[float], List[Tuple[float, ...]], List[bool], float]:
    paired = [sample for sample in samples if sample.accepted_pair]
    start_ms = paired[0].elapsed_ms
    end_ms = paired[-1].elapsed_ms
    duration = (end_ms - start_ms) / 1000.0
    if duration <= 0:
        raise ValueError("valid synchronized samples must span a positive duration")
    frame_count = max(2, round(duration * fps) + 1)
    frame_time = duration / (frame_count - 1)
    frame_times = [index * frame_time for index in range(frame_count)]
    frame_rows: List[Tuple[float, ...]] = []
    held_gap: List[bool] = []
    cursor = 0

    for target in frame_times:
        target_ms = start_ms + target * 1000.0
        while cursor + 1 < len(paired) and paired[cursor + 1].elapsed_ms <= target_ms:
            cursor += 1
        left = paired[cursor]
        left_angles = (*left.board_relative, *left.wearable_relative)
        if cursor + 1 == len(paired) or target_ms <= left.elapsed_ms:
            frame_rows.append(tuple(left_angles))
            held_gap.append(False)
            continue

        right = paired[cursor + 1]
        gap = right.elapsed_ms - left.elapsed_ms
        if gap <= 0:
            raise ValueError("paired sample timestamps are not increasing")
        if gap > max_interpolation_gap_ms:
            frame_rows.append(tuple(left_angles))
            held_gap.append(True)
            continue

        fraction = (target_ms - left.elapsed_ms) / gap
        right_angles = (*right.board_relative, *right.wearable_relative)
        frame_rows.append(tuple(a + (b - a) * fraction for a, b in zip(left_angles, right_angles)))
        held_gap.append(False)

    return frame_times, frame_rows, held_gap, frame_time



def _dataset(group: h5py.Group, name: str, values, units: str,
             description: str = "", dtype=None) -> None:
    dataset = group.create_dataset(name, data=values, dtype=dtype, compression="gzip",
                                   compression_opts=4, shuffle=True)
    dataset.attrs["units"] = units
    if description:
        dataset.attrs["description"] = description


def write_hdf5(path: Path, source: Path, columns: Sequence[str],
               samples: Sequence[Sample], raw_bytes: bytes,
               frame_times: Sequence[float], frame_rows: Sequence[Sequence[float]],
               held_gap: Sequence[bool], frame_time: float, fps: float,
               tau_seconds: float, max_interpolation_gap_ms: int) -> None:
    if h5py is None:
        print("Warning: h5py not installed, skipping HDF5 output")
        return
    
    with h5py.File(path, "w") as file:
        file.attrs["format"] = "AeroProbe dual-IMU conversion"
        file.attrs["format_version"] = "1.0"
        file.attrs["source_file"] = source.name
        file.attrs["coordinate_calibration"] = "unknown; sensor-native axes"
        file.attrs["bvh_interpretation"] = (
            "human-only illustrative cycling pose; hips and legs are static, torso has a "
            "fixed 35-degree forward lean, right forearm follows wearable-minus-board "
            "relative roll/pitch; single forearm IMU cannot independently measure the "
            "upper arm or elbow angle"
        )
        file.attrs["orientation_method"] = (
            f"per-sensor accelerometer/gyroscope complementary tilt estimate; tau={tau_seconds:g}s; "
            "yaw is gyroscope-integrated and unreferenced"
        )
        file.attrs["missing_bvh_frames"] = (
            f"hold previous paired orientation when sample gap exceeds "
            f"{max_interpolation_gap_ms} ms; see /bvh/held_gap"
        )

        raw_group = file.create_group("raw")
        _dataset(raw_group, "source_csv_utf8", list(raw_bytes), "byte",
                 "Exact source CSV bytes, including original line endings and encoding.",
                 dtype="u1")
        raw_group["source_csv_utf8"].attrs["columns"] = ",".join(columns)

        data_group = file.create_group("samples")
        _dataset(data_group, "t_ms", [sample.time_ms for sample in samples], "ms",
                 "Original uint32 capture time from device boot.")
        _dataset(data_group, "elapsed_s", [sample.elapsed_ms / 1000.0 for sample in samples], "s",
                 "Unwrapped capture time relative to the first CSV row.")
        for key, values, units in (
            ("board_t_ms", [sample.board_time_ms for sample in samples], "ms"),
            ("wearable_t_ms", [sample.wearable_time_ms for sample in samples], "ms"),
            ("board_sequence", [sample.board_sequence for sample in samples], "count"),
            ("wearable_sequence", [sample.wearable_sequence for sample in samples], "count"),
            ("skew_ms", [sample.skew_ms for sample in samples], "ms"),
            ("board_valid", [sample.board_valid for sample in samples], "boolean"),
            ("wearable_valid", [sample.wearable_valid for sample in samples], "boolean"),
            ("paired", [sample.paired for sample in samples], "boolean"),
            ("accepted_pair", [sample.accepted_pair for sample in samples], "boolean"),
        ):
            _dataset(data_group, key, values, units)

        for sensor, prefix, valid_attr in (
            ("board", "board", "board_valid"),
            ("wearable", "wt", "wearable_valid"),
        ):
            valid = [getattr(sample, valid_attr) for sample in samples]
            vectors = (
                ("acceleration_g", "a", "g"),
                ("angular_velocity_dps", "g", "dps"),
            )
            for name, vector_prefix, unit in vectors:
                values = [
                    [sample.values[f"{prefix}_{vector_prefix}{axis}_{unit}"] for axis in "xyz"]
                    if getattr(sample, valid_attr) else [math.nan, math.nan, math.nan]
                    for sample in samples
                ]
                _dataset(data_group, f"{sensor}_{name}", values, unit,
                         "Sensor-native XYZ axes; mounting alignment is not known.")
            orientation = [
                getattr(sample, f"{sensor}_orientation") or [math.nan, math.nan, math.nan]
                for sample in samples
            ]
            relative = [
                getattr(sample, f"{sensor}_relative") or [math.nan, math.nan, math.nan]
                for sample in samples
            ]
            _dataset(data_group, f"{sensor}_orientation_estimate_deg", orientation, "deg",
                     "Columns are roll, pitch, yaw; yaw is unreferenced gyro integration.")
            _dataset(data_group, f"{sensor}_orientation_relative_deg", relative, "deg",
                     "Estimated orientation relative to the first valid synchronized pair.")

        reported_angles = [
            [sample.values[key] for key in ("wt_roll_deg", "wt_pitch_deg", "wt_yaw_deg")]
            if sample.wearable_valid else [math.nan, math.nan, math.nan]
            for sample in samples
        ]
        _dataset(data_group, "wearable_reported_euler_deg", reported_angles, "deg",
                 "Device-reported roll, pitch, yaw, preserved from the input CSV.")

        bvh_group = file.create_group("bvh")
        _dataset(bvh_group, "frame_time_s", frame_times, "s",
                 "Uniform BVH frame times relative to the first valid synchronized pair.")
        _dataset(bvh_group, "frame_euler_deg", frame_rows, "deg",
                 "Per frame: board roll/pitch/yaw followed by wearable roll/pitch/yaw.")
        _dataset(bvh_group, "held_gap", held_gap, "boolean",
                 "True when the previous pair was held because the sample gap was too long.")
        bvh_group.attrs["requested_fps"] = fps
        bvh_group.attrs["actual_fps"] = 1.0 / frame_time
        bvh_group.attrs["frame_time_s"] = frame_time
        bvh_group.attrs["max_interpolation_gap_ms"] = max_interpolation_gap_ms
        bvh_group.attrs["joint_names"] = ",".join(bvh_gen.bvh_joint_names())
        bvh_group.attrs["channel_order"] = (
            "root Ground at foot level: Xposition Yposition Zposition Zrotation Xrotation "
            "Yrotation; Hips joint is offset 95 cm above ground; joints: Zrotation "
            "Xrotation Yrotation in hierarchy traversal order"
        )
        bvh_group.attrs["frame_channels_deg"] = (
            "Hips and lower body remain fixed; Spine Zrotation is fixed at -35 degrees; "
            "RightForearm Zrotation is wearable-minus-board relative pitch and Xrotation "
            "is relative roll; yaw is omitted because it is unreferenced and drifts; "
            "upper arms and left forearm stay in the static cycling pose"
        )
        bvh_group.attrs["offset_units"] = (
            "cm; human-only stick skeleton about 172 cm tall, Ground root at foot level "
            "for full-body framing in BVHView"
        )
        _dataset(bvh_group, "frame_channels", bvh_gen.bvh_channel_rows(frame_rows), "per-channel",
                 "Actual BVH motion channels: root translation and all joint rotations.")
        bvh_group["frame_channels"].attrs["channel_names"] = ",".join(
            bvh_gen.bvh_channel_names()
        )
        bvh_group["frame_channels"].attrs["channel_units"] = ",".join(
            ["cm"] * 3 + ["deg"] * (len(bvh_gen.bvh_channel_names()) - 3)
        )


def _atomic_write(path: Path, writer) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        writer(temporary)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def convert_csv_to_bvh(csv_path: Path, output_prefix: Path, fps: float = 10.0,
                       tau_seconds: float = 0.5,
                       max_interpolation_gap_ms: int = 250,
                       generate_hdf5: bool = True,
                       overwrite: bool = False) -> Tuple[Path, Optional[Path], int, int]:
    """Convert a merged IMU CSV to BVH and optionally HDF5."""
    if not math.isfinite(fps) or not 1.0 <= fps <= 200.0:
        raise ValueError("fps must be between 1 and 200")
    if not math.isfinite(tau_seconds) or tau_seconds <= 0:
        raise ValueError("complementary-filter tau must be positive")
    if max_interpolation_gap_ms <= 0:
        raise ValueError("maximum interpolation gap must be positive")

    columns, samples, raw_bytes = load_csv(csv_path)
    estimate_orientations(samples, tau_seconds)
    frame_times, frame_rows, held_gap, frame_time = resample_for_bvh(
        samples, fps, max_interpolation_gap_ms
    )
    
    bvh_path = output_prefix.with_suffix(".bvh")
    hdf5_path = output_prefix.with_suffix(".h5") if generate_hdf5 else None
    
    if not overwrite and bvh_path.exists():
        raise FileExistsError(f"Output BVH exists: {bvh_path}; choose --force to overwrite")
    if hdf5_path and not overwrite and hdf5_path.exists():
        raise FileExistsError(f"Output HDF5 exists: {hdf5_path}; choose --force to overwrite")

    print(f"Generating BVH: {bvh_path.name}")
    _atomic_write(bvh_path, lambda path: path.write_text(bvh_gen.bvh_text(frame_rows, frame_time), encoding="utf-8"))
    
    if generate_hdf5 and hdf5_path:
        print(f"Generating HDF5: {hdf5_path.name}")
        try:
            _atomic_write(hdf5_path, lambda path: write_hdf5(
                path, csv_path, columns, samples, raw_bytes,
                frame_times, frame_rows, held_gap, frame_time, fps,
                tau_seconds, max_interpolation_gap_ms,
            ))
        except Exception:
            if overwrite and bvh_path.exists():
                bvh_path.unlink()
            raise
    
    return bvh_path, hdf5_path, len(samples), sum(sample.accepted_pair for sample in samples)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("input_dir", type=Path, nargs="?", default=Path.cwd(),
                        help="Directory containing IMU CSV files to merge (default: current directory)")
    parser.add_argument("output_dir", type=Path, nargs="?", default=Path.cwd(),
                        help="Output directory for merged CSV and BVH/HDF5 files (default: input directory)")
    parser.add_argument("-o", "--output-prefix", type=Path,
                        help="Custom output file prefix; overrides output_dir")
    parser.add_argument("--fps", type=float, default=10.0,
                        help="BVH frame rate (default: 10)")
    parser.add_argument("--tau", type=float, default=0.5,
                        help="Complementary filter time constant in seconds (default: 0.5)")
    parser.add_argument("--max-interpolation-gap-ms", type=int, default=250,
                        help="Max gap before holding previous pose (default: 250)")
    parser.add_argument("--no-hdf5", action="store_true",
                        help="Skip HDF5 generation (only generate BVH)")
    parser.add_argument("--force", action="store_true",
                        help="Overwrite existing output files")
    
    args = parser.parse_args()
    
    input_dir = Path(args.input_dir).resolve()
    if not args.output_dir:
        output_dir = input_dir
    else:
        output_dir = Path(args.output_dir).resolve()
    
    if args.output_prefix:
        output_prefix = args.output_prefix.resolve()
    else:
        output_prefix = output_dir / "merged_imu"
    
    try:
        print(f"Merging CSV files from: {input_dir}")
        merged_csv = output_prefix.with_suffix(".csv")
        merged_csv = merge_csv_files(input_dir, merged_csv)
        
        print(f"\nConverting to BVH...")
        bvh_path, hdf5_path, rows, paired = convert_csv_to_bvh(
            merged_csv, output_prefix, args.fps, args.tau,
            args.max_interpolation_gap_ms, 
            not args.no_hdf5,
            args.force,
        )
        
        print(f"\n✓ Success!")
        print(f"  Input rows: {rows}; valid synchronized pairs: {paired}")
        print(f"  Merged CSV: {merged_csv}")
        print(f"  BVH output: {bvh_path}")
        if hdf5_path:
            print(f"  HDF5 output: {hdf5_path}")
            
    except (OSError, ValueError, csv.Error, RuntimeError) as error:
        print(f"✗ Error: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
