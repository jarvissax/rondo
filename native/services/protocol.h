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
};

struct CoreState;
// rondo-core's main: never returns.
[[noreturn]] void runCore(int sock, CoreState* shm);

}  // namespace rondo
