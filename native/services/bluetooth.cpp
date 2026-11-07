// Bluetooth (bluetooth.h): one thread owns the sockets and MediaTek's A2DP library (which isn't
// thread-safe), runs the commands in order and answers mtkbt's events. The message layouts are
// libandroid_runtime.so's (MediaTek's BluetoothService.cpp and BluetoothEventLoop.cpp, read with REA):
//
//   power on 0x64 -> 0x91 (result +0x20, nonzero: on); power off 0x65 -> 0x92
//   search 0x67 -> results 0x93 (address +0x20, RSSI +0x44, class +0x48, name length +0x50, name +0x51),
//     names later as 0x94 (the same layout), the end as 0x95; cancel 0x68 -> 0x96
//   pair 0x7e (address +0x20) -> a PIN request 0xa3, answered 0000 (0x75), or an SSP confirmation
//     0xa4, accepted (0x76), as 1.0.x did; the result as 0xab (result +0x20, address +0x24)
//   forget 0x7f (address +0x20) -> 0xad; a device's name 0xc2 (address +0x20, length +0x28, name +0x29)
#include "bluetooth.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <set>

#include <yyjson.h>

#include "../platform/aosp_begin.h"
#include <binder/ProcessState.h>
#include <media/AudioSystem.h>
#include "../platform/aosp_end.h"

// libmtkbtextadpa2dp.so, as BluetoothA2dpService.cpp calls it (S3)
extern "C" {
void btmtk_a2dp_setSockAddress(void* addr, int len);
void btmtk_a2dp_send_activate_req(int replySock, int apiSock);
void btmtk_a2dp_send_stream_open_req(int index, void* addr, int local);
int btmtk_a2dp_handle_message(void* ilm);
void btmtk_a2dp_close_device(void* addr);
void btmtk_a2dp_deactive_req();
void btmtk_a2dp_get_bdaddr(void* out);
}

namespace rondo {
namespace {

constexpr size_t kIlm = 0x81c;  // an ILM message buffer, as MediaTek's code allocates it
const char* kMtkbt = "/dev/socket/bt.int.adp";
const char* kDevDb = "/data/@btmtk/devdb";
const char* kCodDb = "/data/@btmtk/cod";

int64_t nowMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}
uint32_t u32(const uint8_t* m, size_t o) {
  uint32_t v;
  memcpy(&v, m + o, 4);
  return v;
}
uint16_t u16(const uint8_t* m, size_t o) {
  uint16_t v;
  memcpy(&v, m + o, 2);
  return v;
}

// bt_addr_struct printed the usual way, NAP:UAP:LAP
std::string addrString(const uint8_t* a) {
  uint32_t lap = u32(a, 0);
  uint16_t nap = u16(a, 6);
  char s[18];
  snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", nap >> 8, nap & 0xff, a[4], (lap >> 16) & 0xff, (lap >> 8) & 0xff,
           lap & 0xff);
  return s;
}
bool parseAddr(const std::string& s, uint8_t out[8]) {
  unsigned b[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
  memset(out, 0, 8);
  uint32_t lap = (b[3] << 16) | (b[4] << 8) | b[5];
  memcpy(out, &lap, 4);
  out[4] = uint8_t(b[2]);
  uint16_t nap = uint16_t((b[0] << 8) | b[1]);
  memcpy(out + 6, &nap, 2);
  return true;
}
// mtkbt's files keep an address as six bytes: the LAP's three, the UAP, the NAP's two
void fromFile(const uint8_t* b, uint8_t out[8]) {
  memset(out, 0, 8);
  uint32_t lap = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16;
  memcpy(out, &lap, 4);
  out[4] = b[3];
  uint16_t nap = uint16_t(b[4] | b[5] << 8);
  memcpy(out + 6, &nap, 2);
}
bool sameAddr(const uint8_t* a, const uint8_t* b) {
  return (u32(a, 0) & 0xffffff) == (u32(b, 0) & 0xffffff) && a[4] == b[4] && u16(a, 6) == u16(b, 6);
}

std::vector<uint8_t> readFile(const char* path) {
  std::vector<uint8_t> d;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return d;
  uint8_t buf[4096];
  for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) d.insert(d.end(), buf, buf + n);
  close(fd);
  return d;
}

