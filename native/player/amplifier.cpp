// amplifier.h.
#include "amplifier.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

// MediaTek's AudioSystem extensions (libmedia): static, so plain functions by their symbols
extern "C" int rondoGetAudioData(int what, unsigned size, void* data) __asm__("_ZN7android11AudioSystem12GetAudioDataEijPv");
extern "C" int rondoSetAudioData(int what, unsigned size, void* data) __asm__("_ZN7android11AudioSystem12SetAudioDataEijPv");

namespace rondo::amp {
namespace {
constexpr int kTable = 0x245;  // sizeof(AUDIO_VER1_CUSTOM_VOLUME_STRUCT): the HAL checks it
constexpr int kGetVer1 = 0x100, kSetVer1 = 0x101;

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out.assign(kTable, 0);
  bool ok = fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}
}  // namespace

bool read(std::vector<uint8_t>& table) {
  std::vector<uint8_t> b(kTable + 64, 0);
  if (rondoGetAudioData(kGetVer1, kTable, b.data()) != 0) return false;
  table.assign(b.begin(), b.begin() + kTable);
  // an empty answer (all zeros) isn't a table
  for (uint8_t v : table)
    if (v) return true;
  return false;
}

bool louder(const std::vector<uint8_t>& t) {
  return int(t.size()) == kTable && t[kHeadset] >= kLouderHeadset && t[kSpeaker] >= kLouderSpeaker;
}

bool set(bool up, const std::string& backup, std::string& why) {
  std::vector<uint8_t> t;
  if (!read(t)) return why = "the volume table can't be read", false;
  if (louder(t) == up) return true;
  std::vector<uint8_t> shipped;
  bool haveBackup = readFile(backup, shipped);
  if (up && !haveBackup) {  // the table as it came, before anything changes it
    int fd = open(backup.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    bool ok = fd >= 0 && write(fd, t.data(), t.size()) == ssize_t(t.size()) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    if (!ok) return why = "the shipped table can't be kept", false;
  }
  if (up) {
    t[kHeadset] = kLouderHeadset;
    t[kSpeaker] = kLouderSpeaker;
  } else {
    // the kept table's values, else this firmware's own defaults (libcustom_nvram.so's
    // audio_ver1_custom_default, what this Y1 came with)
    t[kHeadset] = haveBackup ? shipped[kHeadset] : 124;
    t[kSpeaker] = haveBackup ? shipped[kSpeaker] : 118;
  }
  rondoSetAudioData(kSetVer1, kTable, t.data());  // NVRAM, then the HAL applies it
  std::vector<uint8_t> now;
  if (!read(now) || now[kHeadset] != t[kHeadset] || now[kSpeaker] != t[kSpeaker]) return why = "the HAL didn't take it", false;
  fprintf(stderr, "rondo-player: amplifier %s: headphones %d, speaker %d\n", up ? "louder" : "standard", now[kHeadset], now[kSpeaker]);
  return true;
}

}  // namespace rondo::amp
