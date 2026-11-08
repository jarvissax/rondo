// The UI thread's queue (main_queue.h).
#include "main_queue.h"

#include <errno.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace rondo::shell {
namespace {

struct Work {
  int64_t at;
  uint64_t order;
  std::function<void()> run;
};

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
std::vector<Work> g_work;
uint64_t g_order = 0;
int g_fd = -1;
pthread_t g_ui;

int64_t monoNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

}  // namespace

void mainQueueInit() {
  g_ui = pthread_self();
  if (g_fd < 0) g_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

void postToMain(std::function<void()> work, int64_t delayMs) {
  pthread_mutex_lock(&g_mu);
  g_work.push_back({monoNs() + delayMs * 1000000LL, g_order++, std::move(work)});
  pthread_mutex_unlock(&g_mu);
  uint64_t one = 1;
  if (g_fd >= 0) (void)!write(g_fd, &one, sizeof(one));
}

int mainQueueFd() { return g_fd; }

int64_t mainQueueWakeAt() {
  pthread_mutex_lock(&g_mu);
  int64_t t = 0;
  for (const Work& w : g_work)
    if (!t || w.at < t) t = w.at;
  pthread_mutex_unlock(&g_mu);
  return t;
}

bool runMainQueue() {
  uint64_t n;
  if (g_fd >= 0) (void)!read(g_fd, &n, sizeof(n));
  int64_t now = monoNs();
  std::vector<Work> due;
  pthread_mutex_lock(&g_mu);
  for (size_t i = 0; i < g_work.size();) {
    if (g_work[i].at <= now) {
      due.push_back(std::move(g_work[i]));
      g_work.erase(g_work.begin() + long(i));
    } else {
      i++;
    }
  }
  pthread_mutex_unlock(&g_mu);
  std::sort(due.begin(), due.end(), [](const Work& a, const Work& b) { return a.at != b.at ? a.at < b.at : a.order < b.order; });
  for (Work& w : due) w.run();
  return !due.empty();
}

bool onMain(std::function<void()> work, int timeoutMs) {
  if (pthread_equal(pthread_self(), g_ui)) {
    work();
    return true;
  }
  struct Wait {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    bool done = false;
  };
  auto w = std::make_shared<Wait>();
  postToMain([w, work] {
    work();
    pthread_mutex_lock(&w->mu);
    w->done = true;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
  });
  timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += timeoutMs / 1000;
  until.tv_nsec += long(timeoutMs % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
  pthread_mutex_lock(&w->mu);
  while (!w->done)
    if (pthread_cond_timedwait(&w->cv, &w->mu, &until) == ETIMEDOUT) break;
  bool done = w->done;
  pthread_mutex_unlock(&w->mu);
  return done;
}

}  // namespace rondo::shell