// The abstract datagram socket mtkbt sends a profile's replies to (socket_local_server(name, ABSTRACT))
int abstractSocket(const char* name, sockaddr_un* me, socklen_t* len) {
  int s = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (s < 0) return -1;
  memset(me, 0, sizeof(*me));
  me->sun_family = AF_UNIX;
  memcpy(me->sun_path + 1, name, strlen(name));
  *len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + strlen(name));
  if (bind(s, (sockaddr*)me, *len) != 0) {
    fprintf(stderr, "rondo-core: can't bind %s (%s)\n", name, strerror(errno));
    close(s);
    return -1;
  }
  fcntl(s, F_SETFD, FD_CLOEXEC);
  return s;
}
// A socket of our own, connected to mtkbt, for a profile's requests (auto-bound, as MtkBt's)
int apiSocket() {
  int s = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (s < 0) return -1;
  timeval tv = {5, 0};
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_un autobind = {};
  autobind.sun_family = AF_UNIX;
  bind(s, (sockaddr*)&autobind, sizeof(sa_family_t));
  sockaddr_un to = {};
  to.sun_family = AF_UNIX;
  strcpy(to.sun_path, kMtkbt);
  if (connect(s, (sockaddr*)&to, sizeof(to)) != 0) {
    close(s);
    return -1;
  }
  fcntl(s, F_SETFD, FD_CLOEXEC);
  return s;
}

// bt_sendmsg's header fields
void header(uint8_t* m, uint32_t id, uint16_t payload) {
  memcpy(m + 0x04, &id, 4);
  uint32_t two = 2, zero = 0;
  memcpy(m + 0x0c, &two, 4);
  memcpy(m + 0x10, &zero, 4);
  uint16_t one = 1;
  memcpy(m + 0x1c, &one, 2);
  memcpy(m + 0x1e, &payload, 2);
}

void copyText(char* out, size_t cap, const std::string& s) {
  size_t n = std::min(cap - 1, s.size());
  memcpy(out, s.data(), n);
  out[n] = 0;
}

}  // namespace

// ---- commands: each runs on the Bluetooth thread ----

void Bluetooth::post(std::function<void()> job) {
  pthread_mutex_lock(&mu_);
  if (!started_) {
    started_ = true;
    if (pipe(wake_) == 0) {
      fcntl(wake_[0], F_SETFL, O_NONBLOCK);
      fcntl(wake_[0], F_SETFD, FD_CLOEXEC);
      fcntl(wake_[1], F_SETFD, FD_CLOEXEC);
    }
    pthread_t t;
    pthread_create(&t, nullptr, run, this);
    pthread_detach(t);
  }
  jobs_.push_back(std::move(job));
  pthread_mutex_unlock(&mu_);
  char one = 1;
  (void)!write(wake_[1], &one, 1);
}

void Bluetooth::configure(const std::string& stateFile) {
  post([this, stateFile] {
    stateFile_ = stateFile;
    load();
    readPaired();
    publish();
  });
}

void Bluetooth::power(bool on) {
  post([this, on] {
    if (!open()) {
      power_ = -1;
      publish();
      return;
    }
    static uint8_t m[kIlm];
    memset(m, 0, kIlm);
    if (on) {
      if (power_ == 1 || power_ == 2) return;
      power_ = 1;
      powerSince_ = nowMs();
      publish();
      sendGap(0x64, m, 0);
    } else {
      if (power_ == 0 || power_ == 3) return;
      stopSearch();
      for (Dev& d : devs_)
        if (d.a2dp) btmtk_a2dp_close_device(d.addr);
      if (!routed_.empty()) route(false, routed_);
      if (a2dpActive_) btmtk_a2dp_deactive_req(), a2dpActive_ = false;
      power_ = 3;
      powerSince_ = nowMs();
      publish();
      sendGap(0x65, m, 0);
    }
  });
}

