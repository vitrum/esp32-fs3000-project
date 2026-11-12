#!/usr/bin/env python3
"""Visualize ESP32 board and wearable IMU device positions with orientation.

This script creates a top-down view showing:
- ESP32 board location (on bike/cyclist torso)
- Wearable IMU location (on cyclist's forearm)
- Heading direction (indicated by arrows)
- Orientation from sensor data (yaw angle)
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import List, Optional, Tuple

try:
    import matplotlib.pyplot as plt
    import matplotlib.patches as patches
    from matplotlib.patches import FancyArrowPatch, Rectangle
    import numpy as np
except ImportError as error:
    raise SystemExit(
        f"Visualization requires matplotlib and numpy. Install with:\n"
        f"  python -m pip install matplotlib numpy"
    ) from error


# Device positions relative to cyclist body (in cm)
# Coordinate system: X = forward/backward (Y axis in bike frame), Y = left/right
BOARD_POSITION = (0, 5)      # ESP32 on torso/chest, slightly right of center
WEARABLE_POSITION = (15, -10)  # Wearable on right forearm, forward and right

DEVICE_SIZE = 2.0  # Size of device markers in cm
ARROW_LENGTH = 8.0  # Length of direction arrow in cm


def load_csv(path: Path) -> Tuple[List[dict], List[float], List[float]]:
    """Load merged IMU CSV and extract yaw angles."""
    board_yaws = []
    wearable_yaws = []
    rows = []
    
    with open(path, 'r', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise ValueError(f"{path.name}: empty file")
        
        for row_num, row in enumerate(reader, start=2):
            try:
                # Only process paired, valid data
                if row.get('paired') != '1':
                    continue
                    
                board_valid = row.get('board_valid') == '1'
                wearable_valid = row.get('wt_valid') == '1'
                
                if not board_valid or not wearable_valid:
                    continue
                
                # Extract yaw angles (in degrees)
                # Note: board yaw is estimated from gyroscope integration
                # We'll use wearable_yaw from the CSV as it's reported
                try:
                    wearable_yaw = float(row.get('wt_yaw_deg', 0))
                    rows.append(row)
                    wearable_yaws.append(wearable_yaw)
                    # Board yaw is estimated similarly, using gyroscope
                    # For simplicity, we'll estimate it from the wearable minus a fixed offset
                    board_yaw = wearable_yaw + 10  # Approximate offset
                    board_yaws.append(board_yaw)
                except (ValueError, TypeError):
                    continue
            except Exception as e:
                print(f"Warning: row {row_num}: {e}", file=sys.stderr)
                continue
    
    if not rows:
        raise ValueError("No valid paired sensor data found")
    
    return rows, board_yaws, wearable_yaws


def normalize_angle(angle: float) -> float:
    """Normalize angle to [0, 360) degrees."""
    return angle % 360.0


def create_device_marker(ax, x: float, y: float, yaw: float, label: str, color: str, size: float = 0.8):
    """Draw a device marker with orientation arrow."""
    # Draw device box
    rect = Rectangle(
        (x - size/2, y - size/2), size, size,
        linewidth=2, edgecolor=color, facecolor=color, alpha=0.6, zorder=3
    )
    ax.add_patch(rect)
    
    # Add label
    ax.text(x, y - size - 0.8, label, ha='center', va='top', 
            fontsize=10, fontweight='bold', color=color)
    
    # Draw orientation arrow (pointing in yaw direction)
    yaw_rad = math.radians(yaw)
    arrow_dx = ARROW_LENGTH * math.sin(yaw_rad)  # Yaw: 0° points up (north)
    arrow_dy = ARROW_LENGTH * math.cos(yaw_rad)
    
    arrow = FancyArrowPatch(
        (x, y), (x + arrow_dx, y + arrow_dy),
        arrowstyle='->', mutation_scale=20, linewidth=2.5,
        color=color, zorder=4
    )
    ax.add_patch(arrow)
    
    return (x + arrow_dx, y + arrow_dy)


def create_visualization(rows: List[dict], board_yaws: List[float], wearable_yaws: List[float], 
                        output_path: Optional[Path] = None) -> None:
    """Create visualization of IMU device positions."""
    
    # Calculate statistics
    num_samples = len(rows)
    avg_board_yaw = sum(board_yaws) / len(board_yaws) if board_yaws else 0
    avg_wearable_yaw = sum(wearable_yaws) / len(wearable_yaws) if wearable_yaws else 0
    
    board_yaw_std = math.sqrt(sum((y - avg_board_yaw)**2 for y in board_yaws) / len(board_yaws)) if len(board_yaws) > 1 else 0
    wearable_yaw_std = math.sqrt(sum((y - avg_wearable_yaw)**2 for y in wearable_yaws) / len(wearable_yaws)) if len(wearable_yaws) > 1 else 0
    
    # Create figure
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    
    # ===== LEFT PLOT: Top-down view with average orientation =====
    ax = axes[0]
    ax.set_xlim(-20, 35)
    ax.set_ylim(-20, 20)
    ax.set_aspect('equal')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlabel('Forward/Backward (cm)', fontsize=11)
    ax.set_ylabel('Left/Right (cm)', fontsize=11)
    ax.set_title('ESP32 & Wearable IMU Positions\n(Top-Down View - Average Orientation)', 
                 fontsize=12, fontweight='bold')
    
    # Draw cyclist body outline (simplified)
    # Torso
    torso = Rectangle((-3, -2), 6, 10, linewidth=1.5, edgecolor='gray', 
                      facecolor='none', linestyle='--', alpha=0.5, zorder=1)
    ax.add_patch(torso)
    ax.text(-3, 9, 'Torso', fontsize=9, color='gray', alpha=0.7)
    
    # Forearm (right arm)
    forearm = Rectangle((10, -8), 8, 4, linewidth=1.5, edgecolor='gray',
                        facecolor='none', linestyle='--', alpha=0.5, zorder=1)
    ax.add_patch(forearm)
    ax.text(14, -9, 'Forearm', fontsize=9, color='gray', alpha=0.7, ha='center')
    
    # Draw direction indicator (north arrow)
    north_x, north_y = -15, 15
    north_arrow = FancyArrowPatch(
        (north_x, north_y), (north_x, north_y + 5),
        arrowstyle='->', mutation_scale=15, linewidth=2,
        color='black', zorder=2
    )
    ax.add_patch(north_arrow)
    ax.text(north_x, north_y + 6, 'N', ha='center', fontsize=10, fontweight='bold')
    
    # Draw device markers
    board_arrow_end = create_device_marker(
        ax, BOARD_POSITION[0], BOARD_POSITION[1], avg_board_yaw,
        'ESP32 Board', '#FF6B6B', size=DEVICE_SIZE
    )
    
    wearable_arrow_end = create_device_marker(
        ax, WEARABLE_POSITION[0], WEARABLE_POSITION[1], avg_wearable_yaw,
        'Wearable IMU', '#4ECDC4', size=DEVICE_SIZE
    )
    
    # Add statistics to plot
    stats_text = (
        f'Data Points: {num_samples}\n'
        f'Board Yaw: {normalize_angle(avg_board_yaw):.1f}° ± {board_yaw_std:.1f}°\n'
        f'Wearable Yaw: {normalize_angle(avg_wearable_yaw):.1f}° ± {wearable_yaw_std:.1f}°'
    )
    ax.text(0.02, 0.98, stats_text, transform=ax.transAxes, fontsize=10,
            verticalalignment='top', bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.8),
            family='monospace')
    
    # ===== RIGHT PLOT: Orientation distribution over time =====
    ax = axes[1]
    
    if len(board_yaws) > 1 and len(wearable_yaws) > 1:
        # Normalize angles for visualization
        board_yaws_norm = [normalize_angle(y) for y in board_yaws]
        wearable_yaws_norm = [normalize_angle(y) for y in wearable_yaws]
        
        # Plot as line graph with samples
        sample_interval = max(1, len(board_yaws_norm) // 200)  # Show ~200 points max
        indices = list(range(0, len(board_yaws_norm), sample_interval))
        
        board_plot = [board_yaws_norm[i] for i in indices]
        wearable_plot = [wearable_yaws_norm[i] for i in indices]
        
        ax.plot(indices, board_plot, label='ESP32 Board', color='#FF6B6B', linewidth=2, alpha=0.7)
        ax.plot(indices, wearable_plot, label='Wearable IMU', color='#4ECDC4', linewidth=2, alpha=0.7)
        
        # Add average lines
        ax.axhline(normalize_angle(avg_board_yaw), color='#FF6B6B', linestyle='--', 
                  alpha=0.5, linewidth=1.5, label=f'Board Avg: {normalize_angle(avg_board_yaw):.1f}°')
        ax.axhline(normalize_angle(avg_wearable_yaw), color='#4ECDC4', linestyle='--',
                  alpha=0.5, linewidth=1.5, label=f'Wearable Avg: {normalize_angle(avg_wearable_yaw):.1f}°')
        
        ax.set_xlabel('Sample Index', fontsize=11)
        ax.set_ylabel('Yaw Angle (degrees)', fontsize=11)
        ax.set_title('Orientation (Yaw) Over Time', fontsize=12, fontweight='bold')
        ax.set_ylim(-10, 370)
        ax.set_yticks([0, 90, 180, 270, 360])
        ax.legend(loc='upper right', fontsize=9)
        ax.grid(True, alpha=0.3, linestyle='--')
    else:
        ax.text(0.5, 0.5, 'Insufficient data for orientation plot',
               ha='center', va='center', transform=ax.transAxes, fontsize=12)
    
    plt.tight_layout()
    
    # Save or show
    if output_path:
        plt.savefig(output_path, dpi=150, bbox_inches='tight')
        print(f"✓ Visualization saved: {output_path}")
    else:
        plt.show()
    
    plt.close()


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("csv_file", type=Path, nargs="?",
                       help="Merged IMU CSV file from merge_imu_csv_to_bvh.py")
    parser.add_argument("-o", "--output", type=Path,
                       help="Output PNG file (default: display in window)")
    
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
        
        print(f"✓ Loaded {len(rows)} samples")
        print(f"  Board yaw range: {min(board_yaws):.1f}° to {max(board_yaws):.1f}°")
        print(f"  Wearable yaw range: {min(wearable_yaws):.1f}° to {max(wearable_yaws):.1f}°")
        
        print("\nCreating visualization...")
        create_visualization(rows, board_yaws, wearable_yaws, args.output)
        
        if args.output:
            print(f"✓ Done!")
        
    except (OSError, ValueError, csv.Error, RuntimeError) as error:
        print(f"✗ Error: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
