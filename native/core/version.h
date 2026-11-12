// Rondo's version, the one every part of the project shares (tools/set-version.sh writes it).
#pragma once

namespace rondo {
constexpr const char* kRondoVersion = "3.0.0";
// the firmware this Rondo needs (ro.rondo.firmware): 3.0's, without the stock firmware's root backdoors
// (2: 2.0's, the first without Android; Rondo 3.0 installs on it only with Innioasis Updater CE)
constexpr int kFirmwareNeeded = 3;
}
