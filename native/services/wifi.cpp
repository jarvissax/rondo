// rondo-core's Wi-Fi: what Android's WifiStateMachine did for 1.0.x, the way N0's test S6 proved it
// (private/native-plan.md): MediaTek's driver through libhardware_legacy and hald, wpa_supplicant's
// control socket for scanning, joining and forgetting, DHCP through libnetutils, the address, route and
// DNS set as Android's connectivity service sets them. Saved networks stay in wpa_supplicant.conf,
// where Android keeps them, so 1.0.x's networks carry over.
#include "../core/clock.h"
#include "wifi.h"

#include <arpa/inet.h>
#include <cutils/properties.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <vector>

extern "C" {
#include <hardware_legacy/wifi.h>
#include <netutils/dhcp.h>
#include <netutils/ifc.h>
// in 4.2's libnetutils, not its header: renews the lease dhcp_do_request got
int dhcp_do_request_renew(const char* ifname, char* ipaddr, char* gateway, uint32_t* prefixLength, char* dns1, char* dns2,
                          char* server, uint32_t* lease, char* vendorInfo);
}

namespace rondo {
namespace {

const char* kIface = "wlan0";

int64_t nowMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

// one command to a daemon's socket (netd numbers its commands; MediaTek's hald does not)
bool daemonCommand(const char* name, const std::string& line, bool numbered) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) return false;
  sockaddr_un a;
  memset(&a, 0, sizeof(a));
  a.sun_family = AF_UNIX;
  snprintf(a.sun_path, sizeof(a.sun_path), "/dev/socket/%s", name);
  if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    close(s);
    return false;
  }
  static int seq = 1;
  std::string l = numbered ? std::to_string(seq++) + " " + line : line;
  bool ok = write(s, l.c_str(), l.size() + 1) == ssize_t(l.size() + 1);
  char reply[256];
  for (int i = 0; ok && i < 4; i++) {  // broadcast lines (6xx) may come before the answer
    timeval tv{3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = read(s, reply, sizeof(reply) - 1);
    if (n <= 0) {
      ok = false;
      break;
    }
    reply[n] = 0;
    if (reply[0] == '2') break;
    if (reply[0] == '4' || reply[0] == '5') ok = false;
  }
  close(s);
  return ok;
}
bool netd(const std::string& line) { return daemonCommand("netd", line, true); }
bool hald(const std::string& line) { return daemonCommand("hald", line, false); }

std::string field(const std::string& reply, const char* key) {  // key=value lines
  std::string k = std::string(key) + "=";
  size_t at = 0;
  while (at < reply.size()) {
    size_t end = reply.find('\n', at);
    if (end == std::string::npos) end = reply.size();
    if (reply.compare(at, k.size(), k) == 0) return reply.substr(at + k.size(), end - at - k.size());
    at = end + 1;
  }
  return "";
}

// SSIDs in scan results escape bytes as \xNN (and \\, \"): back to UTF-8
std::string unescape(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      if (s[i + 1] == 'x' && i + 3 < s.size()) {
        o += char(strtol(s.substr(i + 2, 2).c_str(), nullptr, 16));
        i += 3;
        continue;
      }
      o += s[++i];
      continue;
    }
    o += s[i];
  }
  return o;
}

// a name or password for SET_NETWORK: quoted, or as hex when it holds a quote or a control character
std::string quoteForSupplicant(const std::string& s) {
  bool plain = std::none_of(s.begin(), s.end(), [](char c) { return c == '"' || (unsigned char)c < 0x20; });
  if (plain) return "\"" + s + "\"";
  std::string hex;
  char b[3];
  for (unsigned char c : s) snprintf(b, sizeof(b), "%02x", c), hex += b;
  return hex;
}

void copyName(char* dst, const std::string& s) {
  snprintf(dst, 33, "%s", s.c_str());
}

}  // namespace

Wifi::Wifi(Publish publish) : publish_(std::move(publish)) {}

