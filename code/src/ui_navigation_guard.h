#pragma once

#include <stdint.h>

class UiNavigationGuard {
public:
  bool beginBack(uint32_t now) {
    if (blocked_) return false;
    blocked_ = true;
    blockedSince_ = now;
    trackingRelease_ = false;
    return true;
  }

  bool filterTouch(bool pressed, uint32_t now) {
    if (!blocked_) return pressed;
    if (pressed) {
      trackingRelease_ = false;
    } else {
      if (!trackingRelease_) {
        trackingRelease_ = true;
        releasedSince_ = now;
      }
      // A quiet release, not just a rendering delay, ends the old gesture.
      if (uint32_t(now - blockedSince_) >= 250 &&
          uint32_t(now - releasedSince_) >= 120)
        blocked_ = false;
    }
    return false;
  }

private:
  bool blocked_ = false;
  bool trackingRelease_ = false;
  uint32_t blockedSince_ = 0;
  uint32_t releasedSince_ = 0;
};
