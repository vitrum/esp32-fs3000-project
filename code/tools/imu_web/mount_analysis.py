"""Rigid, gravity-preserving mounting candidates, not inertial positioning."""

import itertools
import math

import numpy as np


def vector(config, name):
    result = np.asarray(config.get(name), dtype=float)
    if result.shape != (3,) or not np.isfinite(result).all() or np.abs(result).max() > 500:
        raise ValueError(f"{name} must contain three finite coordinates within +/-500 cm")
    return result


def axis_candidates():
    for permutation in itertools.permutations(range(3)):
        for signs in itertools.product((-1, 1), repeat=3):
            matrix = np.eye(3)[:, permutation] * signs
            if np.linalg.det(matrix) > 0:
                yield matrix, [("+" if sign > 0 else "-") + "XYZ"[axis]
                               for axis, sign in zip(permutation, signs)]


def ratios(board, arm, shoulder, grip, length, upper_arm):
    directions = np.einsum("nji,nj->ni", board, arm[:, :, 0])
    elbows = grip - length * directions
    return np.linalg.norm(elbows - shoulder, axis=1) / upper_arm


def statistics(values):
    outside = (values < 0.7) | (values > 1.15)
    return {"samples": len(values), "outliers": int(outside.sum()),
            "outlier_fraction": float(outside.mean()),
            "ratio_p05": float(np.quantile(values, 0.05)),
            "ratio_p95": float(np.quantile(values, 0.95))}


