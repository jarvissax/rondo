// rondo-core's main: the command loop and the services (core.h).
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <time.h>

#include <atomic>
#include <deque>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include "../core/api.h"

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

// Two threads fetch the server's API, each with its own libcurl handle (its connection kept open);
// their replies go back on the socket, one whole message at a time.
class ApiWorkers {
 public:
  explicit ApiWorkers(int sock) : sock_(sock) {
    // libcurl's global setup isn't thread-safe: both handles are made here, before the threads
    for (auto& a : apis_) a.reset(new Api());
    for (int i = 0; i < 2; i++) {
      pthread_t t;
      starting_ = i;
      pthread_create(&t, nullptr, run, this);
      pthread_detach(t);
      while (starting_ >= 0) usleep(1000);  // each thread takes its handle first
    }
  }
  void configure(const std::string& base, const std::string& token) {
    pthread_mutex_lock(&mu_);
    base_ = base, token_ = token, generation_++;
    cache_.clear();
    pthread_mutex_unlock(&mu_);
  }
  void get(uint32_t id, int ttl, std::string path) {
    pthread_mutex_lock(&mu_);
    jobs_.push_back({id, ttl, std::move(path)});
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
  }

 private:
  struct Job {
    uint32_t id;
    int ttl;
    std::string path;
  };
  struct Cached {
    std::string body;
    int status = 0;
    int64_t at = 0;
  };
  static void* run(void* self) {
    static_cast<ApiWorkers*>(self)->loop();
    return nullptr;
  }
  void loop() {
    Api& api = *apis_[starting_];
    starting_ = -1;
    int generation = -1;
    for (;;) {
      pthread_mutex_lock(&mu_);
      while (jobs_.empty()) pthread_cond_wait(&cv_, &mu_);
      Job j = jobs_.front();
      jobs_.pop_front();
      if (generation != generation_) generation = generation_, api.configure(base_, token_);
      auto hit = cache_.find(j.path);
      bool fresh = j.ttl > 0 && hit != cache_.end() && nowMs() - hit->second.at < j.ttl;
      Cached c = fresh ? hit->second : Cached{};
      pthread_mutex_unlock(&mu_);
      if (!fresh) {
        c.status = api.get(j.path, c.body);
        c.at = nowMs();
        if (c.status == 200) {
          pthread_mutex_lock(&mu_);
          if (cache_.size() > 64) cache_.clear();  // metadata only, and little of it
          cache_[j.path] = c;
          pthread_mutex_unlock(&mu_);
        }
      }
      reply(j.id, c.status, c.body);
    }
  }
  void reply(uint32_t id, int status, const std::string& body) {
    std::string m(16, '\0');
    uint32_t len = uint32_t(12 + body.size()), type = kApiReply;
    int32_t st = status;
    memcpy(&m[0], &len, 4);
    memcpy(&m[4], &type, 4);
    memcpy(&m[8], &id, 4);
    memcpy(&m[12], &st, 4);
    m += body;
    pthread_mutex_lock(&writeMu_);
    writeFully(sock_, m.data(), m.size());
    pthread_mutex_unlock(&writeMu_);
  }
  static int64_t nowMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
  }
  int sock_;
  std::unique_ptr<Api> apis_[2];
  std::atomic<int> starting_{-1};
  pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER, writeMu_ = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cv_ = PTHREAD_COND_INITIALIZER;
  std::deque<Job> jobs_;
  std::map<std::string, Cached> cache_;
  std::string base_, token_;
  int generation_ = 0;
};

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
  ApiWorkers api(sock);
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
      case kApiConfigure: {
        std::string s(reinterpret_cast<const char*>(p), len);
        size_t z = s.find('\0');
        api.configure(s.substr(0, z), z == std::string::npos ? "" : s.substr(z + 1));
        break;
      }
      case kApiGet: {
        if (len < 8) break;
        uint32_t id, ttl;
        memcpy(&id, p, 4);
        memcpy(&ttl, p + 4, 4);
        api.get(id, int(ttl), std::string(reinterpret_cast<const char*>(p + 8), len - 8));
        break;
      }
      default: break;
    }
  }
}

}  // namespace rondo
