// Monotonic time in nanoseconds, the one clock every part of Rondo measures with.
#pragma once
#include <stdint.h>
#include <time.h>

namespace rondo {

inline int64_t now_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

constexpr int64_t kMs = 1000000;
constexpr int64_t kSec = 1000000000LL;

}  // namespace rondo
