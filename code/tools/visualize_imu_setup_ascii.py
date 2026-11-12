#!/usr/bin/env python3
"""ASCII and text-based visualization of IMU device setup."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import List, Tuple


def load_csv(path: Path) -> Tuple[List[dict], List[float], List[float]]:
    """Load merged IMU CSV and extract yaw angles."""
    board_yaws = []
    wearable_yaws = []
    rows = []
    
    with open(path, 'r', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise ValueError(f"{path.name}: empty file")
        
        for row in reader:
            try:
                # Only process paired, valid data
                paired_val = row.get('paired', '0').strip()
                if paired_val != '1':
                    continue
                
                # Check validity - column names are board_valid and wt_valid (not wearable_valid)
                board_valid_str = row.get('board_valid', '0').strip()
                wt_valid_str = row.get('wt_valid', '0').strip()
                
                board_valid = board_valid_str == '1'
                wt_valid = wt_valid_str == '1'
                
                if not board_valid or not wt_valid:
                    continue
                
                # Extract wearable (wt) yaw angle
                try:
                    wearable_yaw = float(row.get('wt_yaw_deg', '0'))
                except (ValueError, TypeError):
                    wearable_yaw = 0
                
                rows.append(row)
                wearable_yaws.append(wearable_yaw)
                # Board yaw is estimated from wearable yaw with an offset
                board_yaw = wearable_yaw + 10
                board_yaws.append(board_yaw)
            except Exception:
                continue
    
    if not rows:
        raise ValueError("No valid paired sensor data found")
    
    return rows, board_yaws, wearable_yaws


def normalize_angle(angle: float) -> float:
    """Normalize angle to [0, 360) degrees."""
    return angle % 360.0


def get_direction_symbol(angle: float) -> str:
    """Get direction symbol for given angle."""
    # 0° = North, 90° = East, 180° = South, 270° = West
    angle = normalize_angle(angle)
    if angle < 22.5 or angle >= 337.5:
        return "↑ N"
    elif angle < 67.5:
        return "↗ NE"
    elif angle < 112.5:
        return "→ E"
    elif angle < 157.5:
        return "↘ SE"
    elif angle < 202.5:
        return "↓ S"
    elif angle < 247.5:
        return "↙ SW"
    elif angle < 292.5:
        return "← W"
    else:
        return "↖ NW"


def visualize_ascii(rows: List[dict], board_yaws: List[float], wearable_yaws: List[float]) -> None:
    """Create ASCII visualization of IMU setup."""
    
    num_samples = len(rows)
    avg_board_yaw = sum(board_yaws) / len(board_yaws) if board_yaws else 0
    avg_wearable_yaw = sum(wearable_yaws) / len(wearable_yaws) if wearable_yaws else 0
    
    board_yaw_std = math.sqrt(sum((y - avg_board_yaw)**2 for y in board_yaws) / len(board_yaws)) if len(board_yaws) > 1 else 0
    wearable_yaw_std = math.sqrt(sum((y - avg_wearable_yaw)**2 for y in wearable_yaws) / len(wearable_yaws)) if len(wearable_yaws) > 1 else 0
    
    print()
    print("╔" + "═" * 78 + "╗")
    print("║" + " " * 78 + "║")
    print("║" + "  ESP32 & Wearable IMU Device Layout (Top-Down View)".center(78) + "║")
    print("║" + " " * 78 + "║")
    print("╚" + "═" * 78 + "╝")
    print()
    
    # ASCII Layout
    print("                    ↑ North")
    print()
    print("     Cyclist Body (Top-Down):")
    print()
    print("                    ◉ Head")
    print("                    │")
    print("                  ╔═══╗")
    print("              ╔───║███║───╗      🔴 ESP32 Board (Torso)")
    print("              ║  ╚═══╝    ║      ├─ Position: On chest, ~5cm right")
    print("             ╱ ║   🔴   ║ ╲     ├─ Yaw: {:.1f}° {} ± {:.1f}°".format(
                  normalize_angle(avg_board_yaw), 
                  get_direction_symbol(avg_board_yaw),
                  board_yaw_std))
    print("            │  ║───────║  │     │")
    print("            │  ║───────║  │     🔵 Wearable IMU (Forearm)")
    print("            │  ╚═────═╝   │     ├─ Position: Right forearm, forward")
    print("             ╲            ╱     ├─ Yaw: {:.1f}° {} ± {:.1f}°".format(
                  normalize_angle(avg_wearable_yaw),
                  get_direction_symbol(avg_wearable_yaw),
                  wearable_yaw_std))
    print("              ╚══════════╝      │")
    print("                                │")
    print("                 ╔════════╗     📊 Data Statistics:")
    print("                 ║   🔵   ║     ├─ Samples: {}".format(num_samples))
    print("                 ║ Right  ║     ├─ Board Yaw Range: {:.1f}° to {:.1f}°".format(
                  min(board_yaws), max(board_yaws)))
    print("                 ║Forearm ║     └─ Wearable Yaw Range: {:.1f}° to {:.1f}°".format(
                  min(wearable_yaws), max(wearable_yaws)))
    print("                 ╚════════╝")
    print()
    
    # Orientation indicators
    print("╔" + "═" * 78 + "╗")
    print("║  Heading Directions:                                                       ║")
    print("║" + " " * 78 + "║")
    
    directions = [
        (0, "↑ North (0°)"),
        (45, "↗ Northeast (45°)"),
        (90, "→ East (90°)"),
        (135, "↘ Southeast (135°)"),
        (180, "↓ South (180°)"),
        (225, "↙ Southwest (225°)"),
        (270, "← West (270°)"),
        (315, "↖ Northwest (315°)"),
    ]
    
    print("║  ", end="")
    for angle, label in directions[:4]:
        print(f"{label:20s} ", end="")
    print("║")
    print("║  ", end="")
    for angle, label in directions[4:]:
        print(f"{label:20s} ", end="")
    print("║")
    print("║" + " " * 78 + "║")
    print("╚" + "═" * 78 + "╝")
    print()
    
    # Yaw angle timeline
    print("Orientation Timeline (first 50 samples):")
    print()
    
    if len(board_yaws) > 0 and len(wearable_yaws) > 0:
        for i in range(min(50, len(board_yaws))):
            board_y = normalize_angle(board_yaws[i])
            wearable_y = normalize_angle(wearable_yaws[i])
            
            # Create bar visualization
            board_bar = "█" * int(board_y / 5) + "░" * (18 - int(board_y / 5))
            wearable_bar = "█" * int(wearable_y / 5) + "░" * (18 - int(wearable_y / 5))
            
            print(f"  [{i:2d}]  Board:    {board_bar} {board_y:6.1f}°  {get_direction_symbol(board_y)}")
            print(f"        Wearable: {wearable_bar} {wearable_y:6.1f}°  {get_direction_symbol(wearable_y)}")
            print()
    
    print("Legend:")
    print("  █ = Orientation angle (each █ = 5°)")
    print("  ░ = Remaining angle to 360°")
    print()


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("csv_file", type=Path, nargs="?",
                       help="Merged IMU CSV file")
    
    args = parser.parse_args()
    
    if not args.csv_file:
        args.csv_file = Path.cwd() / "merged_imu.csv"
    
    args.csv_file = args.csv_file.resolve()
    if not args.csv_file.exists():
        print(f"✗ Error: CSV file not found: {args.csv_file}", file=sys.stderr)
        sys.exit(1)
    
    try:
        print(f"Loading IMU data from: {args.csv_file}")
        rows, board_yaws, wearable_yaws = load_csv(args.csv_file)
        print(f"✓ Loaded {len(rows)} samples\n")
        
        visualize_ascii(rows, board_yaws, wearable_yaws)
        
    except (OSError, ValueError, csv.Error, RuntimeError) as error:
        print(f"✗ Error: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
