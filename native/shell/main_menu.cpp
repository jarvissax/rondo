// The main menu (MainMenuScreen.kt in 1.0.x): the menu on the left, a preview of the selected entry on
// the right.
#include "app.h"
#include "screens.h"

namespace rondo::shell {

// MainMenuScreen: the menu on the left (190 units), a preview of the selected entry on the right.
class MainMenuScreen : public Screen {
 public:
  MainMenuScreen() {
    struct E {
      const char* id;
      const char* label;
      const Tile* tile;
    };
    static const E kEntries[] = {{"music", "YouTube Music", &tiles::music}, {"videos", "YouTube", &tiles::video},
                                 {"device", "Library", &tiles::device},     {"nowPlaying", "Now Playing", &tiles::nowPlaying},
                                 {"radio", "FM Radio", &tiles::radio},      {"settings", "Settings", &tiles::settings}};
    std::vector<Row> rows;
    for (const MenuEntry& m : g_config.theme.menu) {
      if (!m.visible) continue;
      for (const E& e : kEntries) {
        if (m.id != e.id) continue;
        Row r;
        r.title = e.label;
        r.tile = e.tile;
        r.chevron = true;
        if (m.id == "device") r.onSelect = [] { g_app->push(makeDevice()); };
        if (m.id == "nowPlaying") r.onSelect = [] { openNowPlaying(); };
        rows.push_back(std::move(r));
      }
    }
    list_.onSelectionChanged = [this] {
      const Row* r = list_.selectedRow();
      if (!r) return;
      if (shown_ != r->tile) {
        from_ = shown_;
        shown_ = r->tile;
        fade_.begin(0, 1, now_, 180 * kMs, easeInOutCubic);
      }
    };
    list_.setRows(std::move(rows));
    from_ = shown_;
  }
  std::string title() const override { return "Rondo"; }
  bool onAction(Action a, int steps) override {
    switch (a) {
      case Action::Down: return list_.down(steps);
      case Action::Up: return list_.up(steps);
      case Action::Select: return list_.select();
      case Action::Back: return true;  // the launcher: nothing below it
      default: return false;
    }
  }
  bool step(float dt) override {
    bool a = list_.step(dt);
    return a || fade_.running(now_);
  }
  // the Now Playing pane follows the song and its progress (1.0.x's onTick)
  bool tick(int64_t) override {
    if (shown_ != &tiles::nowPlaying) return false;
    PlayerState s = g_player.state();
    std::string sig = std::string(s.path) + "\n" + std::to_string(s.index) + "\n" +
                      std::to_string(s.durationMs > 0 ? s.positionMs * 120 / s.durationMs : 0);
    if (sig == paneSig_) return false;
    paneSig_ = sig;
    return true;
  }
  void draw(Env& e, float w, float h) override {
    now_ = e.now;
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    float lw = float(e.dp(190)), pad = float(e.dp(3));
    list_.layout(e, lw, h - pad);
    list_.draw(e, 0, pad);
    e.c.layer(0);
    e.c.fill({lw, 0, 1, h}, p.barLine);
    Rect pane{lw + 1, 0, w - lw - 1, h};
    float t = fade_.at(e.now);
    if (from_ && t < 1) drawPane(e, pane, from_, 1);
    if (shown_) {
      e.c.pushAlpha(from_ && t < 1 ? t : 1);
      drawPane(e, pane, shown_, 1);
      e.c.popAlpha();
    }
  }

