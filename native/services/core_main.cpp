// rondo-core's main: the command loop and the services (core.h).
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <vector>

#include "../player/protocol.h"  // readFully
#include "core.h"
#include "protocol.h"
#include "wifi.h"

namespace rondo {
namespace {

CoreState* g_shm = nullptr;
pthread_mutex_t g_stateMu = PTHREAD_MUTEX_INITIALIZER;  // the services' writes, one at a time

void publish(const std::function<void(CoreState&)>& change) {
  pthread_mutex_lock(&g_stateMu);
  CoreState& s = *g_shm;
  __atomic_add_fetch(&s.seq, 1, __ATOMIC_ACQ_REL);  // odd: being written
  change(s);
  s.serial++;
  __atomic_add_fetch(&s.seq, 1, __ATOMIC_ACQ_REL);  // even: consistent
  pthread_mutex_unlock(&g_stateMu);
}

CoreState snapshot() {
  pthread_mutex_lock(&g_stateMu);
  CoreState s = *g_shm;
  pthread_mutex_unlock(&g_stateMu);
  return s;
}

}  // namespace

void runCore(int sock, CoreState* shm) {
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  prctl(PR_SET_NAME, "rondo-core");
  setpriority(PRIO_PROCESS, 0, 5);  // below the UI and the player: nothing here is in a hurry
  signal(SIGPIPE, SIG_IGN);
  setvbuf(stderr, nullptr, _IOLBF, 0);
  g_shm = shm;
  Wifi wifi(publish);
  wifi.snapshot_ = snapshot;
  wifi.start();
  for (;;) {
    uint32_t head[2];
    if (!readFully(sock, head, sizeof(head)) || head[0] < 4) _exit(0);  // the UI is gone
    std::vector<uint8_t> payload(head[0] - 4);
    if (!payload.empty() && !readFully(sock, payload.data(), payload.size())) _exit(0);
    const uint8_t* p = payload.data();
    size_t len = payload.size();
    auto i32 = [&] {
      int32_t v = 0;
      if (len >= 4) memcpy(&v, p, 4);
      return v;
    };
    switch (head[1]) {
      case kWifiEnable: wifi.enable(len && p[0]); break;
      case kWifiScan: wifi.scan(); break;
      case kWifiConnect: wifi.connect(i32()); break;
      case kWifiJoin: {
        if (len < 1) break;
        std::string rest(reinterpret_cast<const char*>(p + 1), len - 1);
        size_t z = rest.find('\0');
        wifi.join(rest.substr(0, z), WifiSecurity(p[0]), z == std::string::npos ? "" : rest.substr(z + 1));
        break;
      }
      case kWifiForget: wifi.forget(i32()); break;
      default: break;
    }
  }
}

}  // namespace rondo
