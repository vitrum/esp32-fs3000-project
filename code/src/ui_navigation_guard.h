#pragma once

#include <stdint.h>

class UiNavigationGuard {
public:
  enum class Sample { Pressed, Released, Invalid };
  static constexpr uint32_t ReleaseMs = 30;
  static constexpr int MoveThresholdPx = 12;

  bool sample(Sample state, int16_t x, int16_t y, uint32_t now) {
    started_ = false;
    if (state == Sample::Invalid) {
      const bool cancel = pressed_;
      blocked_ = consumed_ = true;
      pressed_ = releasePending_ = false;
      return cancel;
    }
    if (state == Sample::Pressed) {
      releasePending_ = false;
      if (blocked_) return false;
      if (!down_) {
        down_ = started_ = true;
        moved_ = consumed_ = false;
        startX_ = x; startY_ = y;
      }
      const int dx = int(x) - startX_, dy = int(y) - startY_;
      if (dx >= MoveThresholdPx || dx <= -MoveThresholdPx ||
          dy >= MoveThresholdPx || dy <= -MoveThresholdPx) moved_ = true;
      x_ = x; y_ = y;
      pressed_ = true;
    } else if (down_ || blocked_) {
      if (!releasePending_) { releasePending_ = true; releasedSince_ = now; }
      // Only valid UP samples can end a touch, including a cancelled touch.
      if (uint32_t(now - releasedSince_) >= ReleaseMs) {
        down_ = blocked_ = pressed_ = releasePending_ = false;
      }
    } else pressed_ = false;
    return false;
  }

  bool navigate() {
    consumed_ = true;
    const bool cancel = pressed_;
    if (down_) {
      blocked_ = true;
      pressed_ = releasePending_ = false;
    }
    return cancel;
  }
  bool pressed() const { return pressed_; }
  bool started() const { return started_; }
  bool clickAllowed() const { return !moved_ && !consumed_ && !blocked_; }
  int16_t x() const { return x_; }
  int16_t y() const { return y_; }

private:
  bool down_ = false, pressed_ = false, blocked_ = false;
  bool moved_ = false, consumed_ = false, started_ = false, releasePending_ = false;
  int16_t x_ = 0, y_ = 0, startX_ = 0, startY_ = 0;
  uint32_t releasedSince_ = 0;
};