def yaw_rotation(angle):
    c, s = math.cos(angle), math.sin(angle)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def diagnose_mount(data, config):
    if config.get("mode") != "hood":
        raise ValueError("Mount diagnosis requires the supported right-hood model; free-arm motion is not a fit constraint")
    shoulder, grip = vector(config, "shoulder"), vector(config, "grip")
    desired = vector(config, "neutral_direction")
    if np.linalg.norm(desired[:2]) < 0.1:
        raise ValueError("The neutral forearm must have a usable horizontal direction")
    upper_arm = config.get("upper_arm")
    length = config.get("forearm_and_palm")
    if not isinstance(upper_arm, (int, float)) or not math.isfinite(upper_arm) or not 10 <= upper_arm <= 80:
        raise ValueError("upper_arm must be between 10 and 80 cm")
    if not isinstance(length, (int, float)) or not math.isfinite(length) or not 10 <= length <= 75:
        raise ValueError("forearm_and_palm must be between 10 and 75 cm")
    baseline_mount = np.asarray(config.get("display_mount", np.eye(3)), dtype=float)
    if (baseline_mount.shape != (3, 3) or not np.isfinite(baseline_mount).all()
            or not np.allclose(baseline_mount.T @ baseline_mount, np.eye(3), atol=1e-6)
            or not np.isclose(np.linalg.det(baseline_mount), 1, atol=1e-6)):
        raise ValueError("display_mount must be a proper rotation")
    frames = []
    seen = set()
    for frame in data["frames"]:
        key = (frame["wt_seq"], frame["wt_time_ms"])
        if frame["paired"] and key not in seen:
            seen.add(key)
            frames.append(frame)
    if len(frames) < 5:
        raise ValueError("At least five independent synchronized wearable samples are required")
    board = np.asarray([frame["board"] for frame in frames])
    wearable = np.asarray([frame["wt"] for frame in frames])
    sensor_reference = np.asarray(data["summary"]["reference_wt_sensor_matrix"])
    sensor = wearable @ sensor_reference
    reference_time = config.get("reference_time_s")
    if reference_time is not None:
        if (not isinstance(reference_time, (float, int)) or not math.isfinite(reference_time)
                or not 0 <= reference_time <= data["summary"]["duration_s"]):
            raise ValueError("reference_time_s must be inside the IMU recording")
        low, high = max(0, reference_time * 1000 - 1500), reference_time * 1000 + 1500
    else:
        low, high = 0, 30000
    quiet = []
    for index, frame in enumerate(frames):
        values = frame["values"]
        if not low <= frame["time_ms"] <= high:
            continue
        good = True
        for name in ("board", "wt"):
            acceleration = math.hypot(*(values[f"{name}_a{axis}_g"] for axis in "xyz"))
            gyro = math.hypot(*(values[f"{name}_g{axis}_dps"] for axis in "xyz"))
            good &= 0.8 <= acceleration <= 1.2 and gyro <= 15
        if good:
            quiet.append(index)
    if len(quiet) < 5:
        raise ValueError("No sufficiently quiet reference window; select a stationary right-hood segment (at least five samples)")
    reference = quiet[len(quiet) // 2]
    # Oversample baseline outliers as well as the whole timeline; never delete them.
    baseline_ratios = ratios(board, wearable @ baseline_mount, shoulder, grip, length, upper_arm)
    outside = np.flatnonzero((baseline_ratios < 0.7) | (baseline_ratios > 1.15))
    uniform = np.linspace(0, len(frames) - 1, min(800, len(frames)), dtype=int)
    abnormal = outside[np.linspace(0, len(outside) - 1, min(400, len(outside)), dtype=int)] if len(outside) else []
    selection = np.unique(np.concatenate((uniform, abnormal, quiet)).astype(int))
    target = desired / np.linalg.norm(desired)
    reference_sensor = board[reference].T @ sensor[reference]
    candidates = []
    for native_from_arm, mapping in axis_candidates():
        first = reference_sensor @ native_from_arm
        if np.linalg.norm(first[:2, 0]) < 0.1:
            continue
        yaw = math.atan2(target[1], target[0]) - math.atan2(first[1, 0], first[0, 0])
        alignment = yaw_rotation(yaw)
        # Keep the reference correction on the right so common rigid motion cancels.
        playback_from_arm = wearable[reference].T @ board[reference] @ alignment @ first
        arm = wearable[selection] @ playback_from_arm
        candidate_ratios = ratios(board[selection], arm, shoulder, grip, length, upper_arm)
        rest = board[reference].T @ wearable[reference] @ playback_from_arm
        reference_direction_error = math.atan2(float(np.linalg.norm(np.cross(rest[:, 0], target))),
                                              float(np.dot(rest[:, 0], target)))
        dorsal_penalty = 1 - float(np.dot(rest[:, 2], [0, -1, 0]))
        reference_ratio = ratios(board[reference:reference + 1],
                                 (wearable[reference] @ playback_from_arm)[None],
                                 shoulder, grip, length, upper_arm)[0]
        loss = float(np.mean(np.minimum(np.abs(np.log(np.maximum(candidate_ratios, 0.01))), 2)))
        result = statistics(candidate_ratios)
        candidates.append({
            "mapping": mapping, "native_from_arm": native_from_arm.tolist(),
            "alignment": alignment.tolist(), "sensor_reference": sensor_reference.tolist(),
            "alignment_frame": "bike_at_reference",
            "playback_from_arm": playback_from_arm.tolist(),
            "reference_direction_error_deg": math.degrees(reference_direction_error),
            "score": loss + 2 * result["outlier_fraction"] + 0.5 * dorsal_penalty
                     + abs(math.log(max(reference_ratio, 0.01))) + 2 * reference_direction_error,
            "sampled_statistics": result,
        })
    if not candidates:
        raise ValueError("No gravity-compatible mounting candidates")
    candidates.sort(key=lambda item: item["score"])
    best = candidates[0]
    corrected = wearable @ np.asarray(best["playback_from_arm"])
    best_ratios = ratios(board, corrected, shoulder, grip, length, upper_arm)
    examples = outside[np.linspace(0, len(outside) - 1, min(40, len(outside)), dtype=int)] if len(outside) else []
    return {
        "model_reference": {"mode": "hood", "shoulder": shoulder.tolist(), "grip": grip.tolist(),
                            "neutral_direction": desired.tolist(), "upper_arm": upper_arm,
                            "forearm_and_palm": length, "display_mount": baseline_mount.tolist(),
                            "requested_reference_time_s": reference_time,
                            "bike_ground_constraint": "Rigid computer/frame; flat reference ground, heading only",
                            "arm_reference": "Full board.T @ wearable @ fixed playback_from_arm"},
        "reference_time_ms": frames[reference]["time_ms"], "quiet_samples": len(quiet),
        "sampled_count": len(selection), "baseline": statistics(baseline_ratios),
        "recommended_validation": statistics(best_ratios), "candidates": candidates[:6],
        "outlier_examples": [{"time_ms": frames[index]["time_ms"],
                             "original_ratio": float(baseline_ratios[index]),
                             "recommended_ratio": float(best_ratios[index]),
                             "measurements": frames[index]["values"]} for index in examples],
        "wearing": "WT9011DCL-BT50; official strap, distal right forearm; logo on dorsal side, USB toward elbow",
        "confidence": "Model-constrained candidates, not manufacturer-axis certification; confirmation required",
        "limitations": [
            "Selected quiet samples must actually be supported on the right hood; quiet does not prove contact.",
            "Upper-arm length/shoulder position are estimates; warnings alone do not prove sensor axis errors.",
            "No samples were deleted and no motion angles or forearm lengths were scaled.",
            "Mounting is fixed in playback coordinates; full board-relative motion cancels common rotation. Gyro drift remains.",
            "The supplied housing orientation constrains a dorsal reference, not a verified native XYZ diagram.",
        ],
    }
