#!/usr/bin/env python3
"""Shared BVH generation logic for IMU-to-BVH conversion.

This module provides the core functions for generating BVH files from
IMU orientation data. It is used by both imu_to_bvh_hdf5.py and 
merge_imu_csv_to_bvh.py to ensure consistent BVH output.
"""

from __future__ import annotations

import math
from typing import List, Sequence, Tuple


TORSO_LEAN_DEG = -35.0
BVH_SKELETON = (
    ("LeftHip", (0, -2, -9), None, (
        ("LeftKnee", (0, -43, 0), None, (
            ("LeftAnkle", (0, -42, 0), None, (
                ("LeftFoot", (12, -8, 0), None, (
                    ("LeftToe", (16, 0, 0), None, ()),
                )),
            )),
        )),
    )),
    ("RightHip", (0, -2, 9), None, (
        ("RightKnee", (0, -43, 0), None, (
            ("RightAnkle", (0, -42, 0), None, (
                ("RightFoot", (12, -8, 0), None, (
                    ("RightToe", (16, 0, 0), None, ()),
                )),
            )),
        )),
    )),
    ("Spine", (0, 5, 0), "torso", (
        ("Chest", (0, 25, 0), None, (
            ("Neck", (0, 14, 0), None, (
                ("Head", (0, 15, 0), None, (
                    ("HeadTop", (0, 18, 0), None, ()),
                )),
            )),
            ("LeftUpperArm", (0, 0, -18), None, (
                ("LeftElbow", (22, -12, 0), None, (
                    ("LeftForearm", (20, -10, 0), None, (
                        ("LeftWrist", (15, -4, 0), None, (
                            ("LeftHand", (8, 0, 0), None, ()),
                        )),
                    )),
                )),
            )),
            ("RightUpperArm", (0, 0, 18), None, (
                ("RightElbow", (22, -12, 0), None, (
                    ("RightForearm", (20, -10, 0), "forearm", (
                        ("RightWrist", (15, -4, 0), None, (
                            ("RightHand", (8, 0, 0), None, ()),
                        )),
                    )),
                )),
            )),
        )),
    )),
)


def bvh_joint_text(joint, depth: int) -> Tuple[List[str], List[str]]:
    """Generate BVH JOINT text and channel names for a skeleton joint and its children."""
    name, offset, _, children = joint
    indent = "\t" * depth
    lines = [
        f"{indent}JOINT {name}",
        f"{indent}{{",
        f"{indent}\tOFFSET {offset[0]:.2f} {offset[1]:.2f} {offset[2]:.2f}",
        f"{indent}\tCHANNELS 3 Zrotation Xrotation Yrotation",
    ]
    channel_names = [f"{name}.Zrotation", f"{name}.Xrotation", f"{name}.Yrotation"]
    if children:
        for child in children:
            child_lines, child_channels = bvh_joint_text(child, depth + 1)
            lines.extend(child_lines)
            channel_names.extend(child_channels)
    else:
        lines.extend([
            f"{indent}\tEnd Site",
            f"{indent}\t{{",
            f"{indent}\t\tOFFSET 0.00 3.00 0.00",
            f"{indent}\t}}",
        ])
    lines.append(f"{indent}}}")
    return lines, channel_names


def bvh_channel_rows(frame_rows: Sequence[Sequence[float]]) -> List[Tuple[float, ...]]:
    """Convert frame orientation data to BVH channel values.
    
    Args:
        frame_rows: Each row contains [board_roll, board_pitch, board_yaw, 
                    arm_roll, arm_pitch, arm_yaw] in degrees.
    
    Returns:
        List of tuples where each tuple contains all channel values for that frame.
    """
    channel_rows = []
    for row in frame_rows:
        board_roll, board_pitch, board_yaw, arm_roll, arm_pitch, arm_yaw = row
        values = [0.0] * 9
        for _, _, binding, children in BVH_SKELETON:
            values.extend(_joint_channel_values(
                binding, children, board_roll, board_pitch,
                arm_roll, arm_pitch,
            ))
        channel_rows.append(tuple(values))
    return channel_rows


def _joint_channel_values(binding, children, board_roll, board_pitch,
                          arm_roll, arm_pitch):
    """Recursively compute channel values for a joint and its children."""
    if binding == "torso":
        values = [TORSO_LEAN_DEG, 0.0, 0.0]
    elif binding == "forearm":
        # BVH Z is the forward-lean plane; yaw is omitted because six-axis yaw drifts.
        values = [arm_pitch - board_pitch, arm_roll - board_roll, 0.0]
    else:
        values = [0.0, 0.0, 0.0]
    for _, _, child_binding, child_children in children:
        values.extend(_joint_channel_values(
            child_binding, child_children,
            board_roll, board_pitch, arm_roll, arm_pitch,
        ))
    return values


def bvh_joint_names() -> List[str]:
    """Get the ordered list of all joint names in the BVH skeleton."""
    names = ["Ground", "Hips"]

    def visit(joints):
        for name, _, _, children in joints:
            names.append(name)
            visit(children)

    visit(BVH_SKELETON)
    return names


def bvh_channel_names() -> List[str]:
    """Get the ordered list of all channel names in the BVH file."""
    names = [
        "Ground.Xposition", "Ground.Yposition", "Ground.Zposition",
        "Ground.Zrotation", "Ground.Xrotation", "Ground.Yrotation",
    ]
    _, hips_channels = bvh_joint_text(("Hips", (0, 95, 0), None, BVH_SKELETON), 1)
    names.extend(hips_channels)
    return names


def bvh_text(frame_rows: Sequence[Sequence[float]], frame_time: float) -> str:
    """Generate complete BVH file text.
    
    Args:
        frame_rows: Each row contains [board_roll, board_pitch, board_yaw,
                    arm_roll, arm_pitch, arm_yaw] in degrees.
        frame_time: Duration in seconds between frames.
    
    Returns:
        Complete BVH file content as a string.
    """
    lines = [
        "HIERARCHY",
        "ROOT Ground",
        "{",
        "  OFFSET 0.00 0.00 0.00",
        "  CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation",
    ]
    channel_names = [
        "Ground.Xposition", "Ground.Yposition", "Ground.Zposition",
        "Ground.Zrotation", "Ground.Xrotation", "Ground.Yrotation",
    ]
    hips_lines, hips_channels = bvh_joint_text(
        ("Hips", (0, 95, 0), None, BVH_SKELETON), 1
    )
    lines.extend(hips_lines)
    channel_names.extend(hips_channels)
    lines.extend(["}", "MOTION", f"Frames: {len(frame_rows)}",
                  f"Frame Time: {frame_time:.9f}"])
    channel_rows = bvh_channel_rows(frame_rows)
    for values in channel_rows:
        if len(values) != len(channel_names):
            raise ValueError("BVH hierarchy channel count does not match motion data")
        channels = values
        lines.append(" ".join(f"{value:.5f}" for value in channels))
    return "\n".join(lines) + "\n"
