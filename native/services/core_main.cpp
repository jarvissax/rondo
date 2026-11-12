// rondo-core's main: the command loop and the services (core.h).
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <time.h>
#include <cutils/properties.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include "../core/api.h"
#include "../core/clock.h"
#include "../direct/innertube.h"
#include "../direct/server.h"

#include "../player/protocol.h"  // readFully
#include "core.h"
#include "base_rom.h"
#include "bluetooth.h"
#include "clock.h"
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

// A default route (the UI's "online", main.cpp's readOnline): without one a request can only fail, so
// one still queued when the network went is answered at once instead of after curl's timeouts
static bool hasDefaultRoute() {
  FILE* f = fopen("/proc/net/route", "r");
  if (!f) return true;  // can't tell: try
  char line[256];
  bool any = false;
  while (!any && fgets(line, sizeof(line), f)) {
    char iface[32];
    unsigned dest = 1;
    any = sscanf(line, "%31s %x", iface, &dest) == 2 && dest == 0;
  }
  fclose(f);
  return any;
}

// Two threads fetch the server's API, each with its own libcurl handle (its connection kept open);
// their replies go back on the socket, one whole message at a time.
class ApiWorkers {
 public:
  explicit ApiWorkers(int sock) : sock_(sock) {
    // libcurl's global setup isn't thread-safe: both handles are made here, before the threads
    for (auto& a : apis_) a.reset(new Api());
    download_.reset(new Api());  // the downloads' own (a bundle takes minutes: requests go on meanwhile)
    for (int i = 0; i < 2; i++) {
      pthread_t t;
      starting_ = i;
      pthread_create(&t, nullptr, run, this);
      pthread_detach(t);
      while (starting_ >= 0) usleep(1000);  // each thread takes its handle first
    }
  }
  void configure(const std::string& base, const std::string& token, const std::string& diskDir) {
    pthread_mutex_lock(&mu_);
    base_ = base, token_ = token, generation_++;
    cache_.clear();
    disk_ = diskDir;
    if (!disk_.empty()) {
      // the answers on disk are this server's and this token's: another pairing starts it empty
      mkdir(disk_.c_str(), 0700);
      char owner[24];
      snprintf(owner, sizeof(owner), "%016llx", (unsigned long long)fnv(base + '\n' + token));
      std::string mark = disk_ + "/server", had;
      if (FILE* f = fopen(mark.c_str(), "r")) {
        char b[32] = {0};
        if (fgets(b, sizeof(b), f)) had = b;
        fclose(f);
      }
      if (had != owner) {
        if (DIR* d = opendir(disk_.c_str())) {
          while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') unlink((disk_ + "/" + e->d_name).c_str());
          closedir(d);
        }
        int fd = open(mark.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) (void)!write(fd, owner, strlen(owner)), close(fd);
      }
    }
    pthread_mutex_unlock(&mu_);
  }
  void get(uint32_t id, int ttl, std::string path) {
    pthread_mutex_lock(&mu_);
    jobs_.push_back({id, ttl, std::move(path), false, ""});
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
  }
  // A file from the server (Software update's bundle), on a thread of its own; one at a time
  std::atomic<bool> cancelDownload_{false};
  void download(std::string path, std::string file) {
    if (g_shm->downloadState == 1) return;
    cancelDownload_ = false;
    publish([](CoreState& s) { s.downloadState = 1, s.downloadDone = 0, s.downloadTotal = -1, s.downloadStatus = 0, s.downloadSeq++; });
    pthread_mutex_lock(&mu_);
    std::string base = base_, token = token_;
    pthread_mutex_unlock(&mu_);
    struct Job {
      ApiWorkers* self;
      std::string base, token, path, file;
    };
    Job* j = new Job{this, base, token, std::move(path), std::move(file)};
    pthread_t t;
    pthread_create(&t, nullptr, [](void* p) -> void* {
      std::unique_ptr<Job> job(static_cast<Job*>(p));
      prctl(PR_SET_NAME, "rondo-download");
      Api& api = *job->self->download_;
      api.configure(job->base, job->token);
      int status = api.download(job->path, job->file, [](int64_t done, int64_t total) {
        publish([&](CoreState& s) { s.downloadDone = done, s.downloadTotal = total; });
      }, &job->self->cancelDownload_);
      publish([&](CoreState& s) { s.downloadState = status == 200 ? 2 : 3, s.downloadStatus = status; });
      return nullptr;
    }, j);
    pthread_detach(t);
  }
  void post(uint32_t id, std::string url, std::string body) {
    pthread_mutex_lock(&mu_);
    jobs_.push_back({id, 0, std::move(url), true, std::move(body)});
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
  }

