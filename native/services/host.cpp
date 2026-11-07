// CoreHost: the UI's side of rondo-core (core.h).
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../player/protocol.h"  // readFully, writeFully
#include "core.h"
#include "protocol.h"

namespace rondo {

bool CoreHost::start() {
  void* m = mmap(nullptr, sizeof(CoreState), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return false;
  shm_ = static_cast<CoreState*>(m);
  memset(shm_, 0, sizeof(CoreState));
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    close(sv[0]);
    runCore(sv[1], shm_);
  }
  close(sv[1]);
  sock_ = sv[0];
  return true;
}

CoreState CoreHost::state() const {
  CoreState s;
  if (!shm_) {
    memset(&s, 0, sizeof(s));
    return s;
  }
  for (int tries = 0; tries < 100; tries++) {  // the sequence lock: an even, unchanged count
    uint32_t a = __atomic_load_n(&shm_->seq, __ATOMIC_ACQUIRE);
    if (a & 1) continue;
    memcpy(&s, shm_, sizeof(s));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&shm_->seq, __ATOMIC_ACQUIRE) == a) return s;
  }
  memcpy(&s, shm_, sizeof(s));
  return s;
}

void CoreHost::send(uint32_t type, const void* payload, uint32_t len) {
  if (sock_ < 0) return;
  uint32_t head[2] = {4 + len, type};
  writeFully(sock_, head, sizeof(head));
  if (len) writeFully(sock_, payload, len);
}

void CoreHost::wifiEnable(bool on) {
  uint8_t v = on;
  send(kWifiEnable, &v, 1);
}
void CoreHost::wifiScan() { send(kWifiScan); }
void CoreHost::wifiConnect(int id) {
  int32_t v = id;
  send(kWifiConnect, &v, 4);
}
void CoreHost::wifiJoin(const std::string& ssid, WifiSecurity security, const std::string& password) {
  std::string b(1, char(security));
  b += ssid;
  b += '\0';
  b += password;
  send(kWifiJoin, b.data(), uint32_t(b.size()));
}
void CoreHost::wifiForget(int id) {
  int32_t v = id;
  send(kWifiForget, &v, 4);
}

int wifiLevel(int rssi) {
  // WifiManager.calculateSignalLevel(rssi, 4): -100 dBm and under 0, -55 and over 3, linear between
  if (rssi <= -100) return 0;
  if (rssi >= -55) return 3;
  return (rssi + 100) * 3 / 45;
}

const char* wifiSignal(int rssi) {
  switch (wifiLevel(rssi)) {
    case 3: return "Strong";
    case 2: return "Good";
    case 1: return "Fair";
    default: return "Weak";
  }
}

}  // namespace rondo
