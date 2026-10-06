#include "wind_probe.h"
#include "config.h"

void WindProbe::begin(TwoWire &w1, TwoWire &w2, float halfAngleDeg) {
  _half = halfAngleDeg * (PI / 180.0f);
  _leftOk  = _left.begin(w1, FS3000_ADDR);
  _rightOk = _right.begin(w2, FS3000_ADDR);
}

bool WindProbe::read(WindVector &out) {
  if (!_leftOk || !_rightOk) return false;

  float v1, v2;
  if (!_left.readMps(v1) || !_right.readMps(v2)) return false;

  const float c2 = 2.0f * cosf(_half);   // θ=45° 时 = √2
  const float s2 = 2.0f * sinf(_half);

  out.vAlong = (v1 + v2) / c2;
  out.vCross = (v1 - v2) / s2;
  out.yawRad = atan2f(out.vCross, out.vAlong);
  out.yawDeg = out.yawRad * (180.0f / PI);
  out.vAir   = sqrtf(out.vAlong * out.vAlong + out.vCross * out.vCross);
  return true;
}