 private:
  struct Job {
    uint32_t id;
    int ttl;
    std::string path;  // a POST's: the full URL
    bool post;
    std::string body;
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
      if (j.post) {
        pthread_mutex_unlock(&mu_);
        std::string body;
        int status = hasDefaultRoute() ? api.post(j.path, j.body, body) : 0;
        reply(j.id, status, body);
        continue;
      }
      auto hit = cache_.find(j.path);
      bool fresh = j.ttl > 0 && hit != cache_.end() && nowMs() - hit->second.at < j.ttl;
      Cached c = fresh ? hit->second : Cached{};
      pthread_mutex_unlock(&mu_);
      if (!fresh && replay(j.path, c.body)) {
        c.status = 200;
        fresh = true;
      }
      if (!fresh && j.ttl > 0 && fromDisk(j.path, j.ttl, c.body)) {  // a restart within the cache's time
        c.status = 200;
        fresh = true;
      }
      if (!fresh && !hasDefaultRoute() && !getenv("RONDO_API_REPLAY")) {
        c.status = 0;  // no network (an answer kept in memory or on disk was still given above)
      } else if (!fresh) {
        c.status = api.get(j.path, c.body);
        c.at = nowMs();
        if (c.status == 200) record(j.path, c.body);
        if (c.status == 200) {
          // kept only if the source is still the one it came from: a switch (server or direct) while it
          // was on its way would otherwise show the other source's feeds for the cache's two minutes
          pthread_mutex_lock(&mu_);
          bool same = generation == generation_;
          if (same) {
            if (cache_.size() > 64) cache_.clear();  // metadata only, and little of it
            cache_[j.path] = c;
          }
          pthread_mutex_unlock(&mu_);
          if (same && j.ttl > 0) toDisk(j.path, c.body, generation);
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
  // Development (the parity captures): RONDO_API_RECORD=dir keeps every answer the server gave, and
  // RONDO_API_REPLAY=dir answers from them, so a native screen can be shot again with the data
  // 1.0.x was captured with.
  static std::string fileFor(const char* dir, const std::string& path) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : path) h = (h ^ ch) * 1099511628211ULL;
    std::string name;
    for (char ch : path.substr(0, 80)) name += isalnum((unsigned char)ch) || ch == '-' || ch == '.' ? ch : '_';
    char hex[20];
    snprintf(hex, sizeof(hex), "-%016llx", (unsigned long long)h);
    return std::string(dir) + "/" + name + hex + ".json";
  }
  static bool replay(const std::string& path, std::string& body) {
    static const char* dir = getenv("RONDO_API_REPLAY");
    if (!dir) return false;
    FILE* f = fopen(fileFor(dir, path).c_str(), "rb");
    if (!f) return false;
    body.clear();
    char buf[16384];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) body.append(buf, n);
    fclose(f);
    return true;
  }
  static void record(const std::string& path, const std::string& body) {
    static const char* dir = getenv("RONDO_API_RECORD");
    if (!dir) return;
    if (FILE* f = fopen(fileFor(dir, path).c_str(), "wb")) {
      fwrite(body.data(), 1, body.size(), f);
      fclose(f);
    }
  }
  // The memory cache on disk too (answers only, never media): the same time to live, from the time it
  // was fetched, so a screen opened within it after a restart opens at once, as it would have before.
  static uint64_t fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ULL;
    return h;
  }
  static int64_t wallMs() {
    timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
  }
  std::string diskFile(const std::string& path) {
    pthread_mutex_lock(&mu_);
    std::string dir = disk_;
    pthread_mutex_unlock(&mu_);
    if (dir.empty()) return "";
    char name[24];
    snprintf(name, sizeof(name), "/%016llx", (unsigned long long)fnv(path));
    return dir + name;
  }
  bool fromDisk(const std::string& path, int ttl, std::string& body) {
    std::string file = diskFile(path);
    FILE* f = file.empty() ? nullptr : fopen(file.c_str(), "rb");
    if (!f) return false;
    // the first line: when it was fetched (ms since 1970) and the path it answers
    char head[1100] = {0};
    bool ok = false;
    if (fgets(head, sizeof(head), f)) {
      char* sp = strchr(head, ' ');
      int64_t at = atoll(head);
      std::string p = sp ? std::string(sp + 1) : "";
      if (!p.empty() && p.back() == '\n') p.pop_back();
      int64_t age = wallMs() - at;
      if (p == path && age >= 0 && age < ttl) {
        body.clear();
        char b[16384];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) body.append(b, n);
        ok = !body.empty();
      }
    }
    fclose(f);
    return ok;
  }
  void toDisk(const std::string& path, const std::string& body, int generation) {
    std::string file = diskFile(path);
    if (file.empty() || path.size() > 1000) return;
    std::string tmp = file + ".part";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);  // the account's feeds: this user only
    FILE* f = fd >= 0 ? fdopen(fd, "wb") : nullptr;
    if (!f) {
      if (fd >= 0) close(fd);
      return;
    }
    fprintf(f, "%lld %s\n", (long long)wallMs(), path.c_str());
    bool ok = fwrite(body.data(), 1, body.size(), f) == body.size();
    ok = fclose(f) == 0 && ok;
    // in place only while the source is the same (configure empties the folder under the same lock)
    pthread_mutex_lock(&mu_);
    ok = ok && generation == generation_ && rename(tmp.c_str(), file.c_str()) == 0;
    pthread_mutex_unlock(&mu_);
    if (!ok) {
      unlink(tmp.c_str());
      return;
    }
    // at most 128 answers: the oldest go
    std::string dir = file.substr(0, file.rfind('/'));
    std::vector<std::pair<time_t, std::string>> all;
    if (DIR* d = opendir(dir.c_str())) {
      while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "server")) continue;
        struct stat st;
        std::string p = dir + "/" + e->d_name;
        if (stat(p.c_str(), &st) == 0) all.push_back({st.st_mtime, p});
      }
      closedir(d);
    }
    if (all.size() <= 128) return;
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i + 96 < all.size(); i++) unlink(all[i].second.c_str());
  }
  static int64_t nowMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
  }
  int sock_;
  std::unique_ptr<Api> apis_[2], download_;
  std::atomic<int> starting_{-1};
  pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER, writeMu_ = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cv_ = PTHREAD_COND_INITIALIZER;
  std::deque<Job> jobs_;
  std::map<std::string, Cached> cache_;
  std::string base_, token_, disk_;
  int generation_ = 0;
};

}  // namespace

