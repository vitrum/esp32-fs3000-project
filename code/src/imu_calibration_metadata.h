#pragma once

#include "imu_mount_calibration.h"
#include <cstring>

struct ImuCalibrationMetadata {
  ImuMountCalibration::Result calibration;
  char address[18] = {};
  uint32_t utcSeconds = 0, utcMilliseconds = 0, traceSession = 0;
  bool utcKnown = false, utcTrusted = false, trace = false;

  size_t format(char *out, size_t capacity) const {
    for (const char *p=address; *p; ++p)
      if (!((*p>='0' && *p<='9') || (*p>='a' && *p<='f') ||
            (*p>='A' && *p<='F') || *p==':')) return 0;
    const auto &c=calibration;
    const int size=snprintf(out,capacity,
      "{\"schema\":1,\"algorithm\":\"forearm-functional-v1\",\"device\":\"WT9011DCL-BT50\","
      "\"address\":\"%s\",\"trace\":%s,\"trace_session\":%lu,\"calibration_id\":%lu,\"valid\":%s,"
      "\"reference_ms\":%lu,\"confirmed_ms\":%lu,\"board_forward_axis\":%d,"
      "\"wearing\":\"right distal forearm; USB toward elbow; logo/buttons dorsal; palm-down reference\","
      "\"coordinates\":\"X forward,Y left,Z up; fixed reference-axis vectors, not dynamic world vectors\","
      "\"board_native_to_reference\":[[%.8f,%.8f,%.8f],[%.8f,%.8f,%.8f],[%.8f,%.8f,%.8f]],"
      "\"wt_native_to_reference\":[[%.8f,%.8f,%.8f],[%.8f,%.8f,%.8f],[%.8f,%.8f,%.8f]],"
      "\"board_bias_dps\":[%.6f,%.6f,%.6f],\"wt_bias_dps\":[%.6f,%.6f,%.6f],"
      "\"roll_off_axis\":%.6f,\"lift_off_axis\":%.6f,\"roll_samples\":%lu,\"lift_samples\":%lu,"
      "\"utc_anchor\":{\"known\":%s,\"trusted\":%s,\"utc_seconds\":%lu,\"device_ms\":%lu},"
      "\"limitations\":\"user-confirmed functional reference; no position, magnetic north or BLE latency calibration\"}\n",
      address,trace?"true":"false",(unsigned long)traceSession,(unsigned long)c.id,c.valid?"true":"false",
      (unsigned long)c.referenceMs,(unsigned long)c.confirmedMs,int(c.forwardAxis),
      c.board[0][0],c.board[0][1],c.board[0][2],c.board[1][0],c.board[1][1],c.board[1][2],c.board[2][0],c.board[2][1],c.board[2][2],
      c.wt[0][0],c.wt[0][1],c.wt[0][2],c.wt[1][0],c.wt[1][1],c.wt[1][2],c.wt[2][0],c.wt[2][1],c.wt[2][2],
      c.boardBias[0],c.boardBias[1],c.boardBias[2],c.wtBias[0],c.wtBias[1],c.wtBias[2],
      c.rollOffAxis,c.liftOffAxis,(unsigned long)c.rollSamples,(unsigned long)c.liftSamples,
      utcKnown?"true":"false",utcTrusted?"true":"false",(unsigned long)utcSeconds,(unsigned long)utcMilliseconds);
    return size>0 && size_t(size)<capacity ? size_t(size) : 0;
  }
};
