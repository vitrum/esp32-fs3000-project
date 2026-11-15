#pragma once

#include "imu_sample.h"
#include <cmath>
#include <cstdio>

class ImuMountCalibration {
public:
  enum class Stage : uint8_t { Offline, Waiting, Wearing, Static, Roll, Lift, Hood, Confirm, Ready, Failed };
  enum class Error : uint8_t { None, Data, BoardMoved, Motion, Axis, Timeout, Rate, Cancelled };
  struct Result {
    uint32_t id = 0, referenceMs = 0, confirmedMs = 0;
    float board[3][3] = {}, wt[3][3] = {};
    float boardBias[3] = {}, wtBias[3] = {};
    float rollOffAxis = 0, liftOffAxis = 0;
    uint32_t rollSamples = 0, liftSamples = 0;
    int8_t forwardAxis = 1;
    bool valid = false;
  };

  Stage stage() const { return stage_; }
  Error error() const { return error_; }
  const Result &result() const { return result_; }
  unsigned cycles() const { return cycles_; }
  bool active() const { return stage_ >= Stage::Static && stage_ <= Stage::Confirm; }
  void cancel() { fail(Error::Cancelled); }
  void invalidate() { result_.valid = false; stage_ = Stage::Offline; error_ = Error::Data; }

  bool begin(uint32_t now, int8_t forwardAxis) {
    if (stage_ != Stage::Wearing && stage_ != Stage::Failed && stage_ != Stage::Ready) return false;
    if (!forwardAxis || forwardAxis < -3 || forwardAxis > 3) return false;
    result_ = Result{};
    result_.forwardAxis = forwardAxis;
    error_ = Error::None;
    enter(Stage::Static, now);
    return true;
  }

  bool confirm(uint32_t now, uint32_t id) {
    if (stage_ != Stage::Confirm || !id) return false;
    result_.id = id;
    result_.confirmedMs = now;
    result_.valid = true;
    stage_ = Stage::Ready;
    return true;
  }

