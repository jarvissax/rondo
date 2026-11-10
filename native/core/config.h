// Settings: the same config.json as Rondo 1.0.x (Config.kt), read and written with the same rules, so
// a device moving to 2.0 keeps its theme, behavior, server pairing and radio stations.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace rondo {

struct MenuEntry {
  std::string id;
  bool visible = true;
};

struct Theme {
  std::string accent = "#1c5ccc";  // iPod blue
  std::string appearance = "light";  // light | dark
  float textScale = 1.4f;
  std::vector<MenuEntry> menu;
};

struct Behavior {
  bool lyricsOnOpen = false;
  std::string wheelAcceleration = "normal";  // off | normal | fast
  std::string wheelVibration = "normal";     // off | light | normal | strong
};

struct Server {
  std::string url, token;  // empty: not paired
  // YouTube straight from the Y1, without the server (direct mode, rondo-core's local server); also
  // whenever no server is set up
  bool direct = false;
};

struct Preset {
  int frequency;  // tenths of MHz
  std::string name;
};

struct Radio {
  int frequency = 1000;
  bool speaker = false;
  std::vector<Preset> presets;
  std::vector<int> found;
  std::string band = "world";  // world 87.5-108 | japan 76-95
  std::map<int, std::string> names;  // RDS names heard, by frequency
};

struct Config {
  Theme theme;
  Behavior behavior;
  Server server;
  Radio radio;
};

// The main menu ids in their default order, and their labels.
extern const char* const kMenuIds[6];
const char* menuLabel(const std::string& id);
extern const float kTextScales[3];
struct AccentPreset {
  const char* name;
  const char* color;
};
extern const AccentPreset kAccentPresets[6];

// Every menu id once, Settings visible, a valid accent, a known text scale (Config.kt sanitize()).
void sanitize(Theme& t);
// A missing or unreadable file gives the defaults. "behaviour" (the spelling before Oct 31) is read too.
Config loadConfig(const char* path);
// Atomic: written to a temporary file, synced, then renamed over the old one.
bool saveConfig(const char* path, const Config& c);
// The color #rrggbb as 0xRRGGBB; false if it isn't one.
bool parseColor(const std::string& s, unsigned* rgb);

}  // namespace rondo
