#pragma once

#include <cstdint>
#include <cstring>

class DashboardLayout {
public:
  static constexpr unsigned Count = 5;
  static constexpr int Top = 44;
  static constexpr int Budget = 272;
  static constexpr uint32_t TapMinMs = 80;
  static constexpr uint32_t DragMinMs = 600;
  static constexpr uint32_t ActionGuardMs = 180;
  static constexpr int MoveThresholdPx = 12;
  struct Settings { uint8_t order[Count]; uint8_t levels[Count]; };

  DashboardLayout() { reset(); }
  void reset() {
    for (unsigned index = 0; index < Count; ++index) {
      settings_.order[index] = index;
      settings_.levels[index] = index < 3 ? 3 : 1;
      priority_[index] = 0;
    }
    serial_ = 0;
    calculate();
  }
  bool load(const Settings &settings) {
    unsigned seen = 0;
    for (unsigned index = 0; index < Count; ++index) {
      if (settings.order[index] >= Count || (seen & (1u << settings.order[index])) ||
          settings.levels[index] < 1 || settings.levels[index] > 4) return false;
      seen |= 1u << settings.order[index];
    }
    settings_ = settings;
    calculate();
    return true;
  }
  const Settings &settings() const { return settings_; }
  unsigned id(unsigned position) const { return settings_.order[position]; }
  unsigned level(unsigned id) const { return effective_[id]; }
  int height(unsigned id) const { return rowHeight(effective_[id]); }
  int top(unsigned position) const {
    int result = Top;
    for (unsigned index = 0; index < position; ++index) result += height(id(index)) + 2;
    return result;
  }
  unsigned positionAt(int coordinate) const {
    for (unsigned index = 0; index < Count; ++index)
      if (coordinate < top(index) + height(id(index))) return index;
    return Count - 1;
  }
  void cycle(unsigned id) {
    if (id >= Count) return;
    settings_.levels[id] = effective_[id] % 4 + 1;
    priority_[id] = ++serial_;
    calculate();
  }
  void move(unsigned from, unsigned to) {
    if (from >= Count || to >= Count || from == to) return;
    const uint8_t moving = settings_.order[from];
    if (from < to) for (unsigned index = from; index < to; ++index) settings_.order[index] = settings_.order[index + 1];
    else for (unsigned index = from; index > to; --index) settings_.order[index] = settings_.order[index - 1];
    settings_.order[to] = moving;
  }
  void press(unsigned position, int coordinate, uint32_t now, int horizontal = 12) {
    if (pressed_) return;
    moved_ = dragging_ = false;
    pressed_ = position < Count && coordinate >= Top && coordinate < Top + Budget &&
      horizontal >= 12 && horizontal < 228;
    if (!pressed_) return;
    position_ = position;
    pressY_ = coordinate;
    pressX_ = horizontal;
    pressMs_ = now;
    moved_ = dragging_ = false;
  }
  bool motion(int coordinate, uint32_t now, int horizontal = 12) {
    if (!pressed_) return false;
    const int distance = coordinate > pressY_ ? coordinate - pressY_ : pressY_ - coordinate;
    const int horizontalDistance = horizontal > pressX_ ? horizontal - pressX_ : pressX_ - horizontal;
    if (distance >= MoveThresholdPx || horizontalDistance >= MoveThresholdPx) moved_ = true;
    if (distance >= MoveThresholdPx && now - pressMs_ >= DragMinMs) dragging_ = true;
    return dragging_;
  }
  bool release(int coordinate, uint32_t now, int horizontal = 12) {
    if (!pressed_) return false;
    motion(coordinate, now, horizontal);
    pressed_ = false;
    if (coordinate < Top || coordinate >= Top + Budget || horizontal < 12 || horizontal >= 228) return false;
    if (acted_ && now - lastActionMs_ < ActionGuardMs) return false;
    if (dragging_) {
      const unsigned target = positionAt(coordinate);
      if (target == position_) return false;
      move(position_, target);
    } else {
      const uint32_t elapsed = now - pressMs_;
      if (moved_ || elapsed < TapMinMs) return false;
      cycle(id(position_));
    }
    lastActionMs_ = now;
    acted_ = true;
    return true;
  }
  void cancel() { pressed_ = moved_ = dragging_ = false; }

private:
  static int rowHeight(unsigned level) {
    const int heights[] = {0, 36, 44, 56, 64};
    return heights[level];
  }
  void calculate() {
    memcpy(effective_, settings_.levels, Count);
    for (;;) {
      int used = (Count - 1) * 2;
      unsigned largest = 0;
      for (unsigned index = 0; index < Count; ++index) {
        used += rowHeight(effective_[index]);
        if (effective_[index] == 4) ++largest;
      }
      if (used <= Budget && largest <= 3) break;
      unsigned candidate = Count;
      for (unsigned index = 0; index < Count; ++index) {
        if (effective_[index] <= 1 || (largest > 3 && effective_[index] != 4)) continue;
        if (candidate == Count || priority_[index] < priority_[candidate]) candidate = index;
      }
      if (candidate == Count) break;
      --effective_[candidate];
    }
  }
  Settings settings_{};
  uint8_t effective_[Count]{};
  uint32_t priority_[Count]{}, serial_ = 0;
  uint32_t pressMs_ = 0;
  uint32_t lastActionMs_ = 0;
  int pressY_ = 0, pressX_ = 12;
  unsigned position_ = 0;
  bool pressed_ = false, moved_ = false, dragging_ = false;
  bool acted_ = false;
};