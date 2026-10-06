#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "fs3000.h"

// 视风向量：探针相对车头坐标系
//   vAlong > 0 表示迎面风；vCross > 0 表示风从右侧来
struct WindVector {
  float vAlong = 0;   // 沿车头方向视风分量 (m/s)
  float vCross = 0;   // 侧向视风分量 (m/s)
  float yawRad = 0;   // 偏航角 (rad)，正 = 右侧风
  float yawDeg = 0;   // 偏航角 (°)
  float vAir   = 0;   // 视风速度 (m/s)
};

// V 形双 FS3000 视风探针
// 两片传感器轴各与车头成 ±θ（默认 45°），同点安装。
// 每片读数为其轴向来流分量：
//   v1 = v_a·cos(θ − φ)，v2 = v_a·cos(θ + φ)
// 反解：
//   vAlong = (v1 + v2) / (2·cosθ)
//   vCross = (v1 − v2) / (2·sinθ)
//   φ = atan2(vCross, vAlong)，v_a = √(vAlong² + vCross²)
// 有效测角范围约 ±θ（超出后背风一片读数归零，差分失效）
class WindProbe {
public:
  void begin(TwoWire &w1, TwoWire &w2, float halfAngleDeg = 45.0f);
  bool read(WindVector &out);   // 读取一次视风向量
  bool leftOk() const  { return _leftOk; }
  bool rightOk() const { return _rightOk; }

private:
  FS3000 _left;    // 左臂（负角侧）
  FS3000 _right;   // 右臂（正角侧）
  bool   _leftOk = false;
  bool   _rightOk = false;
  float  _half = 0;   // 半夹角 rad
};
