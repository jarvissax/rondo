// CoreHost: the UI's side of rondo-core (core.h).
#include <sys/wait.h>
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
  child_ = pid;
  close(sv[1]);
  sock_ = sv[0];
  return true;
}

void CoreHost::download(const std::string& path, const std::string& file) {
  std::string b = path;
  b += '\0';
  b += file;
  send(kDownload, b.data(), uint32_t(b.size()));
}

void CoreHost::cancelDownload() { send(kDownloadCancel); }

bool CoreHost::alive() {
  if (child_ <= 0) return true;
  int status;
  return waitpid(child_, &status, WNOHANG) != child_;
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
void CoreHost::screen(bool on) {
  uint8_t v = on;
  send(kScreen, &v, 1);
}
void CoreHost::clockConfigure(bool autoTime, bool autoZone) {
  uint8_t v[2] = {uint8_t(autoTime), uint8_t(autoZone)};
  send(kClock, v, 2);
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

void CoreHost::btConfigure(const std::string& stateFile) { send(kBtConfigure, stateFile.data(), uint32_t(stateFile.size())); }
void CoreHost::btPower(bool on) {
  uint8_t v = on;
  send(kBtPower, &v, 1);
}
void CoreHost::btSearch(bool on) {
  uint8_t v = on;
  send(kBtSearch, &v, 1);
}
void CoreHost::btPair(const std::string& a) { send(kBtPair, a.data(), uint32_t(a.size())); }
void CoreHost::btConnect(const std::string& a) { send(kBtConnect, a.data(), uint32_t(a.size())); }
void CoreHost::btDisconnect(const std::string& a) { send(kBtDisconnect, a.data(), uint32_t(a.size())); }
void CoreHost::btForget(const std::string& a) { send(kBtForget, a.data(), uint32_t(a.size())); }

void CoreHost::apiConfigure(const std::string& baseUrl, const std::string& token, const std::string& cacheDir) {
  std::string b = baseUrl + '\0' + token + '\0' + cacheDir;
  send(kApiConfigure, b.data(), uint32_t(b.size()));
}

uint32_t CoreHost::get(const std::string& path, int cacheMs, Reply done) {
  uint32_t id = nextId_++;
  pending_[id] = std::move(done);
  std::string b(8, '\0');
  uint32_t ttl = uint32_t(cacheMs);
  memcpy(&b[0], &id, 4);
  memcpy(&b[4], &ttl, 4);
  b += path;
  send(kApiGet, b.data(), uint32_t(b.size()));
  return id;
}

uint32_t CoreHost::post(const std::string& url, const std::string& json, Reply done) {
  uint32_t id = nextId_++;
  pending_[id] = std::move(done);
  std::string b(4, '\0');
  memcpy(&b[0], &id, 4);
  b += url;
  b += '\0';
  b += json;
  send(kApiPost, b.data(), uint32_t(b.size()));
  return id;
}

void CoreHost::cancel(uint32_t id) { pending_.erase(id); }

bool CoreHost::pump() {
  if (sock_ < 0) return false;
  char buf[16384];
  for (;;) {
    ssize_t n = recv(sock_, buf, sizeof(buf), MSG_DONTWAIT);
    if (n <= 0) break;
    in_.append(buf, size_t(n));
  }
  bool handed = false;
  while (in_.size() >= 8) {
    uint32_t len, type;
    memcpy(&len, in_.data(), 4);
    if (in_.size() < 4 + size_t(len)) break;
    memcpy(&type, in_.data() + 4, 4);
    if (type == kApiReply && len >= 12) {
      uint32_t id;
      int32_t status;
      memcpy(&id, in_.data() + 8, 4);
      memcpy(&status, in_.data() + 12, 4);
      auto it = pending_.find(id);
      if (it != pending_.end()) {
        Reply done = std::move(it->second);
        pending_.erase(it);
        done(status, in_.substr(16, len - 12));
        handed = true;
      }
    }
    in_.erase(0, 4 + size_t(len));
  }
  return handed;
}

void Requests::get(const std::string& path, int cacheMs, CoreHost::Reply done) {
  ids_.push_back(coreHost().get(path, cacheMs, std::move(done)));
}

void Requests::cancelAll() {
  for (uint32_t id : ids_) coreHost().cancel(id);
  ids_.clear();
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
