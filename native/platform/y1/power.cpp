// The Y1's power (power.h), as S7 measured it.
#include "../power.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <string>

#include <cutils/properties.h>

namespace rondo::power {
namespace {

bool writeText(const char* path, const char* text) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  bool ok = write(fd, text, strlen(text)) == ssize_t(strlen(text));
  close(fd);
  return ok;
}
int readInt(const char* path, int otherwise) {
  char b[32] = {0};
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return otherwise;
  ssize_t n = read(fd, b, sizeof(b) - 1);
  close(fd);
  return n > 0 ? atoi(b) : otherwise;
}

std::atomic<bool> g_drawing{true};
int g_wake = -1;
bool g_held = false;

// The early-suspend handshake: when the kernel says the display is going to sleep, drawing has
// stopped (the screen went dark first), so it is told at once; when late resume is done, drawing may
// start again and the loop is woken.
void* fbWatch(void*) {
  char b[16];
  for (;;) {
    int fd = open("/sys/power/wait_for_fb_sleep", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    (void)!read(fd, b, sizeof(b));  // blocks until early suspend
    close(fd);
    g_drawing = false;
    fd = open("/sys/power/wait_for_fb_wake", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    (void)!read(fd, b, sizeof(b));  // the acknowledgment; blocks until late resume
    close(fd);
    g_drawing = true;
    uint64_t one = 1;
    (void)!write(g_wake, &one, sizeof(one));
  }
}

void startWatch() {
  static bool started = false;
  if (started) return;
  started = true;
  g_wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  pthread_t t;
  pthread_create(&t, nullptr, fbWatch, nullptr);
  pthread_detach(t);
}

}  // namespace

void setBacklight(int value) {
  char b[8];
  snprintf(b, sizeof(b), "%d", value < 0 ? 0 : value > 255 ? 255 : value);
  writeText("/sys/class/leds/lcd-backlight/brightness", b);
}

void suspend(bool on) {
  startWatch();
  writeText("/sys/power/state", on ? "mem" : "on");
}

bool drawingAllowed() { return g_drawing.load(); }

int wakeFd() {
  startWatch();
  return g_wake;
}

void hold(bool on) {
  if (on == g_held) return;
  g_held = on;
  writeText(on ? "/sys/power/wake_lock" : "/sys/power/wake_unlock", "rondo");
}

Battery battery() {
  Battery b;
  b.level = readInt("/sys/class/power_supply/battery/capacity", 100);
  b.plugged = readInt("/sys/class/power_supply/ac/online", 0) || readInt("/sys/class/power_supply/usb/online", 0);
  b.tempTenths = readInt("/sys/class/power_supply/battery/batt_temp", 250);
  return b;
}

int headset() { return readInt("/sys/class/switch/h2w/state", 0); }

namespace {
bool writeFile(const char* path, const char* value) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return false;
  bool ok = write(fd, value, strlen(value)) == ssize_t(strlen(value));
  close(fd);
  return ok;
}
constexpr const char* kHotplug = "/sys/devices/system/cpu/cpufreq/hotplug/";
std::string hp(const char* name) { return std::string(kHotplug) + name; }
bool g_boosted = false;
}  // namespace

void cpuSetup() {
  writeFile(hp("cpu_hotplug_disable").c_str(), "0");      // the second core when there is work
  writeFile("/proc/cpufreq/cpufreq_state", "1");          // DVFS: the clock follows the load
  writeFile("/proc/cpufreq/usb_raise_freq", "0");         // a cable doesn't pin it at the top
  writeFile(hp("cpu_num_base").c_str(), "1");
  writeFile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", "598000");
  g_boosted = false;
}

void cpuBoost(bool on) {
  if (on == g_boosted) return;
  g_boosted = on;
  // the floor at the top step, and both cores in at once (the governor would take 300 ms to add one)
  writeFile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", on ? "1001000" : "598000");
  writeFile(hp("cpu_num_base").c_str(), on ? "2" : "1");
}

void shutdown() {
  sync();
  property_set("sys.powerctl", "shutdown");
}

void reboot() {
  sync();
  property_set("sys.powerctl", "reboot");
}

}  // namespace rondo::power