void Wifi::post(std::function<void()> job) {
  pthread_mutex_lock(&mu_);
  jobs_.push_back(std::move(job));
  pthread_cond_signal(&cv_);
  pthread_mutex_unlock(&mu_);
}

void Wifi::start() {
  pthread_t t;
  pthread_create(&t, nullptr, [](void* self) -> void* {
    static_cast<Wifi*>(self)->workLoop();
    return nullptr;
  }, this);
  pthread_detach(t);
  post([this] {
    // the supplicant still running (Android left Wi-Fi on): take over; else off until the UI says
    // (it sends the switch as it was left: prefs wifiOn)
    if (wifi_connect_to_supplicant(kIface) == 0) {
      attached();
    } else {
      update([](CoreState& s) { s.wifiEnabled = 0; });
    }
  });
}

void Wifi::update(const std::function<void(CoreState&)>& f) { publish_(f); }

bool Wifi::command(const std::string& c, std::string* reply) {
  char buf[8192];
  size_t len = sizeof(buf) - 1;
  if (wifi_command(kIface, c.c_str(), buf, &len) != 0) return false;
  buf[len] = 0;
  if (reply) *reply = buf;
  return strncmp(buf, "FAIL", 4) != 0;
}

void Wifi::workLoop() {
  for (;;) {
    std::function<void()> job;
    pthread_mutex_lock(&mu_);
    while (jobs_.empty()) {
      // every 3 s while connected and the screen is on: the signal and the speed; every 60 s with it
      // off, the route and the DHCP lease (renewed halfway)
      if (waitUntil(&cv_, &mu_, now_ns() + (screenOn_ ? 3 : 60) * kSec) != 0) break;
    }
    if (!jobs_.empty()) {
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    pthread_mutex_unlock(&mu_);
    if (job) job();
    else poll();
  }
}

// the driver loaded, the supplicant started and reached
void Wifi::turnOn() {
  update([](CoreState& s) { s.wifiEnabled = 1; });
  bool ok = wifi_load_driver() == 0;
  // MediaTek's supplicant crashes started for wlan0 alone: it runs as Android runs it, p2p_supplicant,
  // which needs p2p0 loaded first (S6)
  if (ok) ok = hald("hal load p2p");
  if (ok) ok = wifi_start_supplicant(1) == 0;
  int64_t giveUp = nowMs() + 10000;
  while (ok && wifi_connect_to_supplicant(kIface) != 0) {
    if (nowMs() > giveUp) ok = false;
    else usleep(100000);
  }
  if (!ok) {
    fprintf(stderr, "rondo-core: Wi-Fi didn't start\n");
    update([](CoreState& s) { s.wifiEnabled = 0; });
    return;
  }
  attached();
  command("ENABLE_NETWORK all");
  command("RECONNECT");
}

void Wifi::attached() {
  update([](CoreState& s) { s.wifiEnabled = 2; });
  if (!monitoring_) {
    monitoring_ = true;
    pthread_t t;
    pthread_create(&t, nullptr, [](void* self) -> void* {
      static_cast<Wifi*>(self)->eventLoop();
      return nullptr;
    }, this);
    pthread_detach(t);
  }
  readSaved();
  driverSetup();
  // where the supplicant is now: joined (with an address or still needing one), or not
  std::string st;
  command("STATUS", &st);
  std::string wpa = field(st, "wpa_state"), ssid = field(st, "ssid");
  if (wpa == "COMPLETED") {
    update([&](CoreState& s) { copyName(s.ssid, ssid); });
    char leaseIp[PROPERTY_VALUE_MAX];
    property_get("dhcp.wlan0.ipaddress", leaseIp, "");
    if (!field(st, "ip_address").empty() && field(st, "ip_address") == leaseIp && applyLease()) {
      // joined with an address before Rondo started (Android, or an earlier run): the lease dhcpcd
      // keeps in properties applied again (the route may be gone), and renewed from now on
    } else if (!field(st, "ip_address").empty()) {
      char dns1[PROPERTY_VALUE_MAX], dns2[PROPERTY_VALUE_MAX];
      property_get("dhcp.wlan0.dns1", dns1, "");
      property_get("dhcp.wlan0.dns2", dns2, "");
      if (dns1[0]) setDns(dns1, dns2);
      update([&](CoreState& s) {
        s.wifiState = 4;
        s.ip = inet_addr(field(st, "ip_address").c_str());
      });
    } else {
      getAddress();
    }
  }
  command("SCAN");
}

void Wifi::turnOff() {
  update([](CoreState& s) { s.wifiEnabled = 3; });
  dhcp_stop(kIface);
  netd(std::string("interface clearaddrs ") + kIface);
  wifi_close_supplicant_connection(kIface);
  wifi_stop_supplicant(1);
  wifi_unload_driver();
  monitoring_ = false;
  update([](CoreState& s) {
    s.wifiEnabled = 0;
    s.wifiState = 0;
    s.ssid[0] = 0;
    s.ip = 0;
    s.networkCount = 0;
  });
}

void Wifi::eventLoop() {
  char buf[1024];
  while (monitoring_) {
    int n = wifi_wait_for_event(kIface, buf, sizeof(buf) - 1);
    if (n <= 0) {
      usleep(100000);
      continue;
    }
    buf[n] = 0;
    std::string e = buf;
    if (e.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) {
      post([this] { readScanResults(); });
    } else if (e.find("CTRL-EVENT-STATE-CHANGE") != std::string::npos) {
      // wpa_state: 4 authenticating, 5 associating, 6 associated, 7-8 the key handshakes, 9 completed
      size_t at = e.find("state=");
      int st = at == std::string::npos ? -1 : atoi(e.c_str() + at + 6);
      size_t s = e.find(" SSID=");  // (not BSSID=)
      std::string ssid = s == std::string::npos ? "" : unescape(e.substr(s + 6));
      if (!ssid.empty()) eventSsid_ = ssid;
      post([this, st, ssid] {
        if (st >= 4 && st <= 8)
          update([&](CoreState& c) {
            c.wifiState = st == 4 || st >= 7 ? 2 : 1;  // Android's AUTHENTICATING: 4, 7 and 8
            if (!ssid.empty()) copyName(c.ssid, ssid);
          });
      });
    } else if (e.find("CTRL-EVENT-CONNECTED") != std::string::npos) {
      // MediaTek's supplicant sends "CONNECTED (reauth)" again on every STATUS command: asking STATUS
      // here looped. The name comes from the state change just before; a reauth of the network already
      // connected changes nothing.
      std::string ssid = eventSsid_;
      CoreState now0 = snapshot_();
      if (e.find("(reauth)") != std::string::npos && now0.wifiState == 4 && now0.ip && ssid == now0.ssid) continue;
      post([this, ssid] {
        // the same network joined again (a re-association, a key renewal): its address stays
        CoreState now = snapshot_();
        bool same = now.wifiState == 4 && now.ip && ssid == now.ssid;
        update([&](CoreState& c) {
          copyName(c.ssid, ssid);
          if (!strcmp(c.failedSsid, c.ssid)) c.failedSsid[0] = 0;
        });
        if (enableAllOnConnect_) {  // a network chosen by hand: the others may join again later
          enableAllOnConnect_ = false;
          command("ENABLE_NETWORK all");
          command("SAVE_CONFIG");
        }
        if (!same) getAddress();
      });
    } else if (e.find("CTRL-EVENT-DISCONNECTED") != std::string::npos) {
      post([this] {
        // a disconnection the supplicant recovers from at once (it reconnects within a second or two)
        // isn't a lost network: look again shortly before giving the address up
        usleep(2000000);
        std::string st;
        command("STATUS", &st);  // (it re-sends CONNECTED, which the reauth check above ignores)
        if (field(st, "wpa_state") == "COMPLETED" && field(st, "ssid") == snapshot_().ssid) return;
        lostAddress();
      });
    } else if (e.find("CTRL-EVENT-SSID-TEMP-DISABLED") != std::string::npos && e.find("WRONG_KEY") != std::string::npos) {
      size_t s = e.find("ssid=\"");
      std::string ssid = s == std::string::npos ? "" : e.substr(s + 6, e.find('"', s + 6) - s - 6);
      post([this, ssid] {
        update([&](CoreState& c) {
          copyName(c.failedSsid, unescape(ssid));
          c.wifiState = 0;
        });
      });
    } else if (e.find("CTRL-EVENT-TERMINATING") != std::string::npos) {
      monitoring_ = false;
      post([this] { update([](CoreState& c) { c.wifiEnabled = 0, c.wifiState = 0, c.ip = 0; }); });
    }
  }
}

// While DHCP runs (a lease or its renewal): out of suspend mode and power save, so the answer isn't
// missed (WifiStateMachine's handlePreDhcpSetup and handlePostDhcpSetup)
struct Wifi::Dhcp {
  Wifi& w;
  explicit Dhcp(Wifi& wifi) : w(wifi) {
    if (!w.screenOn_) w.suspendMode(false);
    w.command("SET ps 0");
  }
  ~Dhcp() {
    w.command("SET ps 1");
    if (!w.screenOn_) w.suspendMode(true);
  }
};

// DHCP (libnetutils starts dhcpcd_wlan0), then the address, route and DNS
void Wifi::getAddress() {
  update([](CoreState& s) { s.wifiState = 3; });
  dhcp_stop(kIface);
  char ip[PROPERTY_VALUE_MAX], gw[PROPERTY_VALUE_MAX], dns1[PROPERTY_VALUE_MAX], dns2[PROPERTY_VALUE_MAX],
      server[PROPERTY_VALUE_MAX], vendor[PROPERTY_VALUE_MAX];
  uint32_t prefix = 0, lease = 0;
  Dhcp awake(*this);
  if (dhcp_do_request(kIface, ip, gw, &prefix, dns1, dns2, server, &lease, vendor) != 0) {
    fprintf(stderr, "rondo-core: DHCP failed: %s\n", dhcp_get_errmsg());
    update([](CoreState& s) { s.wifiState = 1; });
    renewAt_ = 0;
    return;
  }
  configure(ip, gw, prefix, dns1, dns2);
  renewAt_ = nowMs() + int64_t(lease ? lease : 3600) * 500;  // halfway through the lease
}

void Wifi::configure(const char* ip, const char* gw, uint32_t prefix, const char* dns1, const char* dns2) {
  ifc_init();
  ifc_configure(kIface, inet_addr(ip), prefix, inet_addr(gw), inet_addr(dns1), dns2[0] ? inet_addr(dns2) : 0);
  ifc_close();
  setDns(dns1, dns2);
  uint32_t addr = inet_addr(ip);
  update([&](CoreState& s) {
    s.wifiState = 4;
    s.ip = addr;
  });
}

namespace {
// A default route out through wlan0 (/proc/net/route). dhcpcd drops it when the link bounces and puts the
// address back alone (reason REBOOT): the Y1 then had an address and no way to the server (N5 log).
bool hasDefaultRoute() {
  FILE* f = fopen("/proc/net/route", "r");
  if (!f) return true;
  char line[256];
  bool found = false;
  while (fgets(line, sizeof(line), f)) {
    char ifc[32];
    unsigned dest = 1, gw = 0;
    if (sscanf(line, "%31s %x %x", ifc, &dest, &gw) == 3 && dest == 0 && !strcmp(ifc, kIface)) found = true;
  }
  fclose(f);
  return found;
}
}  // namespace

bool Wifi::applyLease() {
  char ip[PROPERTY_VALUE_MAX], gw[PROPERTY_VALUE_MAX], mask[PROPERTY_VALUE_MAX], dns1[PROPERTY_VALUE_MAX],
      dns2[PROPERTY_VALUE_MAX], lease[PROPERTY_VALUE_MAX];
  property_get("dhcp.wlan0.ipaddress", ip, "");
  property_get("dhcp.wlan0.gateway", gw, "");
  property_get("dhcp.wlan0.mask", mask, "255.255.255.0");
  property_get("dhcp.wlan0.dns1", dns1, "");
  property_get("dhcp.wlan0.dns2", dns2, "");
  property_get("dhcp.wlan0.leasetime", lease, "3600");
  if (!ip[0] || !gw[0]) return false;
  configure(ip, gw, uint32_t(__builtin_popcount(ntohl(inet_addr(mask)))), dns1[0] ? dns1 : gw, dns2);
  if (!renewAt_) renewAt_ = nowMs() + int64_t(std::max(60, atoi(lease))) * 500;
  return true;
}

// DNS as Android's ConnectivityService set it: netd's per-interface servers, and the global net.dns
// properties with net.dnschange bumped, which 4.2's resolver reads (without them nothing resolves)
void Wifi::setDns(const char* dns1, const char* dns2) {
  netd(std::string("resolver setifdns ") + kIface + " \"\" " + dns1 + " " + dns2);
  netd(std::string("resolver setdefaultif ") + kIface);
  property_set("net.dns1", dns1);
  property_set("net.dns2", dns2);
  char n[PROPERTY_VALUE_MAX];
  property_get("net.dnschange", n, "0");
  property_set("net.dnschange", std::to_string(atoi(n) + 1).c_str());
}

void Wifi::lostAddress() {
  renewAt_ = 0;
  dhcp_stop(kIface);
  netd(std::string("interface clearaddrs ") + kIface);
  update([](CoreState& s) {
    s.wifiState = 0;
    s.ip = 0;
    s.ssid[0] = 0;
  });
}

void Wifi::poll() {
  CoreState now = snapshot_();
  if (now.wifiEnabled != 2 || now.wifiState != 4) return;
  std::string r;
  int rssi = now.rssi, speed = now.linkSpeed;
  if (!screenOn_) {
    // no signal to show: only the route and the lease
  } else if (command("SIGNAL_POLL", &r) && !field(r, "RSSI").empty()) {
    rssi = atoi(field(r, "RSSI").c_str()), speed = atoi(field(r, "LINKSPEED").c_str());
  } else {
    // MediaTek's supplicant has the older driver commands instead: "<ssid> rssi -45", "LinkSpeed 65"
    rssi = speed = 0;
    if (command("DRIVER RSSI", &r)) {
      size_t at = r.rfind("rssi ");
      if (at != std::string::npos) rssi = atoi(r.c_str() + at + 5);
    }
    if (command("DRIVER LINKSPEED", &r)) {
      size_t at = r.find("LinkSpeed ");
      if (at != std::string::npos) speed = atoi(r.c_str() + at + 10);
    }
    if (!rssi)  // the network's level in the last scan
      for (int i = 0; i < now.networkCount; i++)
        if (!strcmp(now.networks[i].ssid, now.ssid)) rssi = now.networks[i].rssi;
  }
  if (rssi != now.rssi || speed != now.linkSpeed) update([&](CoreState& s) { s.rssi = rssi, s.linkSpeed = speed; });
  if (!hasDefaultRoute()) {
    fprintf(stderr, "rondo-core: no route out: the lease applied again\n");
    applyLease();
  }
  if (renewAt_ && nowMs() > renewAt_) {
    char ip[PROPERTY_VALUE_MAX], gw[PROPERTY_VALUE_MAX], dns1[PROPERTY_VALUE_MAX], dns2[PROPERTY_VALUE_MAX],
        server[PROPERTY_VALUE_MAX], vendor[PROPERTY_VALUE_MAX];
    uint32_t prefix = 0, lease = 0;
    Dhcp awake(*this);
    if (dhcp_do_request_renew(kIface, ip, gw, &prefix, dns1, dns2, server, &lease, vendor) == 0) {
      configure(ip, gw, prefix, dns1, dns2);
      renewAt_ = nowMs() + int64_t(lease ? lease : 3600) * 500;
    } else {
      getAddress();
    }
  }
}

void Wifi::readScanResults() {
  std::string r;
  if (!command("SCAN_RESULTS", &r)) return;
  // bssid / frequency / signal level / flags / ssid, one per line after the header
  std::map<std::string, WifiNetwork> best;
  size_t at = r.find('\n');
  while (at != std::string::npos && at + 1 < r.size()) {
    size_t end = r.find('\n', at + 1);
    std::string line = r.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
    at = end;
    std::string cols[5];
    size_t p = 0;
    for (int i = 0; i < 4; i++) {
      size_t t = line.find('\t', p);
      if (t == std::string::npos) break;
      cols[i] = line.substr(p, t - p);
      p = t + 1;
    }
    cols[4] = line.substr(std::min(p, line.size()));
    std::string ssid = unescape(cols[4]);
    // MediaTek's fake "NVRAM WARNING" network, and hidden networks, are left out
    if (ssid.empty() || ssid.find_first_not_of(' ') == std::string::npos || ssid.compare(0, 13, "NVRAM WARNING") == 0) continue;
    WifiNetwork n{};
    copyName(n.ssid, ssid);
    n.rssi = int16_t(atoi(cols[2].c_str()));
    const std::string& f = cols[3];
    n.security = uint8_t(f.find("EAP") != std::string::npos   ? WifiSecurity::Eap
                         : f.find("PSK") != std::string::npos ? WifiSecurity::Psk
                         : f.find("WEP") != std::string::npos ? WifiSecurity::Wep
                                                              : WifiSecurity::Open);
    auto it = best.find(ssid);
    if (it == best.end() || it->second.rssi < n.rssi) best[ssid] = n;
  }
  std::vector<WifiNetwork> list;
  for (auto& kv : best) list.push_back(kv.second);
  std::sort(list.begin(), list.end(), [](const WifiNetwork& a, const WifiNetwork& b) { return a.rssi > b.rssi; });
  update([&](CoreState& s) {
    s.networkCount = int32_t(std::min<size_t>(list.size(), kMaxWifiNetworks));
    for (int i = 0; i < s.networkCount; i++) s.networks[i] = list[size_t(i)];
  });
}

void Wifi::readSaved() {
  std::string r;
  if (!command("LIST_NETWORKS", &r)) return;
  // network id / ssid / bssid / flags
  std::vector<WifiSaved> saved;
  size_t at = r.find('\n');
  while (at != std::string::npos && at + 1 < r.size()) {
    size_t end = r.find('\n', at + 1);
    std::string line = r.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
    at = end;
    size_t t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    size_t t2 = line.find('\t', t1 + 1);
    WifiSaved s{};
    s.id = atoi(line.c_str());
    copyName(s.ssid, unescape(line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1)));
    saved.push_back(s);
  }
  update([&](CoreState& s) {
    s.savedCount = int32_t(std::min<size_t>(saved.size(), kMaxWifiSaved));
    for (int i = 0; i < s.savedCount; i++) s.saved[i] = saved[size_t(i)];
  });
}

