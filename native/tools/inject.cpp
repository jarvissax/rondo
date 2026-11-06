// inject: wheel pulses and button presses at real speed, for the perf harness (tools/perf).
//
// sendevent writes one event per process, about 4 pulses a second; a fast spin on the Y1 sends one every
// ~40 ms. This writes whole press/release pairs straight to the input device from one process.
//
//   inject wheel <count> [gap ms] [cw|ccw]     default gap 40, clockwise; on /dev/input/event2
//   inject key <name> [hold ms]                center, menu, play, next, prev, power; default hold 80
//
// The wheel and Menu/Play/Next/Prev are on event2 (mtk-tpd); Center and Power on event0 (mtk-kpd).
// See docs/device/README.md.

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {

void sleep_ms(long ms) {
  timespec t = {ms / 1000, (ms % 1000) * 1000000};
  nanosleep(&t, nullptr);
}

bool emit(int fd, int code, int value) {
  input_event ev[2] = {};
  ev[0].type = EV_KEY;
  ev[0].code = code;
  ev[0].value = value;
  ev[1].type = EV_SYN;
  ev[1].code = SYN_REPORT;
  return write(fd, ev, sizeof(ev)) == ssize_t(sizeof(ev));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && !strcmp(argv[1], "wheel")) {
    int count = atoi(argv[2]);
    long gap = argc > 3 ? atol(argv[3]) : 40;
    int code = argc > 4 && !strcmp(argv[4], "ccw") ? KEY_UP : KEY_DOWN;
    int fd = open("/dev/input/event2", O_WRONLY);
    if (fd < 0) {
      perror("inject: /dev/input/event2");
      return 1;
    }
    for (int i = 0; i < count; i++) {
      // a detent's press and release share one moment on the real wheel
      input_event ev[4] = {};
      ev[0].type = EV_KEY, ev[0].code = code, ev[0].value = 1;
      ev[1].type = EV_SYN;
      ev[2].type = EV_KEY, ev[2].code = code, ev[2].value = 0;
      ev[3].type = EV_SYN;
      if (write(fd, ev, sizeof(ev)) != ssize_t(sizeof(ev))) return 1;
      if (i + 1 < count) sleep_ms(gap);
    }
    return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "key")) {
    struct { const char* name; const char* dev; int code; } keys[] = {
        {"center", "/dev/input/event0", KEY_REPLY}, {"power", "/dev/input/event0", KEY_POWER},
        {"menu", "/dev/input/event2", KEY_BACK},    {"play", "/dev/input/event2", KEY_PLAYPAUSE},
        {"next", "/dev/input/event2", KEY_RIGHT},   {"prev", "/dev/input/event2", KEY_LEFT}};
    long hold = argc > 3 ? atol(argv[3]) : 80;
    for (auto& k : keys) {
      if (strcmp(k.name, argv[2])) continue;
      int fd = open(k.dev, O_WRONLY);
      if (fd < 0) {
        perror("inject");
        return 1;
      }
      emit(fd, k.code, 1);
      sleep_ms(hold);
      emit(fd, k.code, 0);
      return 0;
    }
    fprintf(stderr, "inject: unknown key %s\n", argv[2]);
    return 2;
  }
  fprintf(stderr, "usage: inject wheel <count> [gap ms] [cw|ccw] | inject key <name> [hold ms]\n");
  return 2;
}