 private:
  // renderPane() for an entry that isn't playing: its tile's colors corner to corner, its glyph on a
  // translucent rounded square (the logos alone), and its name
  // renderPane() for Now Playing with a song: the artwork on its own color (darker toward the bottom),
  // the title and the artist under it, centered, and a thin progress line
  void drawPlaying(Env& e, Rect pane, const PlayerState& s) {
    auto darker = [](Color c, float f) {
      return rgb(uint32_t(((c >> 16) & 0xff) * f) << 16 | uint32_t(((c >> 8) & 0xff) * f) << 8 | uint32_t((c & 0xff) * f));
    };
    std::string path = s.path;
    Color base = rgb(0x3a3f4a);
    artColor(e, path, &base);
    e.c.layer(0);
    e.c.fillGradient(pane, darker(base, 1.0f), darker(base, 0.45f));
    int padX = e.dp(10), padY = e.dp(8);
    float ix = pane.x + padX, iy = pane.y + padY, iw = pane.w - 2 * padX, ih = pane.h - 2 * padY;
    int box = e.dp(98);
    SongInfo song = songInfo(path);
    std::string artist = firstAndRest(song.subtitle).first;
    std::vector<Label> title = wrap(e, song.title, 14, true, iw, 2), text;
    if (!artist.empty()) text = wrap(e, artist, 11, false, iw, 2);
    int titleH = 0, textH = 0;
    for (auto& l : title) titleH += l.h;
    for (auto& l : text) textH += l.h;
    if (title.empty()) titleH = label(e, " ", 14, true).h;
    int barH = e.dp(8);
    int total = box + e.dp(9) + titleH + (text.empty() ? 0 : e.dp(2) + textH) + e.dp(6) + barH;
    float top = iy + float((int(ih) - total) / 2);
    float bx = ix + float((int(iw) - box) / 2);
    Rect ar{bx, top, float(box), float(box)};
    e.c.layer(2);
    e.c.shape(ar, e.dpf(6), 0x3cffffff);
    float a = 0;
    if (const Texture* t = e.covers.get(path, box, box, 0, e.now, &a)) {
      e.c.layer(3);
      e.c.image(*t, ar, {0, 0, 1, 1}, e.dpf(6), withAlpha(0xffffffff, a));
    }
    e.c.layer(4);
    float y = top + box + e.dp(9);
    for (auto& l : title) drawLabel(e, l, centerText(ix, iw, l.w), y, rgb(0xffffff)), y += l.h;
    if (title.empty()) y += titleH;
    if (!text.empty()) {
      y += e.dp(2);
      for (auto& l : text) drawLabel(e, l, centerText(ix, iw, l.w), y, 0xd7ffffff), y += l.h;
    }
    y += e.dp(6);
    e.c.layer(2);
    progressBar(e, {bx, y, float(box), float(barH)}, 0x46ffffff, rgb(0xffffff),
                s.durationMs > 0 ? float(s.positionMs) / s.durationMs : 0, 0, 2);
  }

  void drawPane(Env& e, Rect pane, const Tile* tile, float) {
    if (tile == &tiles::nowPlaying) {
      PlayerState s = g_player.state();
      if (s.index >= 0 && s.path[0]) return drawPlaying(e, pane, s);
    }
    auto darker = [](Color c, float f) {
      return rgb(uint32_t(((c >> 16) & 0xff) * f) << 16 | uint32_t(((c >> 8) & 0xff) * f) << 8 | uint32_t((c & 0xff) * f));
    };
    e.c.layer(0);
    fillDiagonal(e.c, pane, tile->top, tile->bottom, darker(tile->bottom, 0.7f));
    int padX = e.dp(10), padY = e.dp(8);
    float ix = pane.x + padX, iy = pane.y + padY, iw = pane.w - 2 * padX, ih = pane.h - 2 * padY;
    int box = e.dp(98);
    const char* name = tile == &tiles::music       ? "YouTube Music"
                       : tile == &tiles::video      ? "YouTube"
                       : tile == &tiles::device     ? "Library"
                       : tile == &tiles::nowPlaying ? "Nothing playing"
                       : tile == &tiles::radio      ? "FM Radio"
                                                    : "Settings";
    Label title = label(e, name, 14, true, iw);
    int total = box + e.dp(9) + title.h;
    float top = iy + float((int(ih) - total) / 2);
    float bx = ix + float((int(iw) - box) / 2);
    Rect vis{bx, top, float(box), float(box)};
    e.c.layer(2);
    if (!tile->brand) {
      e.c.shape(vis, e.dpf(22), 0x2dffffff);
      int g = e.dp(70);
      e.c.layer(4);
      e.icons.draw(e.c, tile->kind, {bx + float((box - g) / 2), top + float((box - g) / 2), float(g), float(g)}, rgb(0xffffff));
    } else {
      e.c.layer(4);
      int gw = tile->brand == 1 ? e.dp(80) : e.dp(70), gh = tile->brand == 1 ? e.dp(56) : e.dp(70);
      Rect lr{bx + float((box - gw) / 2), top + float((box - gh) / 2), float(gw), float(gh)};
      if (tile->brand == 1) e.icons.videoLogo(e.c, lr);
      else e.icons.musicLogo(e.c, lr);
    }
    e.c.layer(4);
    drawLabel(e, title, centerText(ix, iw, title.w), top + box + e.dp(9), rgb(0xffffff));
  }

  RowList list_;
  const Tile* shown_ = nullptr;
  const Tile* from_ = nullptr;
  Tween fade_;
  int64_t now_ = 0;
  std::string paneSig_;
};

std::unique_ptr<Screen> makeMainMenu() { return std::unique_ptr<Screen>(new MainMenuScreen()); }

}  // namespace rondo::shell
