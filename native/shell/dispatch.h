// The wheel and buttons turned into actions, with 1.0.x's rules (input/WheelInput.kt):
// - every wheel pulse is a step (two per felt click); pulses under 154 ms apart in one direction are a
//   spin, and acceleration multiplies its steps;
// - Center: Select on release, LongSelect after a 600 ms hold (then nothing on release);
// - Menu: Back on release; Play: PlayPause on press;
// - Next/Prev: Next/Prev on release; held 600 ms, LongNext/LongPrev every 300 ms, SeekEnd on release.
#pragma once
#include <stdint.h>

#include <functional>
#include <string>

#include "../platform/platform.h"

namespace rondo::shell {

enum class Action { Up, Down, Select, LongSelect, Back, PlayPause, Next, Prev, LongNext, LongPrev, SeekEnd };

class Dispatcher {
 public:
  std::function<void(Action, int steps)> emit;
  std::string acceleration = "normal";  // off | normal | fast

  void event(const InputEvent& e) {
    const int64_t t = e.time;
    switch (e.key) {
      case Key::WheelCw: pulse(1, t); break;
      case Key::WheelCcw: pulse(-1, t); break;
      case Key::Center:
        if (e.down) {
          centerAt_ = t;
          longFired_ = false;
        } else if (centerAt_) {
          centerAt_ = 0;
          if (!longFired_) emit(Action::Select, 1);
        }
        break;
      case Key::Menu:
        if (!e.down) emit(Action::Back, 1);
        break;
      case Key::Play:
        if (e.down) emit(Action::PlayPause, 1);
        break;
      case Key::Next:
      case Key::Prev:
        if (e.down) {
          held_ = e.key;
          heldAt_ = t;
          nextRepeat_ = t + kLong;
          longFired_ = false;
        } else if (held_ == e.key) {
          held_ = Key::None;
          if (longFired_) emit(Action::SeekEnd, 1);
          else emit(e.key == Key::Next ? Action::Next : Action::Prev, 1);
        }
        break;
      default: break;
    }
  }

  // Fires the holds that are due at `now`.
  void tick(int64_t now) {
    if (centerAt_ && !longFired_ && now - centerAt_ >= kLong) {
      longFired_ = true;
      emit(Action::LongSelect, 1);
    }
    while (held_ != Key::None && now >= nextRepeat_) {
      longFired_ = true;
      emit(held_ == Key::Next ? Action::LongNext : Action::LongPrev, 1);
      nextRepeat_ += kRepeat;
    }
  }

  // When tick() next has something to do (0: nothing held).
  int64_t nextDeadline() const {
    if (centerAt_ && !longFired_) return centerAt_ + kLong;
    if (held_ != Key::None) return nextRepeat_;
    return 0;
  }

 private:
  static constexpr int64_t kGap = 154000000LL, kLong = 600000000LL, kRepeat = 300000000LL;

  void pulse(int dir, int64_t t) {
    spin_ = dir == lastDir_ && t - lastPulse_ < kGap ? spin_ + 1 : 1;
    lastPulse_ = t;
    lastDir_ = dir;
    int steps = 1;
    if (acceleration == "fast") steps = spin_ >= 6 ? 3 : 1;
    else if (acceleration != "off") steps = spin_ >= 12 ? 2 : 1;
    emit(dir > 0 ? Action::Down : Action::Up, steps);
  }

  int64_t lastPulse_ = 0, centerAt_ = 0, heldAt_ = 0, nextRepeat_ = 0;
  int lastDir_ = 0, spin_ = 0;
  bool longFired_ = false;
  Key held_ = Key::None;
};

}  // namespace rondo::shell