// The clock and the time zone (clock.h): set from the server once each Wi-Fi connection is up, each
// tried again a minute after a failure
std::string g_clockBase, g_clockToken;
pthread_mutex_t g_clockMu = PTHREAD_MUTEX_INITIALIZER;
Api* g_zoneApi = nullptr;  // made before the thread (libcurl's setup isn't thread-safe)
// Settings › Date & time (kClock): either one off leaves what was set by hand alone; switching one on
// asks the network again at once (g_clockKick)
std::atomic<bool> g_autoTime{true}, g_autoZone{true};
std::atomic<int> g_clockKick{0};
void* clockLoop(void*) {
  prctl(PR_SET_NAME, "rondo-clock");
  uint32_t syncedFor = 0, zonedFor = 0;  // the address of the connection the clock, the zone was set on
  int64_t retryAt = 0, zoneRetryAt = 0;
  int kick = 0;
  for (;;) {
    sleep(5);
    if (kick != g_clockKick) kick = g_clockKick, syncedFor = zonedFor = 0, retryAt = zoneRetryAt = 0;
    uint32_t ip = g_shm->wifiState == 4 ? g_shm->ip : 0;
    if (!ip) {
      syncedFor = zonedFor = 0;
      continue;
    }
    pthread_mutex_lock(&g_clockMu);
    std::string base = g_clockBase, token = g_clockToken;
    pthread_mutex_unlock(&g_clockMu);
    if (base.empty()) continue;
    if (!g_autoTime) syncedFor = ip;  // set by hand: not asked
    if (!g_autoZone) zonedFor = ip;
    if (ip != syncedFor && now_ns() >= retryAt) {
      // direct mode's local server has only this Y1's own clock: YouTube's Date header instead
      bool local = base.compare(0, 17, "http://127.0.0.1:") == 0;
      if (syncClock(local ? "https://www.youtube.com/generate_204" : base + "/healthz")) syncedFor = ip;
      else retryAt = now_ns() + 60 * kSec;
    }
    if (ip != zonedFor && now_ns() >= zoneRetryAt) {
      g_zoneApi->configure(base, token);
      if (timeZoneFromServer(*g_zoneApi)) zonedFor = ip;
      else zoneRetryAt = now_ns() + 60 * kSec;
    }
  }
  return nullptr;
}