void Bluetooth::search(bool on) {
  post([this, on] {
    wantSearch_ = on;
    if (on && power_ == 2 && !searching_) startSearch();
    if (!on) stopSearch();
    publish();
  });
}

void Bluetooth::pair(const std::string& address) {
  post([this, address] {
    uint8_t a[8];
    if (power_ != 2 || !parseAddr(address, a)) return;
    stopSearch();
    Dev& d = add(a);
    d.pairing = true;
    static uint8_t m[kIlm];
    memset(m, 0, kIlm);
    memcpy(m + 0x20, a, 8);
    sendGap(0x7e, m, 0x88);
    publish();
  });
}

void Bluetooth::connect(const std::string& address) {
  post([this, address] {
    uint8_t a[8];
    if (power_ != 2 || !parseAddr(address, a)) return;
    stopSearch();  // a search stalls connecting
    // only one pair of headphones at a time
    for (Dev& d : devs_) {
      if (d.a2dp && !sameAddr(d.addr, a)) {
        pendingConnect_ = address;
        btmtk_a2dp_close_device(d.addr);
        publish();
        return;
      }
    }
    Dev& d = add(a);
    if (d.a2dp == 2) return;
    d.a2dp = 1;
    connectSince_ = nowMs();
    btmtk_a2dp_send_stream_open_req(0, d.addr, 1);
    publish();
  });
}

void Bluetooth::disconnect(const std::string& address) {
  post([this, address] {
    uint8_t a[8];
    if (!parseAddr(address, a)) return;
    if (pendingConnect_ == address) pendingConnect_.clear();
    btmtk_a2dp_close_device(a);
  });
}

void Bluetooth::forget(const std::string& address) {
  post([this, address] {
    uint8_t a[8];
    if (!parseAddr(address, a)) return;
    if (Dev* d = find(a))
      if (d->a2dp) btmtk_a2dp_close_device(a);
    if (last_ == address) last_.clear(), save();
    static uint8_t m[kIlm];
    memset(m, 0, kIlm);
    memcpy(m + 0x20, a, 8);
    sendGap(0x7f, m, 0xc);
  });
}

// ---- the thread ----

void* Bluetooth::run(void* self) {
  static_cast<Bluetooth*>(self)->loop();
  return nullptr;
}

void Bluetooth::loop() {
  static uint8_t m[kIlm];
  for (;;) {
    pollfd p[4] = {{wake_[0], POLLIN, 0}, {gap_, POLLIN, 0}, {a2dp_, POLLIN, 0}, {avrcp_, POLLIN, 0}};
    poll(p, 4, 1000);
    if (p[0].revents & POLLIN) {
      char buf[64];
      while (read(wake_[0], buf, sizeof(buf)) > 0) {
      }
      for (;;) {
        pthread_mutex_lock(&mu_);
        if (jobs_.empty()) {
          pthread_mutex_unlock(&mu_);
          break;
        }
        std::function<void()> job = std::move(jobs_.front());
        jobs_.pop_front();
        pthread_mutex_unlock(&mu_);
        job();
      }
    }
    ssize_t n;
    if (gap_ >= 0 && (p[1].revents & POLLIN) && (n = recv(gap_, m, kIlm, MSG_DONTWAIT)) >= 8) onGap(m, n);
    if (a2dp_ >= 0 && (p[2].revents & POLLIN) && (n = recv(a2dp_, m, kIlm, MSG_DONTWAIT)) >= 8) onA2dp(m, n);
    if (avrcp_ >= 0 && (p[3].revents & POLLIN) && (n = recv(avrcp_, m, kIlm, MSG_DONTWAIT)) >= 8) onAvrcp(m, n);
    // what never answered: a power change (15 s, as 1.0.x's switch waits), a search, a pairing, a connection
    int64_t now = nowMs();
    bool changed = false;
    if ((power_ == 1 || power_ == 3) && now - powerSince_ > 15000) power_ = power_ == 1 ? 0 : 2, changed = true;
    if (searching_ && now - searchSince_ > 90000) searching_ = false, changed = true;
    if (autoConnectAt_ && now >= autoConnectAt_) {
      autoConnectAt_ = 0;
      bool any = std::any_of(devs_.begin(), devs_.end(), [](const Dev& x) { return x.a2dp != 0; });
      if (!any && power_ == 2) connect(autoConnect_);
    }
    if (connectSince_ && now - connectSince_ > 25000) {
      connectSince_ = 0;
      for (Dev& d : devs_)
        if (d.a2dp == 1) d.a2dp = 0, changed = true;
    }
    if (changed) publish();
  }
}

