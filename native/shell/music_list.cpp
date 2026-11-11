// The YouTube Music building blocks and Up next; see music_list.h.
#include "music_list.h"

#include "downloads.h"

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
    else if (logo == HeaderLogo::Video) e.icons.videoLogo(e.c, {r.x, r.y + float((s - e.dp(11)) / 2), float(s), float(e.dp(11))});
    else e.icons.draw(e.c, Icon::Wifi, r, white);
    x += s + e.dp(7);
  }
  // YouTube's lists: the battery alone (as its home and Shorts); the others the whole group
  float iconsLeft = logo == HeaderLogo::Video ? batteryIcon(e, w - pad, h, e.p.darkMuted) : statusIcons(e, w - pad, h, e.p.darkMuted);
  Label t = label(e, title, 14, true, iconsLeft - x);
  drawLabel(e, t, x, float((h - t.h) / 2), titleColor);
}

void Ambient::follow(Env& e, const std::string& thumb, int artPx, int artPxH) {
  bool has = false;
  Color c = 0;
  if (!thumb.empty()) {
    if (!artColor(e, thumb, artPx, &c, artPxH)) return;  // not decoded yet: the current color stays
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

Color Ambient::colorAt(Env& e, float y, float h) const {
  auto at = [&](bool has, Color c) -> Color {
    if (!has) return e.p.dark;
    float half = floorf(h / 2);
    return y < half ? mix(darker(c, 0.55f), darker(c, 0.22f), y / half) : mix(darker(c, 0.22f), e.p.dark, (y - half) / (h - half));
  };
  Color now = at(has_, shown_);
  return fade_.running(e.now) ? mix(at(hadFrom_, from_), now, fade_.at(e.now)) : now;
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

namespace {
bool isContainer(const std::string& kind) { return kind == "playlist" || kind == "album" || kind == "artist"; }
}  // namespace

std::string httpProblem(int status, const std::string& path) {
  return status == 0 ? (directMode() ? "couldn't reach YouTube" : "couldn't reach the server") : "HTTP " + std::to_string(status) + " for " + path;
}

std::string offlineOr(const std::string& problem) {
  if (!directMode() && g_config.server.url.empty()) return "No music server yet.\nConnect one in Settings \xe2\x80\xba Server.";
  if (!g_app->status().online) return "No Wi-Fi connection.\nConnect in Settings \xe2\x80\xba Wi-Fi, then press center.";
  return problem + "\nPress center to try again";
}

void openMusicItem(const Item& item, const std::vector<Item>& siblings) {
  if (isContainer(item.kind)) return openMusicContainer(item);
  std::vector<Item> playable;
  int at = -1;
  for (const Item& s : siblings)
    if (!isContainer(s.kind)) {
      if (at < 0 && s.id == item.id) at = int(playable.size());  // the first copy, as indexOf
      playable.push_back(s);
    }
  if (at < 0) at = 0;
  playStreams(playable, at);
  openNowPlaying();
}

std::vector<Row> musicRows(const std::vector<Item>& items, std::function<std::vector<Item>()> queue) {
  std::vector<Row> rows;
  for (const Item& it : items) {
    Row r;
    r.title = it.title;
    r.subtitle = it.subtitle;
    bool container = isContainer(it.kind);
    r.trailing = container || it.durationSec < 0 ? "" : formatTime(it.durationSec);
    r.thumb = it.thumb;
    r.chevron = container;
    Item item = it;
    r.onSelect = [item, queue] { openMusicItem(item, queue()); };
    if (!container) r.onLongSelect = [item] { openSongMenu(item); };
    rows.push_back(std::move(r));
  }
  return rows;
}

std::vector<Row> videoRows(const std::vector<Item>& items) {
  std::vector<Row> rows;
  for (const Item& it : items) {
    Row r;
    r.title = it.title;
    r.subtitle = it.subtitle;
    r.trailing = it.durationSec < 0 ? "" : formatTime(it.durationSec);
    r.thumb = it.thumb;
    Item item = it;
    r.onSelect = [item] { openVideo(item); };
    if (!it.id.empty() && it.id[0] != '/') r.onLongSelect = [item] { askDownload(item); };
    rows.push_back(std::move(r));
  }
  return rows;
}

void askDownload(const Item& item) {
  if (!downloadedFile(item.id).empty()) return g_app->toast("Already on the card");
  int now = g_prefs.videoQuality ? g_prefs.videoQuality : 480;
  g_app->push(makeChoice(
      "Download", {{"720p", "720"}, {"480p", "480"}, {"360p", "360"}}, [now] { return std::to_string(now); },
      [item](const std::string& v) { downloadVideo(item, atoi(v.c_str())); },
      [](const std::string& v) { return std::string(v == "720" ? "the most room" : v == "360" ? "the least room" : ""); }));
}

bool miniPlayerShown() {
  PlayerState s = g_player.state();
  return s.index >= 0 && s.path[0] && !s.video;  // a song, not a video (1.0.x: !isVideo)
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
  if (const Texture* t = e.covers.get(artOf(path, 0), art, art, 0, e.now, &a)) e.c.image(*t, ar, {0, 0, 1, 1}, e.dpf(3), withAlpha(white, a));
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

void TabbedListScreen::focusTabsFromTop() {
  list_.onTopReached = [this] {
    focused_ = active_;
    list_.active = false;
    return true;
  };
}

bool TabbedListScreen::tick(int64_t now) {
  bool tabs = syncTabs();
  return MusicListScreen::tick(now) || tabs;
}

bool TabbedListScreen::syncTabs() {
  std::vector<std::string> want = wantedTabs();
  if (want == tabs_) return false;
  tabs_ = want;
  int search = int(tabs_.size());
  if (focused_ > search) focused_ = search;
  if (active_ >= search) load(0);  // its tab went: Home
  return true;
}

bool TabbedListScreen::onAction(Action a, int steps) {
  if (focused_ < 0) {
    if (list_.rows().empty() && a == Action::Select) {  // a message: center tries again
      load(active_);
      return true;
    }
    return MusicListScreen::onAction(a, steps);
  }
  int search = int(tabs_.size());
  switch (a) {
    case Action::Up: focused_ = (focused_ + search) % (search + 1); return true;
    case Action::Down: focused_ = (focused_ + 1) % (search + 1); return true;
    case Action::Select: {
      int f = focused_;
      focused_ = -1;
      list_.active = true;
      if (f == search) openSearch();
      else load(f);
      return true;
    }
    default: return false;
  }
}

float TabbedListScreen::drawPills(Env& e, float x, int H, float screenH) {
  int ph = e.dp(21), py = (H - ph) / 2;
  // Pill (Widgets.kt): GradientDrawable, rounded 12 units, white when active, else white at 60/255;
  // focused, a 2-unit red stroke over it. Inside the ring the fill is drawn over what is behind the
  // header there (the ambient color), as Android blends it.
  const Color translucent = 0x3cffffff;
  Color behind = ambient_.colorAt(e, float(H) / 2, screenH);
  Color inside = mix(behind, rgb(0xffffff), 60.f / 255.f);
  auto pill = [&](Rect r, bool active, bool ring) {
    float radius = std::min(e.dpf(12), r.h / 2);
    e.c.layer(2);
    if (ring) {
      int t = e.dp(2);
      e.c.shape(r, radius, e.p.red);
      e.c.shape({r.x + t, r.y + t, r.w - 2 * t, r.h - 2 * t}, std::max(0.f, radius - t), active ? rgb(0xffffff) : inside);
    } else {
      e.c.shape(r, radius, active ? rgb(0xffffff) : translucent);
    }
  };
  for (int i = 0; i < int(tabs_.size()); i++) {
    bool active = i == active_;
    Label l = label(e, tabs_[size_t(i)], 12.5f, active);
    int pw = l.w + 2 * e.dp(10);
    pill({x, float(py), float(pw), float(ph)}, active, i == focused_);
    e.c.layer(4);
    drawLabel(e, l, centerText(x, float(pw), l.w), float(py + ((ph - l.h) >> 1)), active ? e.p.dark : rgb(0xffffff));
    x += pw + e.dp(5);
  }
  // the search button: a round pill with the magnifier
  pill({x, float(py), float(ph), float(ph)}, false, focused_ == int(tabs_.size()));
  e.c.layer(4);
  int s = e.dp(11);
  e.icons.draw(e.c, Icon::Search, {x + float((ph - s) / 2), float(py + (ph - s) / 2), float(s), float(s)}, rgb(0xffffff));
  return x + ph;
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
    sh.tile = &tiles::shuffle;
    sh.onSelect = [this] {
      shuffle_ = !shuffle_;
      g_player.setShuffle(shuffle_);
      render();
    };
    rows.push_back(sh);
    Row rp;
    rp.title = "Repeat";
    rp.trailing = repeat_ == 1 ? "All" : repeat_ == 2 ? "One" : "Off";
    rp.tile = &tiles::repeat;
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
      r.thumb = artOf(queue[k], 0);  // a streamed song's thumbnail, a card song's file
      if (int(k) == current_) {
        r.trailing = "Playing";
      } else if (const Item* it = isStream(queue[k]) ? streamItem(queue[k]) : nullptr) {
        r.trailing = it->durationSec >= 0 ? formatTime(it->durationSec) : "";
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
