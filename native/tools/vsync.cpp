// vsync: the panel's refresh as MediaTek's hardware vsync reports it (/dev/mtkfb_vsync), for the 60 Hz work
// (private/native-plan.md). Prints the median interval and the rate.
//
//   vsync [count]     default 120 intervals
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

int main(int argc, char** argv) {
  int count = argc > 1 ? atoi(argv[1]) : 120;
  int fd = open("/dev/mtkfb_vsync", O_RDONLY);
  if (fd < 0) {
    perror("vsync: /dev/mtkfb_vsync");
    return 1;
  }
  auto now = [] {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
  };
  ioctl(fd, 0x5601);
  int64_t last = now();
  std::vector<int64_t> d;
  for (int i = 0; i < count; i++) {
    ioctl(fd, 0x5601);
    int64_t t = now();
    d.push_back(t - last);
    last = t;
  }
  std::vector<int64_t> s = d;
  std::sort(s.begin(), s.end());
  double median = s[s.size() / 2] / 1e6;
  printf("vsync: median %.3f ms (%.2f Hz), min %.3f, max %.3f over %d\n", median, 1000 / median, s.front() / 1e6, s.back() / 1e6, count);
  return 0;
}
