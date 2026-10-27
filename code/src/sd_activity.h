#pragma once

#include <atomic>
#include <cstdint>

class SdActivity {
public:
  enum class State { Absent, Ready, Writing, Error };
  void beginWrite() { active_.fetch_add(1); }
  void noteStatus(bool available) { responding_.store(available); }
  void endWrite(uint32_t now, bool success) {
    lastWrite_.store(now);
    written_.store(true);
    failed_.store(!success);
    active_.fetch_sub(1);
  }
  State state(bool available, uint32_t now) const {
    if (!available || !responding_.load()) return State::Absent;
    if (failed_.load()) return State::Error;
    if (active_.load() || (written_.load() && now - lastWrite_.load() < 600)) return State::Writing;
    return State::Ready;
  }
private:
  std::atomic<unsigned> active_{0};
  std::atomic<uint32_t> lastWrite_{0};
  std::atomic<bool> written_{false}, failed_{false};
  std::atomic<bool> responding_{true};
};

extern SdActivity sdActivity;