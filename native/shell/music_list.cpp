// The YouTube Music building blocks and Up next; see music_list.h.
#include "music_list.h"

#include <math.h>
#include <stdio.h>

namespace rondo::shell {
namespace {

constexpr Color white = 0xffffffff;

Color darker(Color c, float f) {
  return rgb(uint32_t(((c >> 16) & 0xff) * f) << 16 | uint32_t(((c >> 8) & 0xff) * f) << 8 | uint32_t((c & 0xff) * f));
}

}  // namespace

void darkHeader(Env& e, float w, const std::string& title, HeaderLogo logo, Color titleColor, float heightUnits) {
  int h = e.dp(heightUnits), pad = e.dp(10);
  float x = float(pad);
  e.c.layer(4);
  if (logo != HeaderLogo::None) {
    int s = e.dp(16);
    Rect r{x, float((h - s) / 2), float(s), float(s)};
    if (logo == HeaderLogo::Music) e.icons.musicLogo(e.c, r);
    else e.icons.draw(e.c, Icon::Wifi, r, white);
    x += s + e.dp(7);
  }
  float iconsLeft = statusIcons(e, w - pad, h, e.p.darkMuted);
  Label t = label(e, title, 14, true, iconsLeft - x);
  drawLabel(e, t, x, float((h - t.h) / 2), titleColor);
}

void Ambient::follow(Env& e, const std::string& thumb, int artPx) {
  bool has = false;
  Color c = 0;
  if (!thumb.empty()) {
    if (!artColor(e, thumb, artPx, &c)) return;  // not decoded yet: the current color stays
    has = true;
  }
  if (has == has_ && (!has || c == shown_)) return;
  hadFrom_ = has_, from_ = shown_;
  has_ = has, shown_ = c;
  fade_.begin(0, 1, e.now, 200 * kMs, easeInOutCubic);
}

void Ambient::fill(Env& e, Rect r, bool has, Color c, float alpha) {
  e.c.pushAlpha(alpha);
  if (!has) {
    e.c.fill(r, e.p.dark);
  } else {  // GradientDrawable TOP_BOTTOM with three colors: the middle one halfway
    float half = floorf(r.h / 2);
    e.c.fillGradient({r.x, r.y, r.w, half}, darker(c, 0.55f), darker(c, 0.22f));
    e.c.fillGradient({r.x, r.y + half, r.w, r.h - half}, darker(c, 0.22f), e.p.dark);
  }
  e.c.popAlpha();
}

void Ambient::draw(Env& e, Rect r) {
  e.c.layer(0);
  if (fade_.running(e.now)) {
    fill(e, r, hadFrom_, from_, 1);
    fill(e, r, has_, shown_, fade_.at(e.now));
  } else {
    fill(e, r, has_, shown_, 1);
  }
}

bool miniPlayerShown() {
  PlayerState s = g_player.state();
  return s.index >= 0 && s.path[0];  // songs only for now: videos come with N3's video player
}

void miniPlayer(Env& e, Rect r) {
  PlayerState s = g_player.state();
  std::string path = s.path;
  e.c.layer(5);
  e.c.fill(r, 0xfa212121);
  int line = e.dp(2);
  e.c.layer(6);
  progressBar(e, {r.x, r.y, r.w, float(line)}, 0x3cffffff, white, s.durationMs > 0 ? float(s.positionMs) / s.durationMs : 0, 0, 2);
  float rowY = r.y + line, rowH = r.h - line;
  int art = e.dp(24), icon = e.dp(12);
  float ax = r.x + e.dp(6);
  Rect ar{ax, rowY + float((int(rowH) - art) / 2), float(art), float(art)};
  e.c.shape(ar, e.dpf(3), e.p.raised);
  float a = 0;
  if (const Texture* t = e.covers.get(path, art, art, 0, e.now, &a)) e.c.image(*t, ar, {0, 0, 1, 1}, e.dpf(3), withAlpha(white, a));
  e.c.layer(7);
  float ix = r.x + r.w - e.dp(12) - icon;
  e.icons.draw(e.c, s.playing ? Icon::Pause : Icon::Play, {ix, rowY + float((int(rowH) - icon) / 2), float(icon), float(icon)}, white);
  float cx = ax + art + e.dp(8), cw = ix - e.dp(8) - cx;
  SongInfo song = songInfo(path);
  Label t = label(e, song.title, 12, true, cw), sub = label(e, firstAndRest(song.subtitle).first, 10, false, cw);
  int colH = t.h + e.dp(1) + sub.h;
  float ty = rowY + float((int(rowH) - colH) / 2);
  drawLabel(e, t, cx, ty, white);
  drawLabel(e, sub, cx, ty + t.h + e.dp(1), e.p.darkMuted);
}

bool MusicListScreen::onAction(Action a, int steps) {
  switch (a) {
    case Action::Down: return list_.down(steps);
    case Action::Up: return list_.up(steps);
    case Action::Select: return list_.select();
    case Action::LongSelect: return list_.longSelect();
    default: return false;
  }
}

bool MusicListScreen::step(float dt) { return list_.step(dt) || ambient_.fading(now_); }

bool MusicListScreen::tick(int64_t now) {
  now_ = now;
  // the mini player's song, state and progress line (1.0.x's onTick)
  PlayerState s = g_player.state();
  char sig[64];
  snprintf(sig, sizeof(sig), "%d %d %d %lld", s.index, s.playing, s.count,
           (long long)(s.durationMs > 0 ? s.positionMs * 480 / s.durationMs : 0));
  std::string v = std::string(s.path) + sig;
  if (v == miniSig_) return false;
  miniSig_ = v;
  return true;
}

void MusicListScreen::draw(Env& e, float w, float h) {
  now_ = e.now;
  const Row* sel = list_.selectedRow();
  std::string thumb;
  if (sel) {
    Row filled = *sel;
    if (list_.fill && !filled.header) list_.fill(list_.selected(), filled);
    thumb = filled.thumb;
  }
  ambient_.follow(e, thumb, e.dp(34));
  ambient_.draw(e, {0, 0, w, h});
  darkHeader(e, w, title_, HeaderLogo::Music);
  float top = float(e.dp(31));
  bool mini = miniPlayerShown();
  float bottom = mini ? float(e.dp(kMiniPlayer)) : 0;
  list_.layout(e, w, h - top - bottom);
  list_.draw(e, 0, top);
  if (mini) miniPlayer(e, {0, h - bottom, w, bottom});
  e.c.layer(0);
}

namespace {

// QueueScreen: Up next, with shuffle and repeat (Now Playing's options).
class QueueScreen : public MusicListScreen {
 public:
  QueueScreen() : MusicListScreen("Up next") {
    PlayerState s = g_player.state();
    shuffle_ = s.shuffle != 0, repeat_ = s.repeat;
    render();
  }