void Wifi::enable(bool on) {
  post([this, on] {
    CoreState s = snapshot_();
    if (on && s.wifiEnabled == 0) turnOn();
    else if (!on && s.wifiEnabled == 2) turnOff();
  });
}

// What WifiStateMachine set once the supplicant was up (SupplicantStartedState, DriverStartedState): the
// supplicant's scan every 15 s while not joined (config_wifi_supplicant_scan_interval; its own default
// is 5 s), IPv4 multicast filtered and IPv6 multicast let through (router advertisements) while the
// driver is in suspend mode, power save on.
void Wifi::driverSetup() {
  command("SCAN_INTERVAL 15");
  command("DRIVER RXFILTER-STOP");
  command("DRIVER RXFILTER-ADD 3");
  command("DRIVER RXFILTER-START");
  command("DRIVER RXFILTER-STOP");
  command("DRIVER RXFILTER-REMOVE 2");
  bool filters = command("DRIVER RXFILTER-START");
  bool ps = command("SET ps 1");
  fprintf(stderr, "rondo-core: Wi-Fi set up: scans every 15 s, multicast filter %s, power save %s\n", filters ? "on" : "refused",
          ps ? "on" : "refused");
  if (!screenOn_) suspendMode(true);
}

// WifiNative.setSuspendOptimizations(): "DRIVER SETSUSPENDMODE 1" with the screen off
void Wifi::suspendMode(bool on) {
  bool ok = command(on ? "DRIVER SETSUSPENDMODE 1" : "DRIVER SETSUSPENDMODE 0");
  static int said = -1;
  if (said != int(ok)) fprintf(stderr, "rondo-core: Wi-Fi suspend mode %s%s\n", on ? "on" : "off", ok ? "" : " (refused)");
  said = int(ok);
}

