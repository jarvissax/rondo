// Monotonic time in nanoseconds, the one clock every part of Rondo measures with.
#pragma once
#include <pthread.h>
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

// Waits on cv until `deadline` (a now_ns() time) or a signal, on the monotonic clock: setting the
// time (from the internet) moves neither. ETIMEDOUT once the deadline has passed.
inline int waitUntil(pthread_cond_t* cv, pthread_mutex_t* mu, int64_t deadline) {
  timespec t{time_t(deadline / kSec), long(deadline % kSec)};
  return pthread_cond_timedwait_monotonic_np(cv, mu, &t);
}

}  // namespace rondo
