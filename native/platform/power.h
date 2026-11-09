// The Y1's power: what Android's PowerManagerService and BatteryService did for 1.0.x, from sysfs (S7).
//
// Screen off is two stages: the backlight off and no drawing (a key brings it back at once), then early
// suspend ("mem" to /sys/power/state), whose handlers include the display: the kernel waits for the
// drawer to stop (it reads /sys/power/wait_for_fb_wake once wait_for_fb_sleep has returned, and that
// read blocks until late resume lets drawing start again). With no wakelock held the kernel then
// suspends fully; a Power press wakes it ("on"), the panel back 0.54 s later.
#pragma once
#include <stdint.h>

namespace rondo::power {

void setBacklight(int value);  // 0-255
// Early suspend on or off. While on, nothing may be drawn: drawingAllowed() says when it may again.
void suspend(bool on);
bool drawingAllowed();
int wakeFd();  // readable when drawing is allowed again after a suspend (poll it with the input)
// The wakelock that keeps the CPU running with the screen off (playing, the radio, a transfer).
void hold(bool on);

struct Battery {
  int level = 100;     // %
  bool plugged = false;  // a charger or a computer
  int tempTenths = 250;  // 0.1 °C
};
Battery battery();

int headset();    // the wired jack (h2w): 0 nothing, 1 a headset, 2 headphones
void shutdown();  // init's sys.powerctl
// The CPU (both cores of the Cortex-A7). After a cold boot the Y1 keeps the second core off and the
// clock pinned at 1.0 GHz until Android's framework says otherwise (measured Nov 9): without it,
// cpuSetup() lets MediaTek's hotplug governor bring the second core in under load and scale the clock
// (598 MHz idle, 1.0 GHz busy), and stops USB pinning the top clock. cpuBoost(true) holds the clock
// at the top for motion, video and scans (no ramp-up in the first frames of a slide); false gives the
// governor back its say. cpuBothCores(true) keeps the second core in: the governor
// taking it out or back costs the whole system 10 to 25 ms (a missed refresh in a slide, Nov 9), so
// that happens only with the screen off.
void cpuSetup();
void cpuBoost(bool on);
void cpuBothCores(bool on);
void reboot();

}  // namespace rondo::power