void Wifi::screen(bool on) {
  post([this, on] {
    if (screenOn_ == on) return;
    screenOn_ = on;
    if (snapshot_().wifiEnabled != 2) return;
    suspendMode(!on);
    if (on) poll();  // the signal now, for the status bar
  });
}

void Wifi::scan() {
  post([this] {
    if (snapshot_().wifiEnabled == 2) command("SCAN");
  });
}

// WifiManager.connect(netId): the network chosen first, its priority raised so it is chosen again
// after a restart; the others are enabled again once it has joined
void Wifi::connect(int id) {
  post([this, id] {
    CoreState s = snapshot_();
    int top = 0;
    for (int i = 0; i < s.savedCount; i++) {
      std::string p;
      command("GET_NETWORK " + std::to_string(s.saved[i].id) + " priority", &p);
      top = std::max(top, atoi(p.c_str()));
    }
    command("SET_NETWORK " + std::to_string(id) + " priority " + std::to_string(top + 1));
    command("ENABLE_NETWORK " + std::to_string(id));
    command("SELECT_NETWORK " + std::to_string(id));
    command("SAVE_CONFIG");
    enableAllOnConnect_ = true;
    update([&](CoreState& c) { c.wifiState = 1; });
    command("RECONNECT");
  });
}

void Wifi::join(const std::string& ssid, WifiSecurity security, const std::string& password) {
  post([this, ssid, security, password] {
    // a new password replaces the saved one
    CoreState s = snapshot_();
    for (int i = 0; i < s.savedCount; i++)
      if (ssid == s.saved[i].ssid) command("REMOVE_NETWORK " + std::to_string(s.saved[i].id));
    std::string r;
    if (!command("ADD_NETWORK", &r)) return;
    std::string id = std::to_string(atoi(r.c_str()));
    command("SET_NETWORK " + id + " ssid " + quoteForSupplicant(ssid));
    if (security == WifiSecurity::Psk) {
      command("SET_NETWORK " + id + " key_mgmt WPA-PSK");
      command("SET_NETWORK " + id + " psk \"" + password + "\"");  // always quoted, as Android does: unquoted means a raw 64-hex key
    } else {
      command("SET_NETWORK " + id + " key_mgmt NONE");
      if (security == WifiSecurity::Wep) {
        // 10 or 26 hex digits are a raw key, anything else an ASCII one
        bool hex = (password.size() == 10 || password.size() == 26) &&
                   password.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
        command("SET_NETWORK " + id + " wep_key0 " + (hex ? password : quoteForSupplicant(password)));
        command("SET_NETWORK " + id + " wep_tx_keyidx 0");
        command("SET_NETWORK " + id + " auth_alg OPEN SHARED");
      }
    }
    update([&](CoreState& c) {
      if (!strcmp(c.failedSsid, ssid.c_str())) c.failedSsid[0] = 0;
    });
    readSaved();
    connect(atoi(id.c_str()));
  });
}

void Wifi::forget(int id) {
  post([this, id] {
    command("REMOVE_NETWORK " + std::to_string(id));
    command("SAVE_CONFIG");
    readSaved();
  });
}

}  // namespace rondo
