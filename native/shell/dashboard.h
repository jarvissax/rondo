// The dashboard's server (DashboardServer.kt in 1.0.x, docs/dev/dashboard-api.md): the web page and its
// JSON API on port 8080, only while Settings › Dashboard is open. One thread per connection (as
// NanoHTTPD); what touches the player, the library or the settings runs on the UI thread.
#pragma once
#include <stdint.h>

#include <string>

namespace rondo::shell {

constexpr int kDashboardPort = 8080;

// assets: <dir>/dashboard (index.html, app.js, app.css) and <dir>/fonts. False when the port can't be had.
bool dashboardStart(const std::string& assetsDir);
void dashboardStop();
bool dashboardRunning();
std::string dashboardCode();    // the pairing code, six digits
int dashboardBrowsers();        // sessions that made a request in the last 30 s
int64_t dashboardLastRequest();  // CLOCK_MONOTONIC ns of the last signed-in request
void dashboardTick();  // the Bluetooth search the page keeps going stops 30 s after its last poll

}  // namespace rondo::shell
