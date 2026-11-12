// Rondo's version on the Y1 (tools/set-version.sh device X.Y.Z writes it); the server has its own.
#pragma once

namespace rondo {
constexpr const char* kRondoVersion = "3.0.2";
// the firmware this Rondo needs (ro.rondo.firmware): 3.0's, without the stock firmware's root backdoors
// (2: 2.0's, the first without Android; Rondo 3.0 installs on it only with Innioasis Updater CE)
constexpr int kFirmwareNeeded = 3;
// The oldest Rondo server this Rondo works with, against the version the server says in GET /healthz
// (server/src/version.ts; the server is released apart, tags server-v<version>). Raised only when this
// Rondo needs something a newer server brings; an older server is asked to update (Settings › YouTube ›
// Connection, the dashboard's server test, the YouTube screens' messages). 3.1.0 is the first server
// that says its version: an older one can't be told apart, and counts as too old.
constexpr const char* kServerVersionNeeded = "3.1.0";
}
