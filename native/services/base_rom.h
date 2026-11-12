// The stock firmware's root backdoors (docs/device/README.md): Innioasis's firmware, and so the base ROM
// every Rondo firmware is made from, runs zxly's agent (aggdll, init's aggsrv: a hidden su and a silent
// app installer), the downloader it drops (wifi_init: a file fetched over plain HTTP and run as root)
// and SPWall (libsp.so, init's spwall: any command run as root). firmware/build.sh leaves them out of
// the firmware since 2.1.0, but a Y1 that takes 2.1.0 as an update keeps its old system and boot
// images, so Rondo takes them out itself.
#pragma once

namespace rondo {

// At every start, on a thread of its own, a couple of seconds in: stops init's aggsrv and spwall when
// init has them, ends whatever still runs one of the programs, and, only when one of the files is still
// there, remounts /system read-write, deletes what firmware/build.sh leaves out, puts Android's am
// script back with Android's owner and mode, and pm's, and remounts /system read-only. One line in the
// log says what it did; on a clean firmware it does nothing and says nothing.
void removeBaseRomBackdoors();

}  // namespace rondo
