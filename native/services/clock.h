// The clock without Android (1.0.x had its network time): after a fully drained battery the Y1's
// clock can be years off, and then no certificate checks out. Once Wi-Fi is up, the server's own
// Date header (the Y1 never asks anyone else) sets the system clock and the RTC, so it holds over the
// next reboot. The request checks the certificate as usual, all but its dates.
#pragma once
#include <string>

namespace rondo {

// true when the clock was right or is now (|server - here| > 2 s: set)
bool syncClock(const std::string& baseUrl);

class Api;

// The time zone (Nov 9), on 2.0's firmware: Etc/UTC until the server has named one (the
// stock firmware's Asia/Shanghai doesn't stand), then the zone of the Y1's public address, which the
// server's /timezone asks a lookup service for, once per Wi-Fi connection. It is Android's own
// persist.sys.timezone, which bionic's localtime reads (it outlasts a restart), and
// /data/rondo/timezone notes that Rondo set it.
void timeZoneDefault();
// false when the server couldn't be asked (try again later); true when it answered (an older server's
// 404 and "no idea" included)
bool timeZoneFromServer(Api& api);

}  // namespace rondo