bool Bluetooth::open() {
  if (gap_ >= 0) return true;
  sockaddr_un me;
  socklen_t len;
  gap_ = abstractSocket("bt.ext.adp", &me, &len);
  if (gap_ < 0) return false;
  a2dp_ = abstractSocket("bt.ext.adp.a2dp", &me, &len);
  if (a2dp_ >= 0) btmtk_a2dp_setSockAddress(&me, int(len));
  a2dpApi_ = apiSocket();
  avrcp_ = abstractSocket("bt.ext.adp.avrcp", &me, &len);
  avrcpApi_ = apiSocket();
  // the audio policy (an A2DP output) is reached over binder
  android::ProcessState::self()->startThreadPool();
  return true;
}

void Bluetooth::sendGap(uint32_t id, uint8_t* m, uint16_t payload) {
  header(m, id, payload);
  sockaddr_un to = {};
  to.sun_family = AF_UNIX;
  strcpy(to.sun_path, kMtkbt);
  if (sendto(gap_, m, payload + 0x1c, 0, (sockaddr*)&to, sizeof(to)) < 0)
    fprintf(stderr, "rondo-core: Bluetooth message 0x%x not sent (%s)\n", id, strerror(errno));
}

void Bluetooth::sendAvrcp(uint32_t id, uint8_t* m, uint16_t payload) {
  if (avrcpApi_ < 0) return;
  header(m, id, payload);
  send(avrcpApi_, m, payload + 0x1c, 0);
}

