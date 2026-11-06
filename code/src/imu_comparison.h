#pragma once

#include "imu_sample.h"
#include <cmath>
#include <cstdio>
#include <stddef.h>

struct ImuComparisonRecord {
  uint32_t milliseconds = 0;
  ImuSample board;
  ImuSample wt;
  float wtAngles[3] = {};
  int32_t skewMs = 0;
  bool paired = false;
};

class ImuComparison {
public:
  static const char *header() {
    return "t_ms,board_t_ms,wt_t_ms,board_seq,wt_seq,board_valid,wt_valid,paired,skew_ms,"
        "board_ax_g,board_ay_g,board_az_g,board_gx_dps,board_gy_dps,board_gz_dps,"
        "wt_ax_g,wt_ay_g,wt_az_g,wt_gx_dps,wt_gy_dps,wt_gz_dps,wt_roll_deg,wt_pitch_deg,wt_yaw_deg,"
        "board_a_norm_g,wt_a_norm_g,delta_a_norm_g,board_g_norm_dps,wt_g_norm_dps,delta_g_norm_dps\n";
  }

  ImuComparisonRecord capture(uint32_t now, ImuSample board, ImuSample wt,
                              float roll, float pitch, float yaw) {
    ImuComparisonRecord result;
    result.milliseconds = now;
    result.board = sanitize(board, now);
    result.wt = sanitize(wt, now);
    result.wtAngles[0] = result.wt.valid ? roll : NAN;
    result.wtAngles[1] = result.wt.valid ? pitch : NAN;
    result.wtAngles[2] = result.wt.valid ? yaw : NAN;
    if (result.board.valid && result.wt.valid) {
      result.skewMs = int32_t(now - wt.receivedMs) - int32_t(now - board.receivedMs);
      result.paired = std::abs(result.skewMs) <= 100 &&
          (!primed_ || (board.sequence != lastBoard_ && wt.sequence != lastWt_));
    }
    if (result.paired) {
      lastBoard_ = board.sequence;
      lastWt_ = wt.sequence;
      primed_ = true;
    }
    return result;
  }

  static float norm(const float values[3]) {
    return std::sqrt(values[0] * values[0] + values[1] * values[1] + values[2] * values[2]);
  }

  static size_t format(const ImuComparisonRecord &record, char *out, size_t capacity) {
    const auto &board = record.board;
    const auto &wt = record.wt;
    const float boardAcceleration = norm(board.accelerationG);
    const float wtAcceleration = norm(wt.accelerationG);
    const float boardGyro = norm(board.angularVelocityDps);
    const float wtGyro = norm(wt.angularVelocityDps);
    const int size = snprintf(out, capacity,
        "%lu,%lu,%lu,%lu,%lu,%u,%u,%u,%ld,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.3f,%.3f,%.3f,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
        (unsigned long)record.milliseconds, (unsigned long)board.receivedMs,
        (unsigned long)wt.receivedMs, (unsigned long)board.sequence,
        (unsigned long)wt.sequence, unsigned(board.valid), unsigned(wt.valid),
        unsigned(record.paired), (long)record.skewMs,
        board.accelerationG[0], board.accelerationG[1], board.accelerationG[2],
        board.angularVelocityDps[0], board.angularVelocityDps[1], board.angularVelocityDps[2],
        wt.accelerationG[0], wt.accelerationG[1], wt.accelerationG[2],
        wt.angularVelocityDps[0], wt.angularVelocityDps[1], wt.angularVelocityDps[2],
        record.wtAngles[0], record.wtAngles[1], record.wtAngles[2],
        boardAcceleration, wtAcceleration, record.paired ? wtAcceleration - boardAcceleration : NAN,
        boardGyro, wtGyro, record.paired ? wtGyro - boardGyro : NAN);
    return size > 0 && size_t(size) < capacity ? size_t(size) : 0;
  }

private:
  static ImuSample sanitize(ImuSample sample, uint32_t now) {
    sample.valid = sample.valid && uint32_t(now - sample.receivedMs) <= 250;
    for (unsigned axis = 0; axis < 3; ++axis)
      sample.valid = sample.valid && std::isfinite(sample.accelerationG[axis]) &&
          std::isfinite(sample.angularVelocityDps[axis]);
    if (!sample.valid)
      for (unsigned axis = 0; axis < 3; ++axis)
        sample.accelerationG[axis] = sample.angularVelocityDps[axis] = NAN;
    return sample;
  }
  uint32_t lastBoard_ = 0, lastWt_ = 0;
  bool primed_ = false;
};