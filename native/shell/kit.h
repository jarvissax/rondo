// The shell's building blocks, ported 1:1 from 1.0.x (ui/Widgets.kt, ui/RowList.kt, ui/Theme.kt): the
// status bar, the SHELL row list with its headers, tiles and artwork, and text laid out as Android
// 4.2 lays out a one-line TextView. Sizes are 1.0.x's mockup units through dp(), as there.
#pragma once
#include <math.h>

#include <functional>
#include <string>
#include <vector>

#include "../ui/covers.h"
#include "../ui/gfx.h"
#include "../ui/icons.h"
#include "../ui/motion.h"
#include "../ui/text.h"
#include "../ui/theme.h"

namespace rondo::shell {
using namespace rondo::ui;

// What a screen draws with.
struct Env {
  Canvas& c;
  Text& text;
  Icons& icons;
  Covers& covers;
  const Palette& p;
  int64_t now = 0;
  // Kotlin's Context.dp(): (v * density * UI_SCALE).roundToInt(), density 1 on the Y1
  int dp(float v) const { return int(floorf(v * p.scale + 0.5f)); }
  float dpf(float v) const { return v * p.scale; }
  int font(float sp, bool bold = false) { return text.font(bold ? Weight::Bold : Weight::Regular, sp * p.scale); }
};

// A one-line TextView: no font padding, ellipsized at the end, as wide as its text (WRAP_CONTENT) up
// to maxW. Its top-left is where it is drawn; the baseline is ascentInt below.
struct Label {
  const TextRun* run = nullptr;
  int font = 0;
  int w = 0, h = 0;
};
Label label(Env& e, const std::string& s, float sp, bool bold, float maxW = 0);
void drawLabel(Env& e, const Label& l, float x, float top, Color c);
// Where a TextView with gravity CENTER starts its text: Android 4.2's Layout.drawText() evens the line
// width, so odd widths round to the right (a view centered by its parent rounds down instead).
inline float centerText(float left, float width, int textW) {
  return float((int(left) * 2 + int(width) - (textW & ~1)) >> 1);
}

// A TextView of up to maxLines lines (word wrap, the last line ellipsized): its lines, top to bottom.
std::vector<Label> wrap(Env& e, const std::string& s, float sp, bool bold, float maxW, int maxLines);

// A colored rounded tile with a white glyph (Tiles in Widgets.kt); the YouTube services show their
// logo on a white tile.
struct Tile {
  Icon kind;
  Color top, bottom;
  int brand = 0;  // 1 YouTube, 2 YouTube Music
};
namespace tiles {
extern const Tile music, video, device, nowPlaying, settings, wifi, bluetooth, quality, server, about, songs, artists,
    albums, dashboard, brightness, timer, radio, lyrics, sound, accel, wheel, videos, shuffle, power, restart, theme,
    playlists, add, search, recent, del, rescan, shorts, update;
}
void drawTile(Env& e, const Tile& t, Rect r, float alpha = 1);

// GradientDrawable TL_BR with three colors (the main menu's preview pane).
void fillDiagonal(Canvas& c, Rect r, Color c0, Color c1, Color c2);

// The iPod status bar (ShellStatusBar): brushed gradient, the play state, the title, Wi-Fi, battery
// percentage and battery, 22 units tall. title2/t: a second title cross-fading in (screen changes).
struct Status {
  bool playing = false, online = false;
  float battery = 1;
};
void statusBar(Env& e, float w, const Status& s, const std::string& title, const std::string& title2 = "", float t = 0);

// One row of a shell list (Row.Entry / Row.Header in RowList.kt).
struct Row {
  bool header = false;
  std::string title, subtitle, trailing;
  std::string thumb;  // a music file whose cover is the row's artwork
  bool chevron = false, enabled = true;
  const Tile* tile = nullptr;
  std::function<void()> onSelect, onLongSelect;
};

// RowList in the SHELL style. One selected row, moved by the wheel; at rest the list sits exactly where
// 1.0.x puts it (the selection re-centered when it reaches the first or last fully visible row; the top
// at the first row), and it glides there. The highlight glides between rows.
class RowList {
 public:
  void setRows(std::vector<Row> rows, bool keepSelection = false);
  void setMessage(const std::string& m) {
    rows_.clear();
    message_ = m;
    selected_ = -1;
  }
  // The list's own area (RecyclerView size).
  void layout(Env& e, float w, float h);
  // 1.0.x's RowList.handle(): Down/Up by `steps`; true when used
  bool down(int steps);
  bool up(int steps);
  bool select();      // Select: the row's action
  bool longSelect();  // LongSelect
  void selectIndex(int i);
  int selected() const { return selected_; }
  const Row* selectedRow() const { return selected_ >= 0 ? &rows_[selected_] : nullptr; }
  const std::vector<Row>& rows() const { return rows_; }
  std::function<void()> onSelectionChanged;
  // Long lists: rows hold only what selection needs (header, enabled); this fills in a row's text and
  // artwork when it is drawn, so opening 821 songs costs nothing up front.
  std::function<void(int, Row&)> fill;

  bool step(float dt);  // true while moving
  void draw(Env& e, float x, float y);

 private:
  bool selectable(int i) const { return i >= 0 && i < int(rows_.size()) && !rows_[i].header && rows_[i].enabled; }
  int firstSelectable() const;
  void ensureVisible();
  void move(int dir, int steps);
  float rowH(int i) const { return rows_[i].header ? headerH_ : entryH_; }

  std::vector<Row> rows_;
  std::vector<float> top_;  // each row's top in the list
  float total_ = 0, w_ = 0, h_ = 0, entryH_ = 50, headerH_ = 28;
  int selected_ = -1;
  float target_ = 0;
  Spring scroll_, bar_;
  bool placed_ = false;
  std::string message_ = "Loading\xe2\x80\xa6";
};

std::string formatTime(int seconds);  // 1.0.x formatTime(): m:ss or h:mm:ss

}  // namespace rondo::shell
