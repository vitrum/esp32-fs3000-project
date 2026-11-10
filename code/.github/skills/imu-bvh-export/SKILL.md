---
name: imu-bvh-export
description: 'Use when generating, changing, or validating BVH from this project''s dual-IMU CSV logs. Covers human-only cycling poses, foot-level roots for full-body framing, IMU-to-limb mapping, BVH channel ordering, HDF5 metadata, and safe export validation.'
---

# Dual-IMU BVH Export

## Purpose

Create a readable human motion proxy from the project's synchronized onboard
and wearable IMU CSV logs. Preserve the source data in HDF5 and clearly
distinguish measured orientation from illustrative or unobservable anatomy.

## When to Use

- Generate BVH from `tmp/IMU/*.csv` or another project dual-IMU comparison log.
- Change skeleton proportions, root placement, motion mapping, or output scale.
- Diagnose a BVH that is off-screen, incomplete, distorted, or rejected by a
  viewer such as BVHView.

## Contract And Limitations

- Inspect the actual CSV header and parser before assuming field names,
  timestamp semantics, units, sensor validity, or angle order.
- The onboard IMU is the bicycle/frame reference; the optional wearable IMU is
  normally worn on the right forearm.
- A single forearm IMU cannot independently determine shoulder orientation,
  upper-arm motion, or elbow flexion. Do not invent these as measured motion.
- Six-axis yaw has no absolute heading reference and drifts. Do not use it to
  animate anatomy unless the task explicitly accepts that limitation and the
  implementation documents it.
- If the log lacks the upright/forward-lean calibration samples or an explicit
  sensor-to-segment calibration, do not claim its first-pair-relative
  orientation is an absolute anatomical angle. A fixed torso lean may be used
  as a documented visual pose, not presented as a measured result.
- State the wearable-to-bicycle relative-axis mapping and its calibration
  assumptions. Sensor roll/pitch are not automatically anatomical joint angles.

## BVH Skeleton Rules

1. Use a human-only hierarchy: feet/legs, pelvis, spine/chest, neck/head,
   shoulders, upper arms, elbows, forearms, wrists, and hands. Do not add a bike
   frame, wheels, handlebar, or other props when the requested output is a
   human body.
2. Put the BVH `ROOT` at ground/foot level, not at the hips. Use a neutral root
   name such as `Ground`, with root offset `0 0 0`; make the pelvis/hips an
   ordinary joint approximately 90-100 cm above it. This keeps the origin at
   the feet and helps viewers frame the complete body.
3. Keep the lower body static when requested. Use zero root translation and
   zero pelvis/leg rotations unless the requested motion requires otherwise.
4. Give the body plausible centimeter-scale proportions, with total height
   around 165-180 cm. BVH has no universal unit declaration; viewers may infer
   centimeters from the skeleton extent. Do not create a tiny upper-body-only
   skeleton and expect its viewer scaling to match a full person.
5. BVH displays joint chains rather than volumetric anatomy. Ensure offsets
   connect recognizable limbs and that End Sites extend terminal joints
   sensibly. Keep the head, hands, feet, and all limbs within the full-body
   bounds.
6. If a posed cycling torso is desired, make the forward lean explicit and
   maintain a coherent head/arm pose. A fixed lean is an illustrative baseline,
   not an inferred angle unless supported by calibration data.

## IMU Motion Mapping

- Animate the trunk only from a documented trunk measurement or an explicitly
  requested illustrative pose.
- For the common single right-forearm wearable configuration, animate the right
  forearm proxy using the wearable orientation relative to the onboard frame.
  Keep the arm/elbow/left side static unless the data can actually resolve
  those joints.
- Confirm axis signs and Euler order against the exporter and the target viewer.
  BVH motion values must follow channel declaration order exactly.
- Use a stable orientation estimator and interpolation consistent with source
  timestamps. Reject invalid pairs; hold or mark gaps according to the existing
  converter policy rather than silently fabricating motion.
- Avoid Euler-angle discontinuities where practical. Preserve the original
  sample angles and validity separately from derived/resampled BVH channels.

## Export Workflow

1. Read the converter, nearest tests, project instructions, and the CSV header.
2. Establish the data contract: sample rate, timestamp wrap, synchronized-pair
   criteria, angle order, coordinate assumptions, and expected frame count.
3. Review at least representative input samples and angle ranges. Check whether
   calibration poses are present before describing values as absolute.
4. Make a minimal change to the existing converter and add regression
   assertions for:
   - a foot-level root and a hips joint above the origin;
   - a complete human skeleton and absence of non-human props;
   - static lower-body channels when required;
   - explicit measured-versus-static upper-body and arm mappings;
   - channel declaration/value count and HDF5 channel metadata;
   - bounded body height and finite motion values.
5. Run the focused converter tests. From the project `code/` directory, the
   repository's established command is:

   ```sh
   PYTHONPATH=.:/tmp/imu-converter-deps python3 tools/test_imu_to_bvh_hdf5.py
   ```

   If dependencies are missing, install only from
   `tools/requirements-imu-convert.txt` in an isolated environment; do not
   install global packages.
6. Regenerate with the existing CLI. Use `--force` only when the user requested
   replacement or explicitly approved overwriting the existing BVH/HDF5 files:

   ```sh
   PYTHONPATH=/tmp/imu-converter-deps python3 tools/imu_to_bvh_hdf5.py \
     tmp/IMU/I3E5A1F74-0.csv --force
   ```

7. Independently validate the artifacts, not just the CLI exit code:
   - declared frame count equals actual motion row count;
   - every row has exactly the root-plus-joint channel count and finite values;
   - root offset is at ground level and body bounds cover feet to head;
   - expected joints are present and forbidden props absent;
   - HDF5 frame-channel shape and names match BVH;
   - HDF5 source bytes and source-row/sample counts are preserved.
8. Run `python3 -m py_compile` on modified Python files and `git diff --check`.
   Update `test/README.md` when the export contract or usage changes.
9. If a visual viewer is available, open the generated BVH and confirm the
   whole body is visible and correctly oriented. Report when visual validation
   was not performed; structural checks are not a substitute for a screenshot.

## Repository Implementation

The current exporter is [tools/imu_to_bvh_hdf5.py](../../../tools/imu_to_bvh_hdf5.py);
its focused tests are [tools/test_imu_to_bvh_hdf5.py](../../../tools/test_imu_to_bvh_hdf5.py).
The current intended display is a foot-rooted human-only cycling proxy:
static lower body, illustrative forward-leaning torso, and wearable-driven
right forearm. Keep the HDF5 `/bvh/frame_channels` dataset synchronized with the
serialized BVH channel order.