void Bluetooth::onGap(uint8_t* m, ssize_t n) {
  static uint8_t r[kIlm];
  uint32_t id = u32(m, 4);
  if (getenv("RONDO_BT_LOG")) fprintf(stderr, "bt: gap 0x%x (%zd bytes) +0x20=%u\n", id, n, m[0x20]);
  switch (id) {
    case 0x91:  // powered on: the profiles, the paired devices, the headphones last used
      if (m[0x20]) {
        power_ = 2;
        if (!a2dpActive_ && a2dp_ >= 0 && a2dpApi_ >= 0) btmtk_a2dp_send_activate_req(a2dp_, a2dpApi_), a2dpActive_ = true;
        if (!avrcpActive_ && avrcpApi_ >= 0) {
          memset(r, 0, kIlm);
          r[0x22] = 14;  // AVRCP 1.4, as MtkBt activates it
          r[0x23] = 1;   // SDP features
          sendAvrcp(500, r, 8);
          avrcpActive_ = true;
        }
        readPaired();
        if (!last_.empty()) {
          std::string a = last_;
          connect(a);
        }
        if (wantSearch_) startSearch();
      } else {
        power_ = 0;
      }
      publish();
      break;
    case 0x92:  // powered off
      power_ = 0;
      links_ = 0;
      searching_ = false;
      a2dpActive_ = avrcpActive_ = false;
      for (Dev& d : devs_) d.found = d.pairing = false, d.a2dp = 0;
      devs_.erase(std::remove_if(devs_.begin(), devs_.end(), [](const Dev& d) { return !d.paired; }), devs_.end());
      publish();
      break;
    case 0x93:  // a device found
    case 0x94: {  // its name, later
      if (n < 0x52 || m[0x50] >= 60) break;
      Dev& d = add(m + 0x20);
      d.found = true;
      if (id == 0x93) d.rssi = int8_t(m[0x44]), d.cod = u32(m, 0x48) & 0xffffff;
      if (m[0x50]) learnName(m + 0x20, reinterpret_cast<const char*>(m + 0x51), m[0x50]);
      publish();
      break;
    }
    case 0x95:  // the search ended (or couldn't start: one was still running, which a cancel ends)
      if (m[0x20] && nowMs() - searchSince_ < 2000 && wantSearch_) {
        memset(r, 0, kIlm);
        sendGap(0x68, r, 0);
        cancels_++;
        startSearch();
        break;
      }
      searching_ = false;
      publish();
      break;
    case 0x96:  // a cancel done
      if (cancels_ > 0) cancels_--;
      break;
    case 0xa3:  // a PIN: headphones that ask for one almost always use 0000
      memset(r, 0, kIlm);
      memcpy(r + 0x20, m + 0x20, 8);
      r[0x28] = 4;
      memcpy(r + 0x29, "0000", 4);
      sendGap(0x75, r, 0x20);
      break;
    case 0xa4:  // a numeric comparison: accepted (both screens' numbers can't be shown side by side)
      memset(r, 0, kIlm);
      memcpy(r + 0x20, m + 0x20, 8);
      r[0x28] = 1;
      sendGap(0x76, r, 0x10);
      break;
    case 0xab: {  // paired (result 0), or refused or timed out: once paired, connected
      Dev* d = find(m + 0x24);
      if (d) d->pairing = false;
      if (m[0x20] == 0) {
        readPaired();
        std::string a = addrString(m + 0x24);
        connect(a);
      }
      publish();
      break;
    }
    case 0xad:  // forgotten
      readPaired();
      publish();
      break;
    case 0xba: {  // the number of links changed (+0x21, the device at +0x24)
      int links = m[0x21];
      bool up = links > links_;
      links_ = links;
      // paired headphones came on and linked: connected unless they connect A2DP themselves within 2 s
      // (Android's A2DP auto-connect)
      Dev* d = find(m + 0x24);
      bool any = std::any_of(devs_.begin(), devs_.end(), [](const Dev& x) { return x.a2dp != 0; });
      if (up && d && d->paired && (d->cod & 0x1f00) == 0x0400 && !any && power_ == 2) {
        autoConnect_ = d->address;
        autoConnectAt_ = nowMs() + 2000;
      }
      break;
    }
    case 0xc2:  // a device's name
      if (n >= 0x29 + m[0x28] && m[0x28] < 60) learnName(m + 0x20, reinterpret_cast<const char*>(m + 0x29), m[0x28]), publish();
      break;
    default: break;
  }
}

void Bluetooth::onA2dp(uint8_t* m, ssize_t n) {
  int s = btmtk_a2dp_handle_message(m);  // 1 disconnected, 2 connecting, 3 connected, 4 playing
  if (getenv("RONDO_BT_LOG")) fprintf(stderr, "bt: a2dp 0x%x (%zd bytes) -> %d\n", u32(m, 4), n, s);
  if (s >= 1 && s <= 4) a2dpState(s);
}

void Bluetooth::a2dpState(int state) {
  bool connected = state >= 3, was = a2dpStateNow_ >= 3;
  int before = a2dpStateNow_;
  a2dpStateNow_ = state;
  if (connected && !was) {
    uint8_t a[8];
    btmtk_a2dp_get_bdaddr(a);
    Dev& d = add(a);
    std::string address = d.address;
    for (Dev& o : devs_) o.a2dp = &o == &d ? 2 : 0;
    connectSince_ = 0;
    if (!routed_.empty() && routed_ != address) route(false, routed_);
    if (routed_.empty()) route(true, address);
    if (last_ != address) last_ = address, save();
    publish();
  } else if (state == 1 && before != 1) {
    if (!routed_.empty()) route(false, routed_);
    for (Dev& o : devs_) o.a2dp = 0;
    publish();
    if (!pendingConnect_.empty()) {
      std::string a = pendingConnect_;
      pendingConnect_.clear();
      connect(a);
    }
  }
}

// The music to the headphones, as Android's audio service did on every A2DP connection: the output made
// available to the audio policy, and "force use for media" none (an old no-A2DP kept music on the jack).
// The headphones start at the volume music had (Android kept one per output; without its audio service
// the A2DP output would take mediaserver's default, which can be loud).
void Bluetooth::route(bool on, const std::string& address) {
  using android::AudioSystem;
  if (getenv("RONDO_BT_LOG")) fprintf(stderr, "bt: route %s %s\n", on ? "on" : "off", address.c_str());
  if (on) {
    int index = 0;
    AudioSystem::getStreamVolumeIndex(AUDIO_STREAM_MUSIC, &index, AudioSystem::getDevicesForStream(AUDIO_STREAM_MUSIC));
    AudioSystem::setStreamVolumeIndex(AUDIO_STREAM_MUSIC, index, AUDIO_DEVICE_OUT_BLUETOOTH_A2DP);
    AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, AUDIO_POLICY_DEVICE_STATE_AVAILABLE, address.c_str());
    AudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA, AUDIO_POLICY_FORCE_NONE);
    routed_ = address;
  } else {
    AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE, address.c_str());
    routed_.clear();
  }
}

// AVRCP (S3): a headset connecting is accepted; its buttons are passed to the UI
void Bluetooth::onAvrcp(uint8_t* m, ssize_t n) {
  static uint8_t r[kIlm];
  uint32_t id = u32(m, 4);
  if (getenv("RONDO_BT_LOG")) fprintf(stderr, "bt: avrcp 0x%x (%zd bytes) op 0x%02x\n", id, n, n > 0x2b ? m[0x2b] : 0);
  if (id == 0x1fa) {
    memset(r, 0, kIlm);
    r[0x20] = m[0x20];  // the channel
    r[0x21] = 1;        // accepted
    sendAvrcp(0x1fb, r, 8);
  } else if (id == 0x207 && n >= 0x2c) {
    uint16_t len = u16(m, 0x26);
    if (len == 3 && !(m[0x2b] & 0x80)) {  // a pass-through button, pressed
      key_ = m[0x2b] & 0x7f;
      keySerial_++;
      publish();
    }
    memset(r, 0, kIlm);
    r[0x20] = m[0x20];  // channel
    r[0x21] = m[0x21];  // sequence
    r[0x22] = 9;        // ACCEPTED
    r[0x23] = 9;        // the panel subunit
    memcpy(r + 0x26, &len, 2);
    memcpy(r + 0x28, m + 0x28, 2);
    if (len && len <= 200 && n >= 0x2a + len) memcpy(r + 0x2a, m + 0x2a, len);
    sendAvrcp(0x208, r, 0xd6);
  }
}

void Bluetooth::startSearch() {
  if (power_ != 2 || searching_) return;
  // with the fields btmtk_gap_discovery sets: every class, its inquiry length and number of answers
  static uint8_t m[kIlm];
  memset(m, 0, kIlm);
  uint32_t all = 0xffffffff;
  memcpy(m + 0x20, &all, 4);
  m[0x24] = 0x30;
  m[0x25] = 0x19;
  sendGap(0x67, m, 0x18);
  searching_ = true;
  searchSince_ = nowMs();
}

void Bluetooth::stopSearch() {
  if (!searching_) return;
  static uint8_t m[kIlm];
  memset(m, 0, kIlm);
  sendGap(0x68, m, 0);
  cancels_++;
  searching_ = false;
}

// mtkbt's paired devices: devdb's 20 slots of 0x150 bytes after an 8-byte header (an address in a
// slot's first six bytes), and their classes in cod (40 slots of 12 bytes after a 4-byte count)
void Bluetooth::readPaired() {
  std::vector<uint8_t> db = readFile(kDevDb), cod = readFile(kCodDb);
  std::vector<std::array<uint8_t, 8>> paired;
  for (size_t at = 8; at + 0x150 <= db.size(); at += 0x150) {
    bool any = false;
    for (int i = 0; i < 6; i++) any |= db[at + size_t(i)] != 0;
    if (!any) continue;
    std::array<uint8_t, 8> a;
    fromFile(&db[at], a.data());
    paired.push_back(a);
  }
  for (Dev& d : devs_) d.paired = false;
  for (auto& a : paired) add(a.data()).paired = true;
  for (size_t at = 4; at + 12 <= cod.size(); at += 12) {
    uint8_t a[8];
    fromFile(&cod[at], a);
    if (Dev* d = find(a))
      if (!d->cod) d->cod = u32(cod.data(), at + 8) & 0xffffff;
  }
  devs_.erase(std::remove_if(devs_.begin(), devs_.end(), [](const Dev& d) { return !d.paired && !d.found && !d.pairing && !d.a2dp; }),
              devs_.end());
}

