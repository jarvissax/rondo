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

  // An audiobook's chapter (Now Playing sets it): the part of the file scrubbed through, lo to hi ms;
  // false: the whole file. A held Next/Prev then moves at most 10 s, 30 s, then 60 s a repeat.
  std::function<bool(int64_t& lo, int64_t& hi)> range;
  int64_t lo() const { return lo_; }
  int64_t hi() const { return lo_ + duration_; }
  float fraction(int64_t duration) const {
    if (!active) return -1;
    if (spoken_) return duration_ > 0 ? float(target - lo_) / float(duration_) : -1;
    return duration > 0 ? float(target) / duration : -1;
  }
  bool begin() {
    PlayerState s = g_player.state();
    if (s.durationMs <= 0) return false;
    active = true, holding_ = false, moved_ = false;
    lo_ = 0, duration_ = s.durationMs, spoken_ = false;
    int64_t a = 0, b = 0;
    if (range && range(a, b) && b > a) lo_ = a, duration_ = b - a, spoken_ = true;
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
    int64_t step = std::max<int64_t>(int64_t(duration_ * share), 2000);
    if (spoken_) step = std::min<int64_t>(step, repeats_ <= 5 ? 10000 : repeats_ <= 10 ? 30000 : 60000);
    move(dir * step);
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
    target = std::max<int64_t>(lo_, std::min(target + delta, std::max<int64_t>(lo_, lo_ + duration_ - 1000)));
    last_ = now_ns();
  }
  bool holding_ = false, moved_ = false, spoken_ = false;
  int repeats_ = 0;
  int64_t duration_ = 0, last_ = 0, lo_ = 0;
};

// ScrubBubble: the target time large, the change and "center to jump" under it, in the middle
void drawScrubBubble(Env& e, float w, float h, int64_t target, int64_t from);

}  // namespace rondo::shell
