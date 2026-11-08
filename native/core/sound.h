// Settings › Sound (new in 2.0): each output keeps its own sound, the speaker, wired headphones and
// every Bluetooth device, as an equalizer preset, the custom 10 bands, a loudness boost; volume
// leveling is one setting for all. Kept in sound.json beside the prefs (1.0.x's config.json knows
// nothing of it). Resolved into the player's SoundSettings (player/dsp.h).
#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>

#include "../player/dsp.h"

namespace rondo {

constexpr int kEqBands = 10;
extern const float kEqFreqs[kEqBands];  // 31 Hz to 16 kHz, an octave apart

struct EqPreset {
  const char* id;
  const char* name;
  const char* note;  // the list's note: "" or a few words
};
// The built-in presets, in the list's order ("flat" first: the sound as it comes)
const std::vector<EqPreset>& eqPresets();

struct UserPreset {
  std::string name;
  std::array<float, kEqBands> gains{};
};

struct SoundProfile {  // one output's sound
  std::string preset = "flat";  // a built-in id, "custom", "user:<name>", or "autoeq"
  std::array<float, kEqBands> custom{};  // the custom equalizer, dB
  int boostDb = 0;                       // the loudness boost: 0, 3, 6, 9 or 12
  // Headphone match: AutoEq's correction for a model, as the server gave it (kept: it works offline)
  std::string autoEqName, autoEqSource;
  float autoEqPreamp = 0;
  std::vector<Filter> autoEq;
};

struct SoundConfig {
  bool leveling = false;
  std::map<std::string, SoundProfile> outputs;  // "speaker", "headphones", "bt:<address>"
  std::vector<UserPreset> userPresets;
};

SoundConfig loadSound(const std::string& path);
bool saveSound(const std::string& path, const SoundConfig& c);

// The preset's name for a profile ("Custom", a user preset's name…)
std::string presetName(const SoundConfig& c, const SoundProfile& p);
// What the player runs for a profile: the filters, the gain, leveling. On the speaker, whenever anything
// is applied, what it can't play (below 180 Hz) is cut first: more loudness, no rattle.
SoundSettings resolveSound(const SoundConfig& c, const SoundProfile& p, bool speaker = false);
// The equalizer's ten gains a profile's preset stands for (for the editor to start from)
std::array<float, kEqBands> presetGains(const SoundConfig& c, const SoundProfile& p);

}  // namespace rondo
