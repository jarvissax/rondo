// 1.0.x's icons (IconView.kt, the YouTube logos in Widgets.kt), drawn the same way: vector shapes in a
// 24-unit box, fitted to the view's smaller side and centered. They are rasterized on the CPU at their
// final pixel size (FreeType's outline renderer and stroker: round caps and joins, antialiased) and
// cached as alpha masks, so they are drawn tinted in one quad like a glyph.
#pragma once
#include <stdint.h>

#include <map>
#include <tuple>

#include "gfx.h"
#include "text.h"

typedef struct FT_LibraryRec_* FT_Library;

namespace rondo::ui {

enum class Icon : uint8_t {
  Chevron, Play, Pause, Battery, Wifi, Search, Note, Shuffle, Repeat, Volume, Check, Bluetooth, Gear, Folder,
  Bars, Info, Person, Disc, Sun, Timer, Screen, Cloud, Radio, Power, Theme, Playlist, Plus, Trash, Refresh,
  Shorts,
  // the two brand logos, each in two colors (drawn as their two masks)
  MusicLogoDisc, MusicLogoMarks, VideoLogoButton, VideoLogoPlay,
  // the battery's two parts: the outline and tip (icon color) and the charge (by level)
  BatteryFrame, BatteryCharge,
};

class Icons {
 public:
  // The masks live in the text engine's glyph atlas: icons and text draw in one batch.
  bool init(Text& text);
  // The mask for an icon drawn into a w x h view (the shapes fit its smaller side, centered). level:
  // the battery charge in hundredths.
  Text::Mask mask(Icon icon, int w, int h, int level = 0);
  // Draws an icon into rect r (pixel-aligned) in one color.
  void draw(Canvas& c, Icon icon, Rect r, Color color, int level = 0);
  // The battery as 1.0.x draws it: the frame in `color`, the charge green, amber or red by level.
  void battery(Canvas& c, Rect r, Color color, float level);
  // The YouTube Music and YouTube logos (MusicLogo, VideoLogo).
  void musicLogo(Canvas& c, Rect r);
  void videoLogo(Canvas& c, Rect r);

 private:
  FT_Library ft_ = nullptr;
  Text* text_ = nullptr;
  uint32_t generation_ = 0;
  std::map<std::tuple<int, int, int, int>, Text::Mask> cache_;
};

// batteryColor() in IconView.kt: green from 50 %, amber from 20 %, red below.
Color batteryColor(float level);

}  // namespace rondo::ui
