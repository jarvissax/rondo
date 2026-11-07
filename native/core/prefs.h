// The device settings 1.0.x kept outside config.json: video quality in its shared preferences, and
// brightness, the screen timeout and the click sound in Android's own settings. Rondo 2.0 has no
// Android settings, so it keeps them in a small file of its own; the first time, it takes them from
// where 1.0.x and Android left them, so nothing resets on moving to 2.0.
#pragma once
#include <string>

namespace rondo {

struct Prefs {
  int videoQuality = 0;           // the height to play videos at; 0: Auto
  int brightness = 128;           // the backlight, 0-255 (Settings.System.SCREEN_BRIGHTNESS)
  int screenTimeoutMs = 60000;    // Settings.System.SCREEN_OFF_TIMEOUT
  bool clickSound = true;         // Settings.System.SOUND_EFFECTS_ENABLED
};

// The file, or on the first run what 1.0.x's preferences and Android's settings database hold.
Prefs loadPrefs(const std::string& path);
bool savePrefs(const std::string& path, const Prefs& p);

}  // namespace rondo