void runCore(int sock, CoreState* shm) {
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  prctl(PR_SET_NAME, "rondo-core");
  setpriority(PRIO_PROCESS, 0, 5);  // below the UI and the player: nothing here is in a hurry
  signal(SIGPIPE, SIG_IGN);
  setvbuf(stderr, nullptr, _IOLBF, 0);
  // drmserver: mediaserver's MetadataRetriever (a card video's frame, its artwork) waits for it for good
  // when it isn't running, and the 2.0 firmware's first builds had it disabled (Nov 10: no video had a
  // picture, on the Y1 or the dashboard). Started here too, so firmwares already flashed have it.
  char drm[PROPERTY_VALUE_MAX] = "";
  property_get("init.svc.drm", drm, "");
  if (strcmp(drm, "running") != 0 && property_set("ctl.start", "drm") == 0) fprintf(stderr, "rondo-core: drmserver started (video frames need it)\n");
  // the stock firmware's root backdoors, out of firmwares flashed before 3.0 too (base_rom.h)
  removeBaseRomBackdoors();
  g_shm = shm;
  Wifi wifi(publish);
  wifi.snapshot_ = snapshot;
  wifi.start();
  ApiWorkers api(sock);
  Bluetooth bt(publish);
  // direct mode's local server (direct/server.h): always listening, used when Rondo is in direct mode
  if (!direct::serve()) fprintf(stderr, "rondo-core: direct mode's local server didn't start\n");
  timeZoneDefault();
  g_zoneApi = new Api();
  pthread_t clockThread;
  pthread_create(&clockThread, nullptr, clockLoop, nullptr);
  pthread_detach(clockThread);
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
      case kScreen: wifi.screen(len && p[0]); break;
      case kClock: {
        if (len < 2) break;
        bool t = p[0] != 0, z = p[1] != 0;
        bool newlyOn = (t && !g_autoTime) || (z && !g_autoZone);
        g_autoTime = t, g_autoZone = z;
        if (newlyOn) g_clockKick++;
        break;
      }
      case kWifiConnect: wifi.connect(i32()); break;
      case kWifiJoin: {
        if (len < 1) break;
        std::string rest(reinterpret_cast<const char*>(p + 1), len - 1);
        size_t z = rest.find('\0');
        wifi.join(rest.substr(0, z), WifiSecurity(p[0]), z == std::string::npos ? "" : rest.substr(z + 1));
        break;
      }
      case kWifiForget: wifi.forget(i32()); break;
      case kBtConfigure: bt.configure(std::string(reinterpret_cast<const char*>(p), len)); break;
      case kBtPower: bt.power(len && p[0]); break;
      case kBtSearch: bt.search(len && p[0]); break;
      case kBtPair: bt.pair(std::string(reinterpret_cast<const char*>(p), len)); break;
      case kBtConnect: bt.connect(std::string(reinterpret_cast<const char*>(p), len)); break;
      case kBtDisconnect: bt.disconnect(std::string(reinterpret_cast<const char*>(p), len)); break;
      case kBtForget: bt.forget(std::string(reinterpret_cast<const char*>(p), len)); break;
      case kApiConfigure: {
        // the base URL, a zero byte, the token, a zero byte, the cache's folder
        std::string s(reinterpret_cast<const char*>(p), len);
        size_t z = s.find('\0'), z2 = z == std::string::npos ? z : s.find('\0', z + 1);
        std::string token = z == std::string::npos ? "" : s.substr(z + 1, z2 == std::string::npos ? std::string::npos : z2 - z - 1);
        std::string cacheDir = z2 == std::string::npos ? "" : s.substr(z2 + 1);
        api.configure(s.substr(0, z), token, cacheDir);
        // direct mode keeps its visitor id and rules beside the prefs (the cache's folder's folder)
        static std::string directData;
        std::string data = cacheDir.empty() ? "" : cacheDir.substr(0, cacheDir.rfind('/'));
        if (!data.empty() && data != directData) directData = data, direct::setDataDir(data);
        pthread_mutex_lock(&g_clockMu);
        g_clockBase = s.substr(0, z), g_clockToken = token;
        pthread_mutex_unlock(&g_clockMu);
        break;
      }
      case kApiPost: {
        if (len < 4) break;
        uint32_t id;
        memcpy(&id, p, 4);
        std::string rest(reinterpret_cast<const char*>(p + 4), len - 4);
        size_t z = rest.find('\0');
        if (z == std::string::npos) break;
        api.post(id, rest.substr(0, z), rest.substr(z + 1));
        break;
      }
      case kDownload: {
        std::string s(reinterpret_cast<const char*>(p), len);
        size_t z = s.find('\0');
        if (z != std::string::npos) api.download(s.substr(0, z), s.substr(z + 1));
        break;
      }
      case kDownloadCancel: api.cancelDownload_ = true; break;
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
