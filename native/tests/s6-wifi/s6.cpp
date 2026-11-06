// N0 test S6: Wi-Fi without Java.
//
// Run with the Java layer stopped (`adb shell stop zygote`). It does what Android's Wi-Fi service does,
// from our own process, and times every step:
//
//   1. cold: stop wpa_supplicant and power the combo chip's Wi-Fi off (wifi_unload_driver)
//   2. load the driver (MediaTek's libhardware_legacy asks netd: "hal load wifi"), start wpa_supplicant
//      for wlan0 only, connect to its control socket
//   3. scan, and let wpa_supplicant join a saved network (/data/misc/wifi/wpa_supplicant.conf)
//   4. DHCP (libnetutils, which starts the dhcpcd_wlan0 service), then DNS through netd
//   5. resolve a host and fetch / from it over plain HTTP
//
//   s6 [host]        default example.com

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cutils/properties.h>

extern "C" {
#include <hardware_legacy/wifi.h>
#include <netutils/dhcp.h>
#include <netutils/ifc.h>
}

static int64_t now_ms() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

static int64_t T0;
static void step(const char* what) { printf("s6: %6lld ms  %s\n", (long long)(now_ms() - T0), what); fflush(stdout); }

static bool cmd(const char* c, char* reply, size_t cap) {
  size_t len = cap - 1;
  if (wifi_command("wlan0", c, reply, &len) != 0) return false;
  reply[len] = 0;
  return true;
}

// waits for a wpa_supplicant event containing `want`, up to `ms`
static bool wait_event(const char* want, int ms) {
  char buf[512];
  int64_t until = now_ms() + ms;
  while (now_ms() < until) {
    int n = wifi_wait_for_event("wlan0", buf, sizeof(buf) - 1);
    if (n <= 0) continue;
    buf[n] = 0;
    if (strstr(buf, want)) return true;
  }
  return false;
}

static bool daemon(const char* socketName, const char* line, bool numbered);
static bool netd(const char* line) { return daemon("netd", line, true); }
// MediaTek's hald loads and unloads the Wi-Fi and p2p drivers ("hal load wifi", "hal load p2p"); it
// takes no sequence numbers
static bool hald(const char* line) { return daemon("hald", line, false); }

// one netd command over its socket, the protocol `ndc` speaks: a NUL-terminated line, then a reply
static bool daemon(const char* socketName, const char* line, bool numberIt) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un a = {};
  a.sun_family = AF_UNIX;
  snprintf(a.sun_path, sizeof(a.sun_path), "/dev/socket/%s", socketName);
  if (connect(s, (sockaddr*)&a, sizeof(a)) < 0) {
    close(s);
    return false;
  }
  static int seq = 1;  // 4.2's netd wants a sequence number before each command
  char numbered[300];
  int n0 = numberIt ? snprintf(numbered, sizeof(numbered), "%d %s", seq++, line)
                    : snprintf(numbered, sizeof(numbered), "%s", line);
  write(s, numbered, n0 + 1);
  char reply[256] = {0};
  bool ok = false;
  for (int i = 0; i < 4; i++) {  // netd may send broadcast lines (6xx) before the answer
    ssize_t n = read(s, reply, sizeof(reply) - 1);
    if (n <= 0) break;
    reply[n] = 0;
    if (reply[0] == '2') { ok = true; break; }
    if (reply[0] == '5' || reply[0] == '4') break;
  }
  printf("s6:           %s \"%s\" -> %.60s\n", socketName, line, reply);
  close(s);
  return ok;
}

