// usb_drive.h on the Y1: vold's command socket and the USB gadget's state.
//
// vold (Android 4.2, MediaTek) takes "<seq> <command>" with a zero byte after it, and answers with
// "<code> <seq> <message>" lines ending in zero bytes; codes 200 and up end a command. The card is
// /storage/sdcard0 (vold's "sdcard2"); its states: 1 idle (unmounted), 4 mounted, 7 shared.
#include "../usb_drive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace rondo::usb {
namespace {

const char* kCard = "/storage/sdcard0";

int64_t nowMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

// One command; the final reply's code (-1: vold unreachable), its lines in `out`.
int vold(const std::string& command, std::string* out = nullptr) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) return -1;
  sockaddr_un a;
  memset(&a, 0, sizeof(a));
  a.sun_family = AF_UNIX;
  snprintf(a.sun_path, sizeof(a.sun_path), "/dev/socket/vold");
  if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    close(s);
    return -1;
  }
  std::string line = "1 " + command;
  if (write(s, line.c_str(), line.size() + 1) != ssize_t(line.size() + 1)) {
    close(s);
    return -1;
  }
  std::string got, msg;
  int code = -1;
  char buf[512];
  int64_t until = nowMs() + 20000;  // an unmount may take vold ten seconds of retries
  while (code < 0 && nowMs() < until) {
    timeval tv{1, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = read(s, buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) continue;
    got.append(buf, size_t(n));
    size_t z;
    while ((z = got.find('\0')) != std::string::npos) {
      std::string m = got.substr(0, z);
      got.erase(0, z + 1);
      int c = atoi(m.c_str());
      if (out) *out += m + "\n";
      // broadcasts (6xx) come in between; a reply with our sequence number ends it
      if (c >= 200 && c < 600 && m.find(" 1 ") != std::string::npos) code = c;
    }
  }
  close(s);
  return code;
}

// the card's vold state (-1: not listed)
int cardState() {
  std::string list;
  if (vold("volume list", &list) != 200) return -1;
  size_t at = list.find(std::string(" ") + kCard + " ");
  if (at == std::string::npos) return -1;
  return atoi(list.c_str() + at + strlen(kCard) + 2);
}

// the paths of the volumes vold lists in a state ("110 <seq> <label> <path> <state>")
std::vector<std::string> volumesIn(int state) {
  std::vector<std::string> paths;
  std::string list;
  if (vold("volume list", &list) != 200) return paths;
  size_t at = 0;
  while (at < list.size()) {
    size_t eol = list.find('\n', at);
    if (eol == std::string::npos) eol = list.size();
    int code, seq, st;
    char label[64], path[128];
    if (sscanf(list.c_str() + at, "%d %d %63s %127s %d", &code, &seq, label, path, &st) == 5 && code == 110 && st == state) paths.push_back(path);
    at = eol + 1;
  }
  return paths;
}

bool waitFor(int state, int ms) {
  int64_t until = nowMs() + ms;
  while (nowMs() < until) {
    if (cardState() == state) return true;
    usleep(250000);
  }
  return cardState() == state;
}

}  // namespace

bool computerConnected() {
  FILE* f = fopen("/sys/class/android_usb/android0/state", "r");
  if (!f) return false;
  char s[32] = {};
  bool ok = fgets(s, sizeof(s), f) && !strncmp(s, "CONFIGURED", 10);
  fclose(f);
  return ok;
}

bool shared() { return cardState() == 7; }

namespace {
// a hand-over to or from the computer in progress: the card passes through idle on its way, which the
// watch leaves alone (on Nov 9's second boot it skipped whenever the cable went to a computer, so a Y1
// started on the Mac's cable never mounted its card)
std::atomic<int> g_switching{0};
struct Switching {
  Switching() { g_switching++; }
  ~Switching() { g_switching--; }
};
}  // namespace

// The watch listens instead of asking: vold tells every client of its socket when a volume changes
// state ("605 Volume sdcard2 /storage/sdcard0 state changed from 0 (No-Media) to 1 (Idle-Unmounted)"),
// as MountService heard it. A volume that becomes idle is mounted (once per change); what is idle when
// the watch connects is mounted then. Nothing wakes while nothing changes (asking every 2 s, as
// this did for a day, woke vold and the CPU all the time).
void watchCard() {
  std::thread([] {
    prctl(PR_SET_NAME, "rondo-card");
    for (;;) {
      int s = socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un a;
      memset(&a, 0, sizeof(a));
      a.sun_family = AF_UNIX;
      snprintf(a.sun_path, sizeof(a.sun_path), "/dev/socket/vold");
      if (s < 0 || connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        if (s >= 0) close(s);
        sleep(2);  // vold not listening yet (boot)
        continue;
      }
      if (!g_switching)
        for (const std::string& v : volumesIn(1)) vold("volume mount " + v);
      std::string got;
      char buf[512];
      for (;;) {
        ssize_t n = read(s, buf, sizeof(buf));
        if (n <= 0) break;
        got.append(buf, size_t(n));
        size_t z;
        while ((z = got.find('\0')) != std::string::npos) {
          std::string m = got.substr(0, z);
          got.erase(0, z + 1);
          if (atoi(m.c_str()) != 605 || m.find(" to 1 (") == std::string::npos || g_switching) continue;
          char label[64], path[128];
          if (sscanf(m.c_str(), "%*d Volume %63s %127s", label, path) == 2) vold(std::string("volume mount ") + path);
        }
      }
      close(s);
      sleep(2);
    }
  }).detach();
}

bool setShared(bool on) {
  Switching busy;
  std::string card = kCard;
  if (on) {
    if (cardState() == 7) return true;
    // unmounted first (vold shares only an idle volume), then given to the computer
    if (cardState() == 4 && vold("volume unmount " + card + " force") != 200 && !waitFor(1, 15000)) return false;
    if (!waitFor(1, 15000)) return false;
    vold("volume share " + card + " ums");
    return waitFor(7, 15000);
  }
  if (cardState() == 4) return true;
  if (cardState() == 7) vold("volume unshare " + card + " ums");
  if (!waitFor(1, 15000)) return false;
  vold("volume mount " + card);
  return waitFor(4, 15000);
}

}  // namespace rondo::usb