 private:
  void render() {
    if (!g_player.active()) return list_.setMessage("Nothing playing");
    PlayerState s = g_player.state();
    current_ = s.index;
    std::vector<Row> rows;
    Row sh;
    sh.title = "Shuffle";
    sh.trailing = shuffle_ ? "On" : "Off";
    sh.onSelect = [this] {
      shuffle_ = !shuffle_;
      g_player.setShuffle(shuffle_);
      render();
    };
    rows.push_back(sh);
    Row rp;
    rp.title = "Repeat";
    rp.trailing = repeat_ == 1 ? "All" : repeat_ == 2 ? "One" : "Off";
    rp.onSelect = [this] {
      repeat_ = repeat_ == 0 ? 1 : repeat_ == 1 ? 2 : 0;
      g_player.setRepeat(repeat_);
      render();
    };
    rows.push_back(rp);
    const auto& q = g_player.queue();
    Row head;
    head.header = true;
    head.title = "Queue \xc2\xb7 " + std::to_string(q.size()) + " songs";
    rows.push_back(head);
    for (size_t i = 0; i < q.size(); i++) {
      Row r;
      r.onSelect = [i] {
        g_player.jumpTo(int(i));
        g_app->pop();
      };
      rows.push_back(std::move(r));
    }
    first_ = 3;
    // 821 songs: each row's text and artwork are filled in when it is drawn
    list_.fill = [this](int i, Row& r) {
      if (i < first_) return;
      size_t k = size_t(i - first_);
      const auto& queue = g_player.queue();
      if (k >= queue.size()) return;
      SongInfo song = songInfo(queue[k]);
      r.title = song.title;
      r.subtitle = song.subtitle;
      r.thumb = queue[k];
      if (int(k) == current_) {
        r.trailing = "Playing";
      } else if (const Track* t = g_library.find(queue[k])) {
        r.trailing = t->durationSec >= 0 ? formatTime(t->durationSec) : "";
      }
    };
    list_.setRows(std::move(rows), true);
  }

  bool shuffle_ = false;
  int repeat_ = 0, current_ = -1, first_ = 3;
};

}  // namespace

std::unique_ptr<Screen> makeQueue() { return std::unique_ptr<Screen>(new QueueScreen()); }

}  // namespace rondo::shell
