#pragma once

#include "imu_sample.h"
#include <cmath>
#include <initializer_list>

class RiderPosture {
public:
  enum class Placement : uint8_t { UpperArm, BackStrap, Helmet, Forearm };
  enum class State : uint8_t { NoData, Moving, NeedUpright, NeedLean, Settling, Upright, Forward, Low };
  struct Estimate {
    State state = State::NeedUpright;
    float relativeDegrees = 0;
    bool indirect = true;
  };

  Placement placement() const { return placement_; }
  void setPlacement(Placement placement) {
    if (uint8_t(placement) > uint8_t(Placement::Forearm) || placement == placement_) return;
    placement_ = placement;
    upright_ = lean_ = tracking_ = false;
    result_ = Estimate{};
  }

  bool calibrate(bool leaning, uint32_t now, const ImuSample &board, const ImuSample &wearable) {
    if (!quiet(now, board, wearable)) return false;
    float wearUnit[3];
    unit(wearable.accelerationG, wearUnit);
    if (!leaning) {
      for (unsigned axis = 0; axis < 3; ++axis) reference_[axis] = wearUnit[axis];
      boardPitch_ = pitch(board);
      boardRoll_ = roll(board);
      upright_ = true;
      lean_ = tracking_ = false;
      result_.state = State::NeedLean;
      return true;
    }
    if (!upright_ || std::fabs(angleDifference(pitch(board), boardPitch_)) > 5 ||
        std::fabs(angleDifference(roll(board), boardRoll_)) > 5) return false;
    const float inclination = std::acos(clamp(dot(reference_, wearUnit))) * kDegrees;
    if (inclination < 8 || inclination > 100) return false;
    cross(reference_, wearUnit, rotationAxis_);
    unit(rotationAxis_, rotationAxis_);
    forwardDegrees_ = inclination;
    lean_ = true;
    tracking_ = false;
    result_.state = State::Settling;
    return true;
  }

  Estimate update(uint32_t now, const ImuSample &board, const ImuSample &wearable) {
    result_.indirect = placement_ != Placement::BackStrap &&
        placement_ != Placement::Forearm;
    if (!fresh(now, board) || !fresh(now, wearable) ||
        std::abs(int32_t(now - board.receivedMs) - int32_t(now - wearable.receivedMs)) > 100) {
      tracking_ = false;
      result_.state = State::NoData;
      return result_;
    }
    if (!quiet(now, board, wearable) || (upright_ &&
        std::fabs(angleDifference(roll(board), boardRoll_)) > 20)) {
      tracking_ = false;
      result_.state = State::Moving;
      return result_;
    }
    if (!upright_ || !lean_) {
      result_.state = upright_ ? State::NeedLean : State::NeedUpright;
      return result_;
    }
    float wearUnit[3], direction[3];
    unit(wearable.accelerationG, wearUnit);
    cross(reference_, wearUnit, direction);
    const float measured = std::atan2(dot(rotationAxis_, direction), dot(reference_, wearUnit)) * kDegrees;
    const float compensated = measured - angleDifference(pitch(board), boardPitch_);
    result_.relativeDegrees = compensated;
    const float sideways = std::fabs(dot(rotationAxis_, wearUnit));
    if (sideways > 0.35f || compensated < -15 || compensated > 110) {
      tracking_ = false;
      result_.state = State::Moving;
      return result_;
    }
    const float ratio = compensated / forwardDegrees_;
    const float lowThreshold = placement_ == Placement::BackStrap ? 1.35f :
        placement_ == Placement::UpperArm ? 1.65f : 1.8f;
    State candidate = ratio < 0.35f ? State::Upright : ratio < lowThreshold ? State::Forward : State::Low;
    if (!tracking_ || candidate != candidate_) {
      candidate_ = candidate;
      candidateSince_ = now;
      tracking_ = true;
      result_.state = State::Settling;
    } else if (uint32_t(now - candidateSince_) >= 2000) {
      result_.state = candidate;
    }
    return result_;
  }

private:
  static constexpr float kDegrees = 57.2957795f;
  static float clamp(float value) { return value < -1 ? -1 : value > 1 ? 1 : value; }
  static float dot(const float first[3], const float second[3]) {
    return first[0] * second[0] + first[1] * second[1] + first[2] * second[2];
  }
  static void cross(const float first[3], const float second[3], float out[3]) {
    out[0] = first[1] * second[2] - first[2] * second[1];
    out[1] = first[2] * second[0] - first[0] * second[2];
    out[2] = first[0] * second[1] - first[1] * second[0];
  }
  static void unit(const float values[3], float out[3]) {
    const float length = std::sqrt(dot(values, values));
    for (unsigned axis = 0; axis < 3; ++axis) out[axis] = values[axis] / length;
  }
  static float pitch(const ImuSample &sample) {
    return std::atan2(-sample.accelerationG[0], std::hypot(sample.accelerationG[1], sample.accelerationG[2])) * kDegrees;
  }
  static float roll(const ImuSample &sample) {
    return std::atan2(sample.accelerationG[1], sample.accelerationG[2]) * kDegrees;
  }
  static float angleDifference(float value, float reference) {
    float difference = std::fmod(value - reference + 540.0f, 360.0f) - 180.0f;
    return difference;
  }
  static bool fresh(uint32_t now, const ImuSample &sample) {
    if (!sample.valid || uint32_t(now - sample.receivedMs) > 250) return false;
    for (unsigned axis = 0; axis < 3; ++axis)
      if (!std::isfinite(sample.accelerationG[axis]) || !std::isfinite(sample.angularVelocityDps[axis])) return false;
    return true;
  }
  static bool quiet(uint32_t now, const ImuSample &board, const ImuSample &wearable) {
    if (!fresh(now, board) || !fresh(now, wearable) ||
        std::abs(int32_t(now - board.receivedMs) - int32_t(now - wearable.receivedMs)) > 100) return false;
    for (const ImuSample *sample : {&board, &wearable}) {
      const float acceleration = dot(sample->accelerationG, sample->accelerationG);
      if (acceleration < 0.81f || acceleration > 1.21f ||
          dot(sample->angularVelocityDps, sample->angularVelocityDps) > 1600) return false;
    }
    return true;
  }

  Placement placement_ = Placement::Forearm;
  Estimate result_;
  bool upright_ = false, lean_ = false, tracking_ = false;
  float reference_[3] = {}, rotationAxis_[3] = {};
  float boardPitch_ = 0, boardRoll_ = 0, forwardDegrees_ = 0;
  State candidate_ = State::NoData;
  uint32_t candidateSince_ = 0;
};