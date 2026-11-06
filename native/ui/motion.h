// Motion: springs and eased tweens, evaluated for the moment a frame will appear (its vsync), so motion
// advances in even steps whatever the drawing cost.
#pragma once
#include <math.h>
#include <stdint.h>

namespace rondo::ui {

// A critically damped spring, integrated exactly, so any time step is stable and it can be retargeted
// on every wheel pulse without a jolt. omega sets the speed: ~95 % of the way in 4.7 / omega seconds.
struct Spring {
  float x = 0, v = 0;

  void snap(float to) {
    x = to;
    v = 0;
  }
  // Returns true while still moving.
  bool step(float target, float dt, float omega) {
    float x0 = x - target;
    if (fabsf(x0) < 0.02f && fabsf(v) < 0.5f) {
      x = target;
      v = 0;
      return false;
    }
    float e = expf(-omega * dt);
    float k = v + omega * x0;
    x = target + (x0 + k * dt) * e;
    v = (v - k * omega * dt) * e;
    return true;
  }
};

inline float easeOutCubic(float t) {
  t = 1 - t;
  return 1 - t * t * t;
}
inline float easeInOutCubic(float t) { return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }
inline float clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }

// A value moving from `from` to `to` over `duration` ns from `start`.
struct Tween {
  int64_t start = 0, duration = 0;
  float from = 0, to = 0;
  float (*ease)(float) = easeOutCubic;

  void begin(float f, float t, int64_t now, int64_t dur, float (*e)(float) = easeOutCubic) {
    from = f, to = t, start = now, duration = dur, ease = e;
  }
  float progress(int64_t now) const { return duration ? clamp01(float(now - start) / float(duration)) : 1; }
  float at(int64_t now) const { return from + (to - from) * ease(progress(now)); }
  bool running(int64_t now) const { return now < start + duration; }
};

}  // namespace rondo::ui