int main(int argc, char** argv) {
  const char* host = argc > 1 ? argv[1] : "example.com";
  char reply[4096];
  T0 = now_ms();

  wifi_close_supplicant_connection("wlan0");
  wifi_stop_supplicant(1);
  wifi_stop_supplicant(0);
  dhcp_stop("wlan0");
  wifi_unload_driver();
  step("Wi-Fi off (supplicant stopped, driver unloaded)");
  sleep(1);

  T0 = now_ms();
  if (wifi_load_driver() != 0) { step("wifi_load_driver failed"); return 1; }
  step("driver loaded, chip powered");
  // MediaTek's wpa_supplicant crashes (SIGSEGV) when started for wlan0 alone, so it runs the way
  // Android starts it: as p2p_supplicant, with wlan0 and p2p0. wifi_load_driver only brings wlan0 back
  // ("load wifi" through MediaTek's hald), so p2p0 is loaded the same way first.
  if (!hald("hal load p2p")) { step("hald load p2p failed"); return 1; }
  step("p2p interface loaded");
  if (wifi_start_supplicant(1) != 0) { step("wifi_start_supplicant failed"); return 1; }
  step("wpa_supplicant started");
  int64_t giveUp = now_ms() + 10000;
  while (wifi_connect_to_supplicant("wlan0") != 0) {
    if (now_ms() > giveUp) { step("can't reach wpa_supplicant's control socket in 10 s"); return 1; }
    usleep(100000);
  }
  step("connected to wpa_supplicant");

  if (cmd("LIST_NETWORKS", reply, sizeof(reply))) {
    int saved = 0;
    for (char* p = strchr(reply, '\n'); p && p[1]; p = strchr(p + 1, '\n')) saved++;
    printf("s6:           %d saved network(s)\n", saved);
  }
  cmd("ENABLE_NETWORK all", reply, sizeof(reply));
  cmd("SCAN", reply, sizeof(reply));
  if (wait_event("CTRL-EVENT-SCAN-RESULTS", 10000)) {
    int seen = 0;
    if (cmd("SCAN_RESULTS", reply, sizeof(reply)))
      for (char* p = strchr(reply, '\n'); p && p[1]; p = strchr(p + 1, '\n')) seen++;
    char what[64];
    snprintf(what, sizeof(what), "scan done, %d networks in range", seen);
    step(what);
  } else {
    step("no scan results within 10 s");
  }
  cmd("RECONNECT", reply, sizeof(reply));
  if (!wait_event("CTRL-EVENT-CONNECTED", 20000)) { step("not associated within 20 s"); return 1; }
  step("associated");

  char ip[PROPERTY_VALUE_MAX], gw[PROPERTY_VALUE_MAX], dns1[PROPERTY_VALUE_MAX], dns2[PROPERTY_VALUE_MAX],
      server[PROPERTY_VALUE_MAX], vendor[PROPERTY_VALUE_MAX];
  uint32_t prefix = 0, lease = 0;
  if (dhcp_do_request("wlan0", ip, gw, &prefix, dns1, dns2, server, &lease, vendor) != 0) {
    step("DHCP failed");
    printf("s6:           %s\n", dhcp_get_errmsg());
    return 1;
  }
  step("DHCP lease");
  // the address and default route as Android's connectivity service sets them, and DNS through netd
  ifc_init();
  ifc_configure("wlan0", inet_addr(ip), prefix, inet_addr(gw), inet_addr(dns1), dns2[0] ? inet_addr(dns2) : 0);
  ifc_close();
  char line[256];
  snprintf(line, sizeof(line), "resolver setifdns wlan0 \"\" %s %s", dns1, dns2);
  netd(line);
  netd("resolver setdefaultif wlan0");
  step("address, route and DNS set");

  addrinfo hints = {}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, "80", &hints, &res) != 0 || !res) { step("DNS lookup failed"); return 1; }
  step("DNS lookup");
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(s, res->ai_addr, res->ai_addrlen) != 0) { step("TCP connect failed"); return 1; }
  step("TCP connected");
  snprintf(line, sizeof(line), "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: Rondo-s6\r\n\r\n", host);
  write(s, line, strlen(line));
  char head[128] = {0};
  read(s, head, sizeof(head) - 1);
  char* eol = strpbrk(head, "\r\n");
  if (eol) *eol = 0;
  char what[160];
  snprintf(what, sizeof(what), "HTTP answer: %s", head);
  step(what);
  close(s);
  freeaddrinfo(res);
  return 0;
}
