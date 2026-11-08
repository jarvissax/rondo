// amplifier.h.
#include "amplifier.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>

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

int level(const std::vector<uint8_t>& t) {
  if (int(t.size()) != kTable) return 0;
  int b = std::min<int>(t[kHeadset], 152), reg = 7 - ((152 - b) / 4) / 2;  // the HAL's arithmetic
  return std::max(0, std::min(kLevels - 1, reg - 4));
}

bool set(int lvl, const std::string& backup, std::string& why) {
  lvl = std::max(0, std::min(kLevels - 1, lvl));
  std::vector<uint8_t> t;
  if (!read(t)) return why = "the volume table can't be read", false;
  std::vector<uint8_t> shipped;
  bool haveBackup = readFile(backup, shipped);
  uint8_t headset = kHeadsetAt[lvl], speaker = kSpeakerAt[lvl];
  if (lvl == 0 && haveBackup) headset = shipped[kHeadset], speaker = shipped[kSpeaker];  // exactly as it came
  if (t[kHeadset] == headset && t[kSpeaker] == speaker) return true;
  if (lvl > 0 && !haveBackup) {  // the table as it came, before anything changes it
    int fd = open(backup.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    bool ok = fd >= 0 && write(fd, t.data(), t.size()) == ssize_t(t.size()) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    if (!ok) return why = "the shipped table can't be kept", false;
  }
  // level 0 without the kept table: this firmware's own defaults (libcustom_nvram.so's
  // audio_ver1_custom_default, what this Y1 came with), kHeadsetAt[0] and kSpeakerAt[0]
  t[kHeadset] = headset;
  t[kSpeaker] = speaker;
  rondoSetAudioData(kSetVer1, kTable, t.data());  // NVRAM, then the HAL applies it
  std::vector<uint8_t> now;
  if (!read(now) || now[kHeadset] != t[kHeadset] || now[kSpeaker] != t[kSpeaker]) return why = "the HAL didn't take it", false;
  fprintf(stderr, "rondo-player: amplifier level %d: headphones %d, speaker %d\n", lvl, now[kHeadset], now[kSpeaker]);
  return true;
}

}  // namespace rondo::amp
