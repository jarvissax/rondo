// N0 test S7: screen off and on, sleep and wake, battery, without Java.
//
// Run with the Java layer stopped (`adb shell stop zygote`). It does what Android's power manager does
// on this earlysuspend kernel and times it:
//
//   1. battery and charger readings from /sys/class/power_supply
//   2. a wakelock (/sys/power/wake_lock), as music playing with the screen off holds
//   3. screen off: backlight 0, then "mem" to /sys/power/state (early suspend), and a check that the
//      display driver really sleeps (MediaTek's vsync wait returns at once while it does)
//   4. wait for a wake key on /dev/input/event0 (Power, or Center, which is wired to the same input)
//   5. screen on: "on" to /sys/power/state, wait_for_fb_wake, backlight back, vsync running again
//
//   s7 [seconds to wait for a key]     default 30

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int64_t now_us() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}

static bool put(const char* path, const char* value) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return false;
  bool ok = write(fd, value, strlen(value)) == ssize_t(strlen(value));
  close(fd);
  return ok;
}

static void get(const char* path, char* out, size_t cap) {
  out[0] = 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return;
  ssize_t n = read(fd, out, cap - 1);
  close(fd);
  out[n > 0 ? n : 0] = 0;
  char* nl = strchr(out, '\n');
  if (nl) *nl = 0;
}

// average interval of a few MediaTek vsync waits, in ms (about 23.7 when awake, near 0 when asleep)
static double vsync_ms(int fd, int count) {
  int64_t t0 = now_us();
  for (int i = 0; i < count; i++) ioctl(fd, 0x5601);
  return (now_us() - t0) / 1000.0 / count;
}

int main(int argc, char** argv) {
  int waitS = argc > 1 ? atoi(argv[1]) : 30;
  char v[64], brightness[16];
  const char* fields[] = {"capacity", "status", "batt_vol", "batt_temp", "BatteryAverageCurrent", "ChargerVoltage", "health"};
  printf("s7: battery:");
  for (const char* f : fields) {
    char p[96];
    snprintf(p, sizeof(p), "/sys/class/power_supply/battery/%s", f);
    get(p, v, sizeof(v));
    printf(" %s=%s", f, v);
  }
  char ac[8], usb[8];
  get("/sys/class/power_supply/ac/online", ac, sizeof(ac));
  get("/sys/class/power_supply/usb/online", usb, sizeof(usb));
  printf("; charger: ac=%s usb=%s\n", ac, usb);

  int vs = open("/dev/mtkfb_vsync", O_RDONLY);
  get("/sys/class/leds/lcd-backlight/brightness", brightness, sizeof(brightness));
  if (!put("/sys/power/wake_lock", "rondo-s7")) printf("s7: can't take a wakelock\n");
  printf("s7: awake: vsync every %.2f ms, backlight %s\n", vsync_ms(vs, 10), brightness);

  int64_t t0 = now_us();
  put("/sys/class/leds/lcd-backlight/brightness", "0");
  put("/sys/power/state", "mem");
  int64_t t1 = now_us();
  // wait_for_fb_sleep blocks until the display driver has gone to sleep (early suspend done)
  char tmp[16];
  get("/sys/power/wait_for_fb_sleep", tmp, sizeof(tmp));
  int64_t t2 = now_us();
  printf("s7: screen off: backlight and \"mem\" in %.1f ms, display driver asleep after %.1f ms; vsync wait now %.2f ms\n",
         (t1 - t0) / 1000.0, (t2 - t0) / 1000.0, vsync_ms(vs, 10));
  fflush(stdout);

  int key = open("/dev/input/event0", O_RDONLY);
  int clk = CLOCK_MONOTONIC;
  ioctl(key, EVIOCSCLOCKID, &clk);
  input_event ev;
  int64_t pressedAt = 0;
  int code = 0;
  int64_t until = now_us() + int64_t(waitS) * 1000000;
  while (!pressedAt && now_us() < until) {
    pollfd p = {key, POLLIN, 0};
    if (poll(&p, 1, 500) <= 0) continue;
    while (read(key, &ev, sizeof(ev)) == sizeof(ev)) {
      if (ev.type == EV_KEY && ev.value == 1 && (ev.code == KEY_POWER || ev.code == KEY_REPLY)) {
        pressedAt = int64_t(ev.time.tv_sec) * 1000000 + ev.time.tv_usec;
        code = ev.code;
        break;
      }
    }
  }
  if (!pressedAt) {
    printf("s7: no wake key within %d s; turning the screen back on\n", waitS);
    pressedAt = now_us();
  }
  int64_t a = now_us();
  put("/sys/power/state", "on");
  get("/sys/power/wait_for_fb_wake", tmp, sizeof(tmp));
  int64_t b = now_us();
  put("/sys/class/leds/lcd-backlight/brightness", brightness[0] ? brightness : "128");
  int64_t c = now_us();
  double awake = vsync_ms(vs, 10);
  printf("s7: %s pressed: read %.1f ms later; display driver awake %.1f ms after the press; backlight on at %.1f ms; vsync every %.2f ms again\n",
         code == KEY_POWER ? "Power" : code == KEY_REPLY ? "Center" : "no key", (a - pressedAt) / 1000.0,
         (b - pressedAt) / 1000.0, (c - pressedAt) / 1000.0, awake);
  put("/sys/power/wake_unlock", "rondo-s7");
  char locks[256];
  get("/sys/power/wake_lock", locks, sizeof(locks));
  printf("s7: wakelocks still held: [%s]\n", locks);
  return 0;
}
