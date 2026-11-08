// Rondo's version, the one every part of the project shares (tools/set-version.sh writes it).
#pragma once

namespace rondo {
constexpr const char* kRondoVersion = "1.0.2";
// the firmware this Rondo needs (ro.rondo.firmware): 2.0's, which runs it without Android
constexpr int kFirmwareNeeded = 2;
}
