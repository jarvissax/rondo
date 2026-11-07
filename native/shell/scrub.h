// The scrubbing Now Playing and the video player share (Scrubber.kt and ScrubBubble in 1.0.x).
#pragma once
#include "app.h"

namespace rondo::shell {

// Scrubber.kt: seeking that scales with the length. The target is previewed (bubble and marker) and
// playback jumps once: on center, when Next/Prev is let go, or 1.2 s after the last wheel turn. Menu
// cancels.
class Scrubber {
 public:
  bool active = false;
  int64_t target = 0, from = 0;  // ms

  float fraction(int64_t duration) const { return active && duration > 0 ? float(target) / duration : -1; }
  bool begin() {
    PlayerState s = g_player.state();
    if (s.durationMs <= 0) return false;
    active = true, holding_ = false, moved_ = false;
    duration_ = s.durationMs;
    from = target = s.positionMs;
    last_ = now_ns();
    return true;
  }
  void turn(int dir, int steps) {
    if (!active && !begin()) return;
    int64_t step = std::max<int64_t>(1000, std::min<int64_t>(60000, duration_ / 150)) * (steps > 1 ? 4 : 1);
    move(dir * step);
  }
  void jump(int dir) {
    if (!active && !begin()) return;
    move(dir * std::max<int64_t>(duration_ / 10, 5000));
  }
  void hold(int dir) {  // one repeat of a held Next/Prev
    if (!holding_) {
      if (!active && !begin()) return;
      holding_ = true;
      repeats_ = 0;
    }
    repeats_++;
    double share = repeats_ <= 5 ? 0.01 : repeats_ <= 10 ? 0.02 : 0.04;
    move(dir * std::max<int64_t>(int64_t(duration_ * share), 2000));
  }
  void commit() {
    if (!active) return;
    if (moved_) g_player.seekTo(target);
    active = holding_ = false;
  }
  void cancel() { active = holding_ = false; }
  // jumps after the wheel has rested (not while a button is held); an unused scrub closes after 4 s
  int64_t deadline() const { return active && !holding_ ? last_ + (moved_ ? 1200 : 4000) * kMs : 0; }
  bool tick(int64_t now) {
    int64_t d = deadline();
    if (d && now >= d) {
      commit();
      return true;
    }
    return false;
  }

 private:
  void move(int64_t delta) {
    moved_ = true;
    target = std::max<int64_t>(0, std::min(target + delta, std::max<int64_t>(0, duration_ - 1000)));
    last_ = now_ns();
  }
  bool holding_ = false, moved_ = false;
  int repeats_ = 0;
  int64_t duration_ = 0, last_ = 0;
};

// ScrubBubble: the target time large, the change and "center to jump" under it, in the middle
void drawScrubBubble(Env& e, float w, float h, int64_t target, int64_t from);

}  // namespace rondo::shell
