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
  Shorts, Clock,
  // 2.0's: one glyph for each place (Settings' groups, Library's sections), none used twice
  Star, Photo, File, Headphones, Film, People, Tag, Flame, History, Sparkle, Stack, Tree, Sliders, Gauge,
  Vibrate, Wheel, Chip, Key, Diamond, Queue, Seek, Download, Sync, Usb, Device, Broadcast, Library, Mic,
  Type, Menu, Wand, Bolt, Lock, Captions,
  // WhatsApp's: a chat, send, a message read (two ticks)
  Chat, Send, Ticks, Tick, Smile,
  // the brand logos, each in two colors (drawn as their two masks)
  MusicLogoDisc, MusicLogoMarks, VideoLogoButton, VideoLogoPlay, WhatsAppBubble, WhatsAppPhone,
  // the marks alone, drawn white on their tiles: WhatsApp's (ring, tail, phone), YouTube Music's (ring, play)
  WhatsAppMark, MusicMark,
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
  // charging: the charge green, and a green bolt just left of the frame, reaching chargeRoom()
  // pixels left of r (callers keep that much room before it while charging)
  void battery(Canvas& c, Rect r, Color color, float level, bool charging = false);
  static int chargeRoom(float w, float h);  // for a battery drawn w x h
  // The YouTube Music and YouTube logos (MusicLogo, VideoLogo).
  void musicLogo(Canvas& c, Rect r);
  void videoLogo(Canvas& c, Rect r);
  void whatsappLogo(Canvas& c, Rect r);  // WhatsApp's: the green bubble, the white phone

  // An icon in a tile (or a round button): its box, bw x bh as draw() takes it, set so the icon's
  // optical center sits on the area's center. The optical center is halfway from the center of the
  // ink's bounding box to the ink's center of mass, so a lopsided shape (WhatsApp's tail, a radio's
  // body under its antenna) looks centered, not just measures centered. The shapes are rasterized at
  // that spot to an eighth of a pixel, so an odd leftover pixel is split instead of going right.
  struct Placed {
    float x = 0, y = 0;  // the mask's top-left (whole pixels)
    int bw = 0, bh = 0;  // the box
    int pad = 0;         // the mask's margin around the box
    int qx = 0, qy = 0;  // the box's offset past x + pad, y + pad, in eighths of a pixel
  };
  Placed place(Icon icon, Rect area, float bw, float bh);
  // Draws an icon at a placement: the one it was made for, or another of the same box (a logo's
  // second color, so the two stay together).
  void drawPlaced(Canvas& c, Icon icon, const Placed& at, Color color);
  void drawInTile(Canvas& c, Icon icon, Rect area, float size, Color color);  // a square box

 private:
  // where an icon's ink sits in its box: the bounding box's center (cx, cy) and the center of mass
  // (mx, my), in pixels from the box's center
  struct Ink {
    float cx = 0, cy = 0, mx = 0, my = 0;
  };
  Ink ink(Icon icon, int bw, int bh);
  // the shapes laid out in the box (bx, by, bw, bh) of a w x h mask, as mask() lays them out in the whole
  std::vector<uint8_t> raster(Icon icon, int w, int h, int level, float bx, float by, float bw, float bh);
  // a mask from the cache, or rastered and added to the atlas; spot: 0 for a whole view, else the
  // placement's code (box and eighths)
  Text::Mask cached(Icon icon, int w, int h, int level, int spot, float bx, float by, float bw, float bh);
  FT_Library ft_ = nullptr;
  Text* text_ = nullptr;
  uint32_t generation_ = 0;
  std::map<std::tuple<int, int, int, int, int>, Text::Mask> cache_;
  std::map<std::tuple<int, int, int>, Ink> ink_;
};

// batteryColor() in IconView.kt: green from 50 %, amber from 20 %, red below.
Color batteryColor(float level);

}  // namespace rondo::ui
