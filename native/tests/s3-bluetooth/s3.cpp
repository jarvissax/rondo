// N0 test S3: Bluetooth without Java, talking to MediaTek's mtkbt directly.
//
// Run with the Java layer stopped (`adb shell stop zygote`). MediaTek's stack on this firmware is its own
// (not BlueZ, not Bluedroid): the mtkbt daemon takes "ILM" messages as datagrams on
// /dev/socket/bt.int.adp and sends every reply and event to the abstract socket "bt.ext.adp", which
// Android's Bluetooth service normally binds. This binds it instead and, with the message layouts read
// from libandroid_runtime.so (MediaTek's BluetoothService.cpp and BluetoothEventLoop.cpp, with REA):
//
//   1. power on      MSG 0x64, answered by 0x91 (MSG_ID_BT_POWERON_CNF, result at +0x20)
//   2. own address   MSG 0x6f, answered by 0x9d (MSG_ID_BT_BM_READ_LOCAL_ADDR_CNF, address at +0x24)
//   3. discovery     MSG 0x67, then results as 0x93 (MSG_ID_BT_BM_DISCOVERY_RESULT_IND): address at
//                    +0x20, RSSI +0x44, class +0x48, name length +0x50 and name +0x51
//   4. pairing (with a name): discovery until that device shows up, cancel (0x68, answered by 0x96),
//      bond (0x7e, address at +0x20), then answer what the device asks: a PIN request (0xa3) with
//      0000 (0x75: address, length +0x28, PIN +0x29) or an SSP confirmation (0xa4, its number at
//      +0x28) with accept (0x76: address, accept +0x28), as today's Rondo does; the result arrives as
//      0xab (MSG_ID_BT_BM_BONDING_CNF, result +0x20, 0 = paired)
//
//   s3 [seconds of discovery] [name to pair with]      default 15

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <set>

namespace {

constexpr size_t kIlm = 0x81c;  // an ILM message buffer, as MediaTek's code allocates it

int64_t now_ms() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

uint32_t u32(const uint8_t* m, size_t off) { uint32_t v; memcpy(&v, m + off, 4); return v; }
uint16_t u16(const uint8_t* m, size_t off) { uint16_t v; memcpy(&v, m + off, 2); return v; }

// MediaTek's bt_addr_struct: LAP (3 bytes in a u32), UAP, NAP; printed the usual way, NAP:UAP:LAP
void addr_string(const uint8_t* a, char* out) {
  uint32_t lap = u32(a, 0);
  uint8_t uap = a[4];
  uint16_t nap = u16(a, 6);
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", nap >> 8, nap & 0xff, uap, (lap >> 16) & 0xff,
           (lap >> 8) & 0xff, lap & 0xff);
}

int g_sock = -1;

// bt_sendmsg (libandroid_runtime.so): the header fields it sets, sent to mtkbt's socket
bool send_ilm(uint32_t id, uint8_t* m, uint16_t payload) {
  memcpy(m + 0x04, &id, 4);
  uint32_t two = 2, zero = 0;
  memcpy(m + 0x0c, &two, 4);
  memcpy(m + 0x10, &zero, 4);
  uint16_t one = 1;
  memcpy(m + 0x1c, &one, 2);
  memcpy(m + 0x1e, &payload, 2);
  sockaddr_un to = {};
  to.sun_family = AF_UNIX;
  strcpy(to.sun_path, "/dev/socket/bt.int.adp");
  ssize_t n = sendto(g_sock, m, payload + 0x1c, 0, (sockaddr*)&to, sizeof(to));
  if (n < 0) printf("s3: send 0x%x failed: %s\n", id, strerror(errno));
  return n >= 0;
}

// reads messages until one with `id` arrives or `ms` pass; every other one is printed and dropped
std::set<uint32_t> g_seen;
bool wait_ilm(uint32_t id, uint8_t* m, int ms) {
  int64_t until = now_ms() + ms;
  while (now_ms() < until) {
    pollfd p = {g_sock, POLLIN, 0};
    if (poll(&p, 1, int(until - now_ms())) <= 0) break;
    ssize_t n = recv(g_sock, m, kIlm, 0);
    if (n < 8) continue;
    uint32_t got = u32(m, 4);
    if (got == id) return true;
    if (g_seen.insert(got).second) printf("s3:   (event 0x%x, %zd bytes)\n", got, n);
  }
  return false;
}

