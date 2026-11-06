"""Validate and compare native-axis QMI8658/WT901 comparison CSV files."""

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import mean

FIELDS = (
        "t_ms", "board_t_ms", "wt_t_ms", "board_seq", "wt_seq", "board_valid", "wt_valid", "paired", "skew_ms",
        *(f"{sensor}_{prefix}{axis}_{unit}" for sensor in ("board", "wt")
            for prefix, unit in (("a", "g"), ("g", "dps")) for axis in "xyz"),
        "wt_roll_deg", "wt_pitch_deg", "wt_yaw_deg",
        "board_a_norm_g", "wt_a_norm_g", "delta_a_norm_g",
        "board_g_norm_dps", "wt_g_norm_dps", "delta_g_norm_dps",
)


def summarize(path: Path) -> dict:
    differences = {"acceleration_norm_g": [], "angular_velocity_norm_dps": []}
    rows = paired = 0
    maximum_skew = 0
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        if tuple(reader.fieldnames or ()) != FIELDS:
            raise ValueError("unexpected IMU CSV header")
        for row in reader:
            rows += 1
            if None in row or None in row.values():
                raise ValueError(f"row {rows}: wrong column count")
            if any(row[key] not in {"0", "1"} for key in ("board_valid", "wt_valid", "paired")):
                raise ValueError(f"row {rows}: invalid validity flag")
            if row["paired"] == "0":
                continue
            if row["board_valid"] != "1" or row["wt_valid"] != "1":
                raise ValueError(f"row {rows}: invalid paired sensor")
            now = int(row["t_ms"])
            board_age = (now - int(row["board_t_ms"])) & 0xFFFFFFFF
            wt_age = (now - int(row["wt_t_ms"])) & 0xFFFFFFFF
            skew = int(row["skew_ms"])
            if max(board_age, wt_age) > 250 or abs(skew) > 100 or skew != wt_age - board_age:
                raise ValueError(f"row {rows}: stale or misaligned pair")
            maximum_skew = max(maximum_skew, abs(skew))
            for kind, prefix, suffix in (
                ("acceleration_norm_g", "a", "g"),
                ("angular_velocity_norm_dps", "g", "dps"),
            ):
                norms = []
                for sensor in ("board", "wt"):
                    values = [float(row[f"{sensor}_{prefix}{axis}_{suffix}"]) for axis in "xyz"]
                    if not all(math.isfinite(value) for value in values):
                        raise ValueError(f"row {rows}: nonfinite paired data")
                    norm = math.sqrt(sum(value * value for value in values))
                    stored = float(row[f"{sensor}_{prefix}_norm_{suffix}"])
                    if not math.isclose(stored, norm, abs_tol=0.00001):
                        raise ValueError(f"row {rows}: inconsistent norm")
                    norms.append(norm)
                difference = norms[1] - norms[0]
                stored = float(row[f"delta_{prefix}_norm_{suffix}"])
                if not math.isclose(stored, difference, abs_tol=0.00001):
                    raise ValueError(f"row {rows}: inconsistent difference")
                differences[kind].append(difference)
            paired += 1
    report = {"file": str(path), "rows": rows, "paired_rows": paired,
              "unpaired_rows": rows - paired, "max_skew_ms": maximum_skew,
              "comparison": "WT901 minus QMI8658 vector magnitudes; not axis calibration"}
    for kind, values in differences.items():
        report[kind] = None if not values else {
            "bias": mean(values), "mae": mean(abs(value) for value in values),
            "rmse": math.sqrt(mean(value * value for value in values)),
        }
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    arguments = parser.parse_args()
    try:
        print(json.dumps(summarize(arguments.csv), indent=2, allow_nan=False))
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"IMU comparison failed: {error}\n")


if __name__ == "__main__":
    main()