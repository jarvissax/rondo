// N0 test S1, part 2: the panel's real refresh, timed from MediaTek's hardware vsync.
//
// MediaTek's hardware composer (hwcomposer.mt6572.so, VSyncThread::threadLoop) waits for the panel
// with ioctl(open("/dev/mtkfb_vsync"), 0x5601), which returns at the next vsync. This times that
// wait, so the panel's rate can be compared with the vsync SurfaceFlinger hands out. Read-only.
//
//   hwvsync [count]     default 300

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

static int64_t now_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

int main(int argc, char** argv) {
  int count = argc > 1 ? atoi(argv[1]) : 300;
  int fd = open("/dev/mtkfb_vsync", O_RDONLY);
  if (fd < 0) {
    perror("hwvsync: /dev/mtkfb_vsync");
    return 1;
  }
  std::vector<int64_t> gaps;
  int64_t last = 0;
  for (int i = 0; i <= count; i++) {
    if (ioctl(fd, 0x5601) < 0) {
      perror("hwvsync: ioctl 0x5601");
      return 1;
    }
    int64_t t = now_ns();
    if (last) gaps.push_back(t - last);
    last = t;
  }
  std::sort(gaps.begin(), gaps.end());
  auto at = [&](double p) { return gaps[std::min(gaps.size() - 1, size_t(p * (gaps.size() - 1)))] / 1e6; };
  double sum = 0;
  for (int64_t g : gaps) sum += g;
  printf("hwvsync: %zu intervals: mean %.3f ms (%.2f Hz), p1 %.3f, p50 %.3f, p99 %.3f ms\n", gaps.size(),
         sum / gaps.size() / 1e6, 1e9 / (sum / gaps.size()), at(0.01), at(0.5), at(0.99));
  close(fd);
  return 0;
}
