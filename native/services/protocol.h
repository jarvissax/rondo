// The messages between the UI and rondo-core: a 32-bit length, a 32-bit type, then the payload (as
// rondo-player's, player/protocol.h).
#pragma once
#include <stdint.h>

namespace rondo {

enum CoreCommand : uint32_t {
  kWifiEnable = 1,  // u8
  kWifiScan,
  kWifiConnect,  // i32 network id
  kWifiJoin,     // u8 security, the name, a zero byte, the password
  kWifiForget,   // i32 network id
  kBtConfigure = 10,  // the state file's path
  kBtPower,           // u8
  kBtSearch,          // u8
  kBtPair,            // the address
  kBtConnect,         // the address
  kBtDisconnect,      // the address
  kBtForget,          // the address
  kApiConfigure = 20,  // the server's base URL, a zero byte, the token
  kApiGet,             // u32 request id, u32 cache ms, the path
  kApiReply = 100,     // rondo-core to the UI: u32 request id, i32 HTTP status (0: no connection), the body
};

struct CoreState;
// rondo-core's main: never returns.
[[noreturn]] void runCore(int sock, CoreState* shm);

}  // namespace rondo
