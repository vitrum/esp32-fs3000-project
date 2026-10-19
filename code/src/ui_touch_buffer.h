#pragma once

#include <stdint.h>

// 轻量的触控缓冲：用于 BLE 相关按钮（扫描 / 选择 / 确认 / 返回）。
//
// 目的：
//   1. 吃掉同一次按下的重复 CLICKED（LVGL 偶发、或手指轻微抖动导致的连击）；
//   2. 忽略按钮刚显示后立刻落下的“穿透点击”（例如界面切换时手指还没抬起）；
//   3. 给阻塞式 BLE 操作留出提交窗口，避免同一设备被连续确认两次。
//
// 每个按钮持有一个实例，在事件回调里这样使用：
//   if (!g_display->xxGuard_.accept(millis())) return;
//
// accept() 只有在 (a) 距上次被接受超过 minGapMs，且 (b) 距最近一次 reset()
// 超过 minGapMs 时才返回 true。
class TouchBuffer {
public:
  explicit TouchBuffer(uint32_t minGapMs = 450) : minGapMs_(minGapMs) {}

  // 界面刚切换/面板刚重新显示时调用：在 minGapMs 之内拒绝一切点击。
  void reset(uint32_t now) {
    lastAcceptMs_ = now;
    lastResetMs_ = now;
    primed_ = true;
  }

  // 是否接受这一次点击。now 应传 millis()。
  bool accept(uint32_t now) {
    if (!primed_) {
      lastAcceptMs_ = now;
      primed_ = true;
      return true;
    }
    // 刚 reset 过：视作“按钮刚出现”，缓冲期内不响应。
    if (uint32_t(now - lastResetMs_) < minGapMs_) return false;
    // 同一次按下产生的重复 CLICKED。
    if (uint32_t(now - lastAcceptMs_) < minGapMs_) return false;
    lastAcceptMs_ = now;
    return true;
  }

  void setMinGap(uint32_t ms) { minGapMs_ = ms; }

private:
  uint32_t minGapMs_ = 450;
  uint32_t lastAcceptMs_ = 0;
  uint32_t lastResetMs_ = 0;
  bool primed_ = false;
};
