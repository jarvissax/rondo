// The clock without Android (1.0.x had its network time): after a fully drained battery the Y1's
// clock can be years off, and then no certificate checks out. Once Wi-Fi is up, the server's own
// Date header (the Y1 never asks anyone else) sets the system clock and the RTC, so it holds over the
// next reboot. The request checks the certificate as usual, all but its dates.
#pragma once
#include <string>

namespace rondo {

// true when the clock was right or is now (|server - here| > 2 s: set)
bool syncClock(const std::string& baseUrl);

}  // namespace rondo
