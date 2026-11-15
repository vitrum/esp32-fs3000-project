#include "imu_mount_calibration.h"
#include <cassert>
#include <iostream>

using Calibration = ImuMountCalibration;
using Stage = Calibration::Stage;

struct Fixture {
  Calibration calibration;
  ImuSample board, wt;
  uint32_t now = 100;
  uint32_t period = 20;
  bool rotated = false;
  Fixture(bool rotation = false) : rotated(rotation) {
    board.valid = wt.valid = true;
    board.accelerationG[2] = wt.accelerationG[2] = 1;
    step();
  }
  void step(float roll = 0, float lift = 0, float speed = 0, bool lifting = false) {
    now += period;
    board.receivedMs = wt.receivedMs = now;
    ++board.sequence; ++wt.sequence;
    const float r = roll / 57.2957795f, p = lift / 57.2957795f;
    float acceleration[3] = {std::sin(p),std::sin(r)*std::cos(p),std::cos(r)*std::cos(p)};
    float gyro[3] = {lifting ? 0.f : speed,lifting ? -speed : 0.f,0};
    for (unsigned i=0;i<3;++i) {
      // Proper cyclic permutation: native X=reference Z, Y=X, Z=Y.
      wt.accelerationG[i] = acceleration[rotated ? (i+2)%3 : i];
      wt.angularVelocityDps[i] = gyro[rotated ? (i+2)%3 : i];
    }
    calibration.update(now,true,board,wt);
  }
  void stationary() {
    assert(calibration.begin(now,1));
    for (unsigned i=0;i<160;++i) step();
    assert(calibration.stage()==Stage::Roll);
  }
  void gesture(bool lift) {
    const Stage stage=lift ? Stage::Lift : Stage::Roll;
    for (unsigned cycle=0;cycle<2 && calibration.stage()==stage;++cycle) {
      for (unsigned i=1;i<=100 && calibration.stage()==stage;++i)
        step(lift ? 0 : i*.6f,lift ? i*.4f : 0,lift ? 20 : 30,lift);
      for (int i=99;i>=0 && calibration.stage()==stage;--i)
        step(lift ? 0 : i*.6f,lift ? i*.4f : 0,lift ? -20 : -30,lift);
    }
  }
  void complete() {
    stationary(); gesture(false);
    assert(calibration.stage()==Stage::Lift);
    gesture(true);
    assert(calibration.stage()==Stage::Hood);
    for (unsigned i=0;i<160;++i) step();
    assert(calibration.stage()==Stage::Confirm);
    assert(!calibration.result().valid);
    assert(!calibration.confirm(now,0));
    assert(calibration.confirm(now,42));
  }
};

int main() {
  for (bool rotated : {false,true}) {
    Fixture f(rotated);
    assert(f.calibration.stage()==Stage::Wearing);
    f.complete();
    const auto &result=f.calibration.result();
    assert(result.valid && result.id==42 && result.rollSamples>100 && result.liftSamples>100);
    float input[3]={}, output[3];
    input[rotated ? 1 : 0]=3;
    Calibration::transform(result.wt,input,output);
    assert(std::fabs(output[0]-3)<1e-4 && std::fabs(output[1])<1e-4 && std::fabs(output[2])<1e-4);
    f.calibration.update(f.now+251,true,f.board,f.wt);
    assert(!f.calibration.result().valid && f.calibration.error()==Calibration::Error::Data);
  }
  Fixture duplicate;
  duplicate.stationary();
  const auto original=duplicate.wt;
  for (unsigned i=0;i<200;++i) duplicate.calibration.update(duplicate.now,true,duplicate.board,original);
  assert(duplicate.calibration.stage()==Stage::Roll && duplicate.calibration.cycles()==0);
  duplicate.calibration.update(duplicate.now,false,duplicate.board,duplicate.wt);
  assert(duplicate.calibration.stage()==Stage::Offline);
  Fixture wrong;
  wrong.stationary();
  for (unsigned cycle=0;cycle<2 && wrong.calibration.stage()==Stage::Roll;++cycle) {
    for (unsigned i=1;i<=100 && wrong.calibration.stage()==Stage::Roll;++i) wrong.step(-i*.6f,0,-30);
    for (int i=99;i>=0 && wrong.calibration.stage()==Stage::Roll;--i) wrong.step(-i*.6f,0,30);
  }
  assert(wrong.calibration.stage()==Stage::Lift);
  wrong.gesture(true);
  assert(wrong.calibration.stage()==Stage::Failed);
  Fixture parallel;
  parallel.stationary();
  parallel.wt.angularVelocityDps[2]=30;
  parallel.now+=20;
  parallel.board.receivedMs=parallel.wt.receivedMs=parallel.now;
  ++parallel.wt.sequence;
  parallel.calibration.update(parallel.now,true,parallel.board,parallel.wt);
  assert(parallel.calibration.error()==Calibration::Error::Axis);
  Fixture moved;
  moved.stationary();
  moved.board.angularVelocityDps[1]=20;
  moved.step();
  assert(moved.calibration.error()==Calibration::Error::BoardMoved);
  Fixture timeout;
  timeout.stationary();
  timeout.now+=45001;
  timeout.board.receivedMs=timeout.wt.receivedMs=timeout.now;
  ++timeout.wt.sequence;
  timeout.calibration.update(timeout.now,true,timeout.board,timeout.wt);
  assert(timeout.calibration.error()==Calibration::Error::Timeout);
  Fixture rate;
  rate.period=100;
  assert(rate.calibration.begin(rate.now,1));
  for (unsigned i=0;i<32;++i) rate.step();
  assert(rate.calibration.error()==Calibration::Error::Rate);
  Fixture vertical;
  assert(vertical.calibration.begin(vertical.now,3));
  for (unsigned i=0;i<160;++i) vertical.step();
  assert(vertical.calibration.error()==Calibration::Error::Axis);
  Fixture reboot;
  reboot.complete();
  reboot.wt.sequence=0;
  reboot.board.receivedMs=reboot.wt.receivedMs=reboot.now;
  reboot.calibration.update(reboot.now,true,reboot.board,reboot.wt);
  assert(!reboot.calibration.result().valid && reboot.calibration.error()==Calibration::Error::Data);
  Fixture wrap;
  wrap.now=0xfffff000u;
  wrap.step(); wrap.complete();
  assert(wrap.calibration.result().valid);
  std::cout << "PASS IMU mounting: guided gestures, proper rotations, explicit confirmation, stale/duplicate data, motion failures and wrap\n";
}