  void update(uint32_t now, bool connected, const ImuSample &board, const ImuSample &wt) {
    if (!connected) { invalidate(); return; }
    if (!fresh(now, board) || !fresh(now, wt) ||
        std::abs(int32_t(board.receivedMs - wt.receivedMs)) > 100) {
      if (active() || stage_ == Stage::Ready) fail(Error::Data);
      else if (stage_ != Stage::Failed) stage_ = Stage::Waiting;
      return;
    }
    if (stage_ == Stage::Offline || stage_ == Stage::Waiting) {
      stage_ = Stage::Wearing;
      error_ = Error::None;
    }
    if (stage_ == Stage::Ready) {
      if (haveSequence_ && (int32_t(wt.sequence - previousSequence_) < 0 ||
                           int32_t(board.sequence - previousBoardSequence_) < 0)) fail(Error::Data);
      previousSequence_ = wt.sequence; previousBoardSequence_ = board.sequence;
      haveSequence_ = true;
      return;
    }
    if (!active()) return;
    if (uint32_t(now - stageStarted_) > 45000) { fail(Error::Timeout); return; }
    if (haveSequence_ && (int32_t(wt.sequence-previousSequence_)<0 ||
                          int32_t(board.sequence-previousBoardSequence_)<0)) { fail(Error::Data); return; }
    if (haveSequence_ && wt.sequence == previousSequence_) return;
    const uint32_t dtMs = haveSequence_ ? uint32_t(wt.receivedMs - previousTime_) : 0;
    previousSequence_ = wt.sequence; previousBoardSequence_ = board.sequence;
    previousTime_ = wt.receivedMs;
    const bool hadSample = haveSequence_;
    haveSequence_ = true;
    if (hadSample && (!dtMs || dtMs > 250)) { fail(Error::Data); return; }
    if (stage_ == Stage::Static) {
      if (!quiet(board, 5) || !quiet(wt, 5)) { resetStatic(now); return; }
      if (samples_ && (distance(board.accelerationG, boardSum_, float(samples_)) > .08f ||
                       distance(wt.accelerationG, wtSum_, float(samples_)) > .08f)) {
        resetStatic(now); return;
      }
      for (unsigned axis = 0; axis < 3; ++axis) {
        boardSum_[axis] += board.accelerationG[axis]; wtSum_[axis] += wt.accelerationG[axis];
        result_.boardBias[axis] += board.angularVelocityDps[axis];
        result_.wtBias[axis] += wt.angularVelocityDps[axis];
      }
      ++samples_;
      if (uint32_t(now - quietSince_) < 3000) return;
      if (samples_ * 1000.f / uint32_t(now-quietSince_) < 20) { fail(Error::Rate); return; }
      for (unsigned axis = 0; axis < 3; ++axis) {
        result_.boardBias[axis] /= samples_; result_.wtBias[axis] /= samples_;
      }
      normalize(boardSum_); normalize(wtSum_);
      float forward[3] = {};
      forward[std::abs(int(result_.forwardAxis)) - 1] = result_.forwardAxis > 0 ? 1 : -1;
      if (!basis(forward, boardSum_, result_.board)) { fail(Error::Axis); return; }
      result_.referenceMs = now;
      enter(Stage::Roll, now);
      return;
    }
    float boardGyro[3], gyro[3], boardUp[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
      boardGyro[axis] = board.angularVelocityDps[axis] - result_.boardBias[axis];
      gyro[axis] = wt.angularVelocityDps[axis] - result_.wtBias[axis];
      boardUp[axis] = board.accelerationG[axis];
    }
    normalize(boardUp);
    if (norm(boardGyro) > 8 || dot(boardUp, boardSum_) < .996f) { fail(Error::BoardMoved); return; }
    if (stage_ == Stage::Hood || stage_ == Stage::Confirm) {
      float up[3]; transform(result_.wt, wt.accelerationG, up);
      const bool stable = quiet(wt, 5) && quiet(board, 5) &&
          up[2] / norm(up) > .3f && std::fabs(up[0] / norm(up)) < .87f;
      if (!stable) {
        if (stage_ == Stage::Confirm) enter(Stage::Hood, now);
        quietSince_ = now; samples_ = 0;
      } else {
        ++samples_;
        if (stage_ == Stage::Hood && uint32_t(now - quietSince_) >= 3000 && samples_ >= 60)
          enter(Stage::Confirm, now);
      }
      return;
    }
    if (!dtMs) return;
    ++samples_;
    float axis[3];
    if (stage_ == Stage::Roll) {
      if (!axisSet_) {
        if (norm(gyro) < 12) return;
        for (unsigned i = 0; i < 3; ++i) rollAxis_[i] = gyro[i];
        normalize(rollAxis_);
        if (std::fabs(dot(rollAxis_, wtSum_)) > .35f) { fail(Error::Axis); return; }
        axisSet_ = true;
      }
      for (unsigned i = 0; i < 3; ++i) axis[i] = rollAxis_[i];
    } else {
      for (unsigned i = 0; i < 3; ++i) axis[i] = -result_.wt[1][i];
    }
    const float speed = dot(gyro, axis), magnitude = norm(gyro);
    if (magnitude > 180) { fail(Error::Motion); return; }
    const float dt = dtMs / 1000.f;
    const float offAxis = std::sqrt(std::fmax(0.f, magnitude * magnitude - speed * speed));
    offAxisAngle_ += offAxis * dt;
    travel_ += std::fabs(speed) * dt;
    angle_ += speed * dt;
    if (angle_ < -15 || angle_ > 150) { fail(Error::Motion); return; }
    if (angle_ > (stage_ == Stage::Roll ? 45 : 25)) outward_ = true;
    if (!outward_ || angle_ > 8 || speed > 0) return;
    if (++cycles_ < 2) { outward_ = false; return; }
    const float seconds = uint32_t(now - stageStarted_) / 1000.f;
    if (seconds <= 0 || samples_ / seconds < 20) { fail(Error::Rate); return; }
    if (offAxisAngle_ / std::fmax(travel_, 1.f) > .35f) { fail(Error::Axis); return; }
    float up[3];
    for (unsigned i = 0; i < 3; ++i) up[i] = wt.accelerationG[i];
    normalize(up);
    if (dot(up, wtSum_) < .94f) { fail(Error::Motion); return; }
    if (stage_ == Stage::Roll) {
      if (!basis(rollAxis_, wtSum_, result_.wt)) { fail(Error::Axis); return; }
      result_.rollOffAxis = offAxisAngle_ / std::fmax(travel_, 1.f);
      result_.rollSamples = samples_;
      enter(Stage::Lift, now);
    } else {
      result_.liftOffAxis = offAxisAngle_ / std::fmax(travel_, 1.f);
      result_.liftSamples = samples_;
      enter(Stage::Hood, now);
    }
  }