int pair(const uint8_t* target, const char* name, uint8_t* m) {
  memset(m, 0, kIlm);
  send_ilm(0x68, m, 0);  // stop discovering first
  wait_ilm(0x96, m, 5000);
  int64_t t0 = now_ms();
  memset(m, 0, kIlm);
  memcpy(m + 0x20, target, 8);
  if (!send_ilm(0x7e, m, 0x88)) return 1;
  printf("s3: bonding with %s:\n", name);
  int64_t giveUp = now_ms() + 40000;
  static uint8_t r[kIlm];
  while (now_ms() < giveUp) {
    pollfd p = {g_sock, POLLIN, 0};
    if (poll(&p, 1, 500) <= 0) continue;
    ssize_t n = recv(g_sock, m, kIlm, 0);
    if (n < 8) continue;
    uint32_t id = u32(m, 4);
    long long at = (long long)(now_ms() - t0);
    if (id == 0xa3) {  // PIN request: 0000
      printf("s3:   +%5lld ms  PIN requested: answering 0000\n", at);
      memset(r, 0, kIlm);
      memcpy(r + 0x20, target, 8);
      r[0x28] = 4;
      memcpy(r + 0x29, "0000", 4);
      send_ilm(0x75, r, 0x20);
    } else if (id == 0xa4) {  // SSP: confirm the number
      char number[7] = {0};
      memcpy(number, m + 0x28, 6);
      printf("s3:   +%5lld ms  confirmation requested (number %s, display %u): accepting\n", at, number, m[0x2e]);
      memset(r, 0, kIlm);
      memcpy(r + 0x20, target, 8);
      r[0x28] = 1;
      send_ilm(0x76, r, 0x10);
    } else if (id == 0xab) {
      printf("s3:   +%5lld ms  bonding result %u (%s)\n", at, m[0x20], m[0x20] == 0 ? "paired" : "failed");
      return m[0x20] == 0 ? 0 : 1;
    } else {
      printf("s3:   +%5lld ms  event 0x%x, %zd bytes, first byte %u\n", at, id, n, m[0x20]);
    }
  }
  printf("s3: no bonding result within 40 s\n");
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 15;
  const char* pairWith = argc > 2 ? argv[2] : nullptr;
  setvbuf(stdout, nullptr, _IONBF, 0);
  g_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
  // the abstract name "bt.ext.adp", as socket_local_server("bt.ext.adp", ABSTRACT, SOCK_DGRAM) binds it
  sockaddr_un me = {};
  me.sun_family = AF_UNIX;
  const char* name = "bt.ext.adp";
  memcpy(me.sun_path + 1, name, strlen(name));
  socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + strlen(name));
  if (bind(g_sock, (sockaddr*)&me, len) != 0) {
    printf("s3: can't bind bt.ext.adp (%s): is Android's Bluetooth service still running?\n", strerror(errno));
    return 1;
  }
  static uint8_t m[kIlm];
  int64_t t0 = now_ms();

  memset(m, 0, kIlm);
  if (!send_ilm(0x64, m, 0) || !wait_ilm(0x91, m, 15000)) {
    printf("s3: no answer to power on within 15 s\n");
    return 1;
  }
  printf("s3: power on: result %u, %lld ms\n", m[0x20], (long long)(now_ms() - t0));

  memset(m, 0, kIlm);
  if (send_ilm(0x6f, m, 0) && wait_ilm(0x9d, m, 5000)) {
    char a[18];
    addr_string(m + 0x24, a);
    printf("s3: own address: result %u, %s\n", m[0x20], a);
  } else {
    printf("s3: no answer to the address request\n");
  }

  // a discovery still running (an inquiry lasts about a minute) makes a new one fail at once (0x95,
  // result 1), so cancel first
  memset(m, 0, kIlm);
  send_ilm(0x68, m, 0);
  wait_ilm(0x96, m, 3000);

  // discovery, with the fields btmtk_gap_discovery sets
  memset(m, 0, kIlm);
  uint32_t all = 0xffffffff, type = 0;
  memcpy(m + 0x20, &all, 4);  // device classes: all
  m[0x24] = 0x30;
  m[0x25] = 0x19;
  memcpy(m + 0x30, &type, 4);
  int64_t t1 = now_ms();
  if (!send_ilm(0x67, m, 0x18)) return 1;
  printf("s3: discovery for %d s:\n", seconds);
  std::set<uint64_t> found;
  uint8_t target[8] = {0};
  bool haveTarget = false;
  int64_t until = now_ms() + int64_t(seconds) * 1000;
  while (now_ms() < until && !haveTarget) {
    pollfd p = {g_sock, POLLIN, 0};
    if (poll(&p, 1, 500) <= 0) continue;
    ssize_t n = recv(g_sock, m, kIlm, 0);
    if (n < 8) continue;
    uint32_t id = u32(m, 4);
    uint8_t nameLen = n > 0x51 ? m[0x50] : 0;
    if (id == 0x93 && nameLen < 60) {
      uint64_t key = (uint64_t(u16(m, 0x26)) << 32) | (uint64_t(m[0x24]) << 24) | (u32(m, 0x20) & 0xffffff);
      if (found.insert(key).second || nameLen) {
        char a[18], nm[64] = {0};
        addr_string(m + 0x20, a);
        memcpy(nm, m + 0x51, nameLen);
        printf("s3:   +%5lld ms  %s  rssi %d  class 0x%06x  %s\n", (long long)(now_ms() - t1), a, int8_t(m[0x44]),
               u32(m, 0x48) & 0xffffff, nameLen ? nm : "(no name yet)");
        if (pairWith && nameLen && !strcasecmp(nm, pairWith)) {
          memcpy(target, m + 0x20, 8);
          haveTarget = true;
        }
      }
    } else if (g_seen.insert(id).second) {
      printf("s3:   +%5lld ms  event 0x%x, %zd bytes, result %u\n", (long long)(now_ms() - t1), id, n, m[0x20]);
    }
  }
  printf("s3: %zu devices found\n", found.size());
  if (!pairWith) return 0;
  if (!haveTarget) {
    printf("s3: \"%s\" not found\n", pairWith);
    return 1;
  }
  return pair(target, pairWith, m);
}
