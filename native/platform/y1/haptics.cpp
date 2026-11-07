// haptics.h on the Y1: the timed_output vibrator Android's VibratorService drives (S7).
#include "../haptics.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace rondo::haptics {

void vibrate(int ms) {
  static int fd = open("/sys/class/timed_output/vibrator/enable", O_WRONLY | O_CLOEXEC);
  if (fd < 0 || ms <= 0) return;
  char b[16];
  int n = snprintf(b, sizeof(b), "%d", ms);
  (void)!pwrite(fd, b, size_t(n), 0);
}

}  // namespace rondo::haptics
