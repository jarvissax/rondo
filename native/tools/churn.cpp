// churn: allocates, touches and frees 2 MB blocks on two threads, as fast as it can. A load generator
// for tools/perf: shows whether memory-map churn in another process disturbs Rondo's frames.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void* run(void*) {
  for (;;) {
    void* p = malloc(2 << 20);
    memset(p, 1, 2 << 20);
    free(p);
    usleep(2000);
  }
  return nullptr;
}

int main() {
  pthread_t t[2];
  for (auto& x : t) pthread_create(&x, nullptr, run, nullptr);
  pause();
}
