// Rondo's version on the Y1 (tools/set-version.sh device X.Y.Z writes it); the server has its own.
#pragma once

namespace rondo {
constexpr const char* kRondoVersion = "3.0.2";
// the firmware this Rondo needs (ro.rondo.firmware): 3.0's, without the stock firmware's root backdoors
// (2: 2.0's, the first without Android; Rondo 3.0 installs on it only with Innioasis Updater CE)
constexpr int kFirmwareNeeded = 3;
// the oldest Rondo server this Rondo works with: its "api" in GET /healthz (server/src/version.ts),
// raised only when a change needs both sides; an older server is asked to update (Settings, the
// dashboard). The server's own version is its own (tags server-v<version>); before 3.1.0 it said no
// "api", which counts as 1.
constexpr int kServerApiNeeded = 1;
}