Bluetooth::Dev* Bluetooth::find(const uint8_t* addr) {
  for (Dev& d : devs_)
    if (sameAddr(d.addr, addr)) return &d;
  return nullptr;
}

Bluetooth::Dev& Bluetooth::add(const uint8_t* addr) {
  if (Dev* d = find(addr)) return *d;
  Dev d;
  memcpy(d.addr, addr, 8);
  d.address = addrString(addr);
  auto it = names_.find(d.address);
  if (it != names_.end()) d.name = it->second;
  devs_.push_back(d);
  return devs_.back();
}

void Bluetooth::learnName(const uint8_t* addr, const char* name, int len) {
  std::string s(name, size_t(len));
  while (!s.empty() && (s.back() == 0 || s.back() == ' ')) s.pop_back();
  if (s.empty()) return;
  Dev& d = add(addr);
  d.name = s;
  if (names_[d.address] != s) names_[d.address] = s, save();
}

// {"last": "AA:BB:...", "names": {"AA:BB:...": "the name"}}
void Bluetooth::load() {
  yyjson_doc* doc = yyjson_read_file(stateFile_.c_str(), 0, nullptr, nullptr);
  if (!doc) return;
  yyjson_val* root = yyjson_doc_get_root(doc);
  const char* last = yyjson_get_str(yyjson_obj_get(root, "last"));
  if (last) last_ = last;
  yyjson_val* names = yyjson_obj_get(root, "names");
  size_t i, max;
  yyjson_val *k, *v;
  yyjson_obj_foreach(names, i, max, k, v) {
    if (yyjson_is_str(v)) names_[yyjson_get_str(k)] = yyjson_get_str(v);
  }
  yyjson_doc_free(doc);
}

void Bluetooth::save() {
  if (stateFile_.empty()) return;
  yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
  yyjson_mut_val* root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_strcpy(doc, root, "last", last_.c_str());
  yyjson_mut_val* names = yyjson_mut_obj(doc);
  for (auto& n : names_) yyjson_mut_obj_add(names, yyjson_mut_strcpy(doc, n.first.c_str()), yyjson_mut_strcpy(doc, n.second.c_str()));
  yyjson_mut_obj_add_val(doc, root, "names", names);
  std::string tmp = stateFile_ + ".part";
  if (yyjson_mut_write_file(tmp.c_str(), doc, 0, nullptr, nullptr)) rename(tmp.c_str(), stateFile_.c_str());
  yyjson_mut_doc_free(doc);
}

void Bluetooth::publish() {
  int power = power_;
  bool searching = searching_;
  uint32_t serial = keySerial_;
  int key = key_;
  std::vector<BtDevice> list;
  for (const Dev& d : devs_) {
    if (!d.paired && !d.found && !d.pairing && !d.a2dp) continue;
    if (list.size() >= size_t(kMaxBtDevices)) break;
    BtDevice b{};
    copyText(b.address, sizeof(b.address), d.address);
    copyText(b.name, sizeof(b.name), d.name);
    b.cod = d.cod;
    b.rssi = int8_t(d.rssi);
    b.paired = d.paired, b.pairing = d.pairing, b.a2dp = uint8_t(d.a2dp);
    list.push_back(b);
  }
  publish_([&](CoreState& s) {
    s.btPower = power;
    s.btSearching = searching;
    s.btKeySerial = serial;
    s.btKey = key;
    s.btCount = int32_t(list.size());
    for (size_t i = 0; i < list.size(); i++) s.bt[i] = list[i];
  });
}

}  // namespace rondo