  static void transform(const float matrix[3][3], const float input[3], float output[3],
                        const float *bias = nullptr) {
    for (unsigned row = 0; row < 3; ++row) {
      output[row] = 0;
      for (unsigned column = 0; column < 3; ++column)
        output[row] += matrix[row][column] * (input[column] - (bias ? bias[column] : 0));
    }
  }

private:
  static bool fresh(uint32_t now, const ImuSample &sample) {
    if (!sample.valid || uint32_t(now - sample.receivedMs) > 250) return false;
    for (unsigned i = 0; i < 3; ++i)
      if (!std::isfinite(sample.accelerationG[i]) || !std::isfinite(sample.angularVelocityDps[i])) return false;
    return norm(sample.accelerationG) > .05f;
  }
  static float dot(const float *a, const float *b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
  static float norm(const float *a) { return std::sqrt(dot(a,a)); }
  static void normalize(float *a) { const float length = norm(a); if (length > 0) for (unsigned i=0;i<3;++i) a[i]/=length; }
  static bool quiet(const ImuSample &s, float gyro) {
    return norm(s.accelerationG) >= .9f && norm(s.accelerationG) <= 1.1f &&
        norm(s.angularVelocityDps) <= gyro;
  }
  static float distance(const float *a, const float *sum, float count) {
    float delta[3]; for (unsigned i=0;i<3;++i) delta[i]=a[i]-sum[i]/count;
    return norm(delta);
  }
  static void cross(const float *a,const float *b,float *c) {
    c[0]=a[1]*b[2]-a[2]*b[1]; c[1]=a[2]*b[0]-a[0]*b[2]; c[2]=a[0]*b[1]-a[1]*b[0];
  }
  static bool basis(const float *forward, const float *up, float matrix[3][3]) {
    const float projection = dot(forward, up);
    for (unsigned i=0;i<3;++i) {
      matrix[0][i]=forward[i]-projection*up[i]; matrix[2][i]=up[i];
    }
    if (norm(matrix[0]) < .5f) return false;
    normalize(matrix[0]); cross(matrix[2],matrix[0],matrix[1]); normalize(matrix[1]);
    cross(matrix[0],matrix[1],matrix[2]);
    return true;
  }
  void fail(Error error) { error_=error; result_.valid=false; stage_=Stage::Failed; }
  void resetStatic(uint32_t now) {
    quietSince_=now; samples_=0;
    for (unsigned i=0;i<3;++i) {
      boardSum_[i]=wtSum_[i]=0; result_.boardBias[i]=result_.wtBias[i]=0;
    }
  }
  void enter(Stage stage, uint32_t now) {
    stage_=stage; stageStarted_=quietSince_=now; samples_=cycles_=0;
    angle_=travel_=offAxisAngle_=0; outward_=axisSet_=false; haveSequence_=false;
    if (stage==Stage::Static) resetStatic(now);
  }
  Stage stage_ = Stage::Offline;
  Error error_ = Error::None;
  Result result_;
  uint32_t stageStarted_=0, quietSince_=0, previousTime_=0, previousSequence_=0, previousBoardSequence_=0;
  unsigned samples_=0, cycles_=0;
  float boardSum_[3]={}, wtSum_[3]={}, rollAxis_[3]={};
  float angle_=0, travel_=0, offAxisAngle_=0;
  bool haveSequence_=false, outward_=false, axisSet_=false;
};
