// Now Playing (NowPlayingScreen.kt in 1.0.x, NowPlaying.dc.html): the artwork over a blurred field of
// itself, the song, up next, shuffle and repeat, the progress and the times. Like an iPod the wheel is
// volume. Center switches to the lyrics and back; holding center opens options (scrub with the wheel,
// up next, shuffle, repeat); holding Next/Prev scrubs faster the longer it is held (Scrubber).
//
// The layout is 1.0.x's view tree worked out at its sizes: a 24-unit header, the body (140-unit
// artwork, the text column beside it) and the progress with the times under it.
#include <math.h>
#include <string.h>

#include <pthread.h>
#include <sys/resource.h>

#include <unordered_map>

#include "../core/lyrics.h"
#include "app.h"
#include "lyrics_view.h"
#include "music_list.h"
#include "screens.h"

namespace rondo::shell {

void progressBar(Env& e, Rect r, Color track, Color played, float fraction, int thumbDp, int barDp, float marker) {
  fraction = std::max(0.f, std::min(1.f, fraction));
  float bar = e.dpf(float(barDp)), rad = e.dpf(float(thumbDp)) / 2;
  float left = std::max(rad, e.dpf(6)), right = r.w - left;
  float top = (r.h - bar) / 2;
  float x = left + (right - left) * fraction;
  auto pill = [&](float x0, float x1, Color c) {
    float w = x1 - x0;
    if (w <= 0) return;
    e.c.shape({r.x + x0, r.y + top, w, bar}, std::min(bar / 2, w / 2), c);
  };
  auto circle = [&](float cx, float rr, Color c) {
    e.c.shape({r.x + cx - rr, r.y + r.h / 2 - rr, 2 * rr, 2 * rr}, rr, c);
  };
  pill(left, right, track);
  pill(left, x, played);
  if (rad > 0) circle(x, rad, played);
  if (marker >= 0) {  // the scrub target: a larger white ring with a red center
    float mx = left + (right - left) * std::min(1.f, marker);
    float base = std::max(rad, e.dpf(6));
    circle(mx, base * 1.25f, rgb(0xffffff));
    circle(mx, base * 0.7f, e.p.red);
  }
}

// VolumeOverlay: a 230 x 50 panel in the middle of the screen, the icon, the level as a bar, the number.
void App::drawVolume(Env& e, float W, float H) {
  int pw = e.dp(230), ph = e.dp(50);
  float px = float((int(W) - pw) / 2), py = float((int(H) - ph) / 2);
  e.c.layer(7);
  e.c.shape({px, py, float(pw), float(ph)}, e.dpf(10), 0xeb212121);
  int pad = e.dp(14), icon = e.dp(22), labelW = e.dp(30);
  float ix = px + pad;
  e.icons.draw(e.c, Icon::Volume, {ix, py + float((ph - icon) / 2), float(icon), float(icon)}, rgb(0xffffff));
  float bx = ix + icon + e.dp(10);
  float bw = px + pw - pad - labelW - e.dp(8) - bx;
  int bh = e.dp(14);
  progressBar(e, {bx, py + float((ph - bh) / 2), bw, float(bh)}, e.p.darkTrack, rgb(0xffffff),
              volumeMax_ > 0 ? float(volumeLevel_) / volumeMax_ : 0, 0, 5);
  char n[8];
  snprintf(n, sizeof(n), "%d", volumeLevel_);
  Label l = label(e, n, 15, true);
  float lx = bx + bw + e.dp(8);
  drawLabel(e, l, lx + labelW - l.w, py + float((ph - l.h) / 2), rgb(0xffffff));
  e.c.layer(0);
}

namespace {
constexpr Color white = 0xffffffff;
inline Color whiteA(int a) { return uint32_t(a) << 24 | 0xffffff; }
}  // namespace

SongInfo songInfo(const std::string& path) {
  SongInfo s;
  if (isStream(path)) {
    if (const Item* it = streamItem(path)) s.title = it->title, s.subtitle = it->subtitle;
    return s;
  }
  if (const Track* t = g_library.find(path)) {
    s.title = t->title;
    s.subtitle = t->artist;
    if (!t->album.empty()) s.subtitle += (s.subtitle.empty() ? "" : " \xc2\xb7 ") + t->album;
  } else {
    size_t slash = path.rfind('/');
    s.title = path.substr(slash == std::string::npos ? 0 : slash + 1);
  }
  return s;
}
std::pair<std::string, std::string> firstAndRest(const std::string& subtitle) {
  static const std::string dot = " \xc2\xb7 ";
  size_t at = subtitle.find(dot);
  if (at == std::string::npos) return {subtitle, ""};
  return {subtitle.substr(0, at), subtitle.substr(at + dot.size())};
}

// SkRGBToHSV and SkHSVToColor, as Color.RGBToHSV and Color.HSVToColor call them
static void rgbToHsv(int r, int g, int b, float hsv[3]) {
  int mn = std::min(r, std::min(g, b)), mx = std::max(r, std::max(g, b)), delta = mx - mn;
  hsv[2] = mx / 255.f;
  if (delta == 0) {
    hsv[0] = hsv[1] = 0;
    return;
  }
  hsv[1] = float(delta) / mx;
  float h = r == mx ? float(g - b) / delta : g == mx ? 2 + float(b - r) / delta : 4 + float(r - g) / delta;
  h *= 60;
  if (h < 0) h += 360;
  hsv[0] = h;
}
static Color hsvToColor(const float hsv[3]) {
  float s = std::max(0.f, std::min(1.f, hsv[1])), v = std::max(0.f, std::min(1.f, hsv[2]));
  int vb = int(lroundf(v * 255));
  if (fabsf(s) < 1.f / 4096) return rgb(uint32_t(vb) << 16 | uint32_t(vb) << 8 | uint32_t(vb));
  float hx = hsv[0] < 0 || hsv[0] >= 360 ? 0 : hsv[0] / 60;
  float w = floorf(hx), f = hx - w;
  int p = int(lroundf((1 - s) * v * 255)), q = int(lroundf((1 - s * f) * v * 255)), t = int(lroundf((1 - s * (1 - f)) * v * 255));
  int r, g, b;
  switch (int(w)) {
    case 0: r = vb, g = t, b = p; break;
    case 1: r = q, g = vb, b = p; break;
    case 2: r = p, g = vb, b = t; break;
    case 3: r = p, g = q, b = vb; break;
    case 4: r = t, g = p, b = vb; break;
    default: r = vb, g = p, b = q; break;
  }
  return rgb(uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b));
}

float statusIconsWidth(Env& e, bool showWifi) {
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", int(floorf(g_app->status().battery * 100 + 0.5f)));
  return float((showWifi ? e.dp(11) + e.dp(5) : 0) + label(e, pct, 10, true).w + e.dp(3) + e.dp(18));
}

float statusIcons(Env& e, float right, int rowH, Color c, bool showWifi, bool hidePercent) {
  const Status& st = g_app->status();
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", int(floorf(st.battery * 100 + 0.5f)));
  Label pl = label(e, hidePercent ? "" : pct, 10, true);
  if (hidePercent) pl.w = -e.dp(3);  // the percentage and its margin gone
  int wifi = showWifi ? e.dp(11) + e.dp(5) : 0, bw = e.dp(18), bh = e.dp(11);
  int gw = wifi + pl.w + e.dp(3) + bw, gh = std::max({showWifi ? e.dp(11) : 0, pl.h, bh});
  float gx = right - gw, gy = float((rowH - gh) / 2);
  if (showWifi) {
    int s = e.dp(11);
    e.c.pushAlpha(st.online ? 1 : 0.3f);
    e.icons.draw(e.c, Icon::Wifi, {gx, gy + float((gh - s) / 2), float(s), float(s)}, c);
    e.c.popAlpha();
  }
  if (!hidePercent) drawLabel(e, pl, gx + wifi, gy + float((gh - pl.h) / 2), c);
  e.icons.battery(e.c, {gx + wifi + pl.w + e.dp(3), gy + float((gh - bh) / 2), float(bw), float(bh)}, c, st.battery);
  return gx;
}

bool artColor(Env& e, const std::string& path, int sourcePx, Color* out) {
  static std::unordered_map<std::string, Color> known;
  std::string key = path + '\n' + std::to_string(sourcePx);
  auto it = known.find(key);
  if (it != known.end()) return *out = it->second, true;
  // the artwork as the view decodes it, then Bitmap.createScaledBitmap(bitmap, 12, 12, true)
  const uint8_t* px = e.covers.pixels(path, 12, 12, -sourcePx);
  if (!px) return false;
  long r = 0, g = 0, b = 0;
  for (int i = 0; i < 144; i++) r += px[i * 4], g += px[i * 4 + 1], b += px[i * 4 + 2];
  float hsv[3];
  rgbToHsv(int(r / 144), int(g / 144), int(b / 144), hsv);
  hsv[1] = std::min(hsv[1] * 1.4f, 0.85f);
  hsv[2] = std::max(0.35f, std::min(hsv[2], 0.6f));
  Color c = hsvToColor(hsv);
  known[key] = c;
  *out = c;
  return true;
}

namespace {
std::string upperExt(const std::string& path) {
  size_t dot = path.rfind('.');
  std::string x = dot == std::string::npos ? path : path.substr(dot + 1);
  for (char& c : x) c = char(toupper((unsigned char)c));
  return x;
}

// Scrubber.kt: seeking that scales with the length. The target is previewed (bubble and marker) and
// playback jumps once: on center, when Next/Prev is let go, or 1.2 s after the last wheel turn. Menu
// cancels.
class Scrubber {
 public:
  bool active = false;
  int64_t target = 0, from = 0;  // ms

  float fraction(int64_t duration) const { return active && duration > 0 ? float(target) / duration : -1; }
  bool begin() {
    PlayerState s = g_player.state();
    if (s.durationMs <= 0) return false;
    active = true, holding_ = false, moved_ = false;
    duration_ = s.durationMs;
    from = target = s.positionMs;
    last_ = now_ns();
    return true;
  }
  void turn(int dir, int steps) {
    if (!active && !begin()) return;
    int64_t step = std::max<int64_t>(1000, std::min<int64_t>(60000, duration_ / 150)) * (steps > 1 ? 4 : 1);
    move(dir * step);
  }
  void jump(int dir) {
    if (!active && !begin()) return;
    move(dir * std::max<int64_t>(duration_ / 10, 5000));
  }
  void hold(int dir) {  // one repeat of a held Next/Prev
    if (!holding_) {
      if (!active && !begin()) return;
      holding_ = true;
      repeats_ = 0;
    }
    repeats_++;
    double share = repeats_ <= 5 ? 0.01 : repeats_ <= 10 ? 0.02 : 0.04;
    move(dir * std::max<int64_t>(int64_t(duration_ * share), 2000));
  }
  void commit() {
    if (!active) return;
    if (moved_) g_player.seekTo(target);
    active = holding_ = false;
  }
  void cancel() { active = holding_ = false; }
  // jumps after the wheel has rested (not while a button is held); an unused scrub closes after 4 s
  int64_t deadline() const { return active && !holding_ ? last_ + (moved_ ? 1200 : 4000) * kMs : 0; }
  bool tick(int64_t now) {
    int64_t d = deadline();
    if (d && now >= d) {
      commit();
      return true;
    }
    return false;
  }

 private:
  void move(int64_t delta) {
    moved_ = true;
    target = std::max<int64_t>(0, std::min(target + delta, std::max<int64_t>(0, duration_ - 1000)));
    last_ = now_ns();
  }
  bool holding_ = false, moved_ = false;
  int repeats_ = 0;
  int64_t duration_ = 0, last_ = 0;
};

// Reads lyrics off the drawing thread: reaching a FLAC's tags past its cover takes about 40 ms on the card.
class LyricsLoader {
 public:
  void request(const std::string& path) {
    pthread_mutex_lock(&mu_);
    want_ = path;
    if (!started_) {
      started_ = true;
      pthread_t t;
      pthread_create(&t, nullptr, run, this);
      pthread_detach(t);
    }
    pthread_cond_signal(&cv_);
    pthread_mutex_unlock(&mu_);
  }
  // the lyrics read for `path`, once they are
  bool take(const std::string& path, Lyrics* out) {
    pthread_mutex_lock(&mu_);
    bool ok = ready_ && donePath_ == path;
    if (ok) *out = std::move(done_), ready_ = false;
    pthread_mutex_unlock(&mu_);
    return ok;
  }

 private:
  static void* run(void* self) {
    setpriority(PRIO_PROCESS, 0, 10);
    auto* l = static_cast<LyricsLoader*>(self);
    pthread_mutex_lock(&l->mu_);
    for (;;) {
      while (l->want_.empty()) pthread_cond_wait(&l->cv_, &l->mu_);
      std::string path = l->want_;
      l->want_.clear();
      pthread_mutex_unlock(&l->mu_);
      Lyrics found = lyricsForFile(path);
      pthread_mutex_lock(&l->mu_);
      l->done_ = std::move(found);
      l->donePath_ = path;
      l->ready_ = true;
    }
    return nullptr;
  }
  pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cv_ = PTHREAD_COND_INITIALIZER;
  bool started_ = false, ready_ = false;
  std::string want_, donePath_;
  Lyrics done_;
};
LyricsLoader g_lyricsLoader;

class NowPlayingScreen : public Screen {
 public:
  // "Open lyrics automatically" (dashboard) applies when Now Playing opens; the page stays for the next visit
  NowPlayingScreen() { showLyrics(lyricsOn() || g_config.behavior.lyricsOnOpen); }
  std::string title() const override { return "Now Playing"; }
  bool statusBar() const override { return false; }

  bool onAction(Action a, int steps) override {
    if (options_) {
      int n = 4;
      switch (a) {
        case Action::Up: cursor_ = (cursor_ + n - 1) % n; break;
        case Action::Down: cursor_ = (cursor_ + 1) % n; break;
        case Action::Select: chooseOption(); break;
        case Action::Back:
        case Action::LongSelect: options_ = false; break;
        default: return false;
      }
      return true;
    }
    bool scrubbing = scrub_.active;
    switch (a) {
      case Action::Up:
      case Action::Down: {
        int dir = a == Action::Down ? 1 : -1;
        if (scrubbing) scrub_.turn(dir, steps);
        else if (lyricsOn() && lyrics_.scrollable()) lyrics_.scrollBy(dir * steps);
        else g_app->changeVolume(dir);
        return true;
      }
      case Action::Select:
        // center: Now Playing <-> lyrics (or, while scrubbing, jump)
        if (scrubbing) scrub_.commit();
        else showLyrics(!lyricsOn());
        return true;
      case Action::LongSelect:
        scrub_.cancel();
        cursor_ = 0;
        options_ = true;
        return true;
      case Action::Back:
        if (scrubbing) {
          scrub_.cancel();
          return true;
        }
        if (lyricsOn()) {
          showLyrics(false);
          return true;
        }
        return false;
      case Action::Next:
        if (!scrubbing) return false;
        scrub_.jump(+1);
        return true;
      case Action::Prev:
        if (!scrubbing) return false;
        scrub_.jump(-1);
        return true;
      case Action::LongNext: scrub_.hold(+1); return true;
      case Action::LongPrev: scrub_.hold(-1); return true;
      case Action::SeekEnd: scrub_.commit(); return true;
      default: return false;
    }
  }

  bool tick(int64_t now) override {
    now_ = now;
    bool changed = scrub_.tick(now);
    PlayerState s = g_player.state();
    Shown v = shownOf(s);
    if (!(v == shown_)) changed = true;
    if (lyricsOn()) {
      std::string path = s.index >= 0 ? std::string(s.path) : std::string();
      if (path != lyricsFor_) loadLyrics(path), changed = true;  // the next song
      Lyrics found;
      if (loadPending_ && g_lyricsLoader.take(lyricsFor_, &found)) {
        loadPending_ = false;
        remember(lyricsFor_, found);
        lyrics_.show(found);
        changed = true;
      }
      if (lyrics_.layoutSome(3 * kMs)) changed = true;
      changed |= lyrics_.update(PlayerHost::positionAt(s, now), s.durationMs, now);
    }
    return changed;
  }
  bool step(float) override { return lyricsOn() && lyrics_.animating(now_); }
  int64_t wakeAt() const override {
    int64_t t = scrub_.deadline();
    auto sooner = [&](int64_t w) { if (w && (!t || w < t)) t = w; };
    if (lyricsOn()) {
      if (loadPending_) sooner(now_ns() + 20 * kMs);  // the loader's answer
      if (lyrics_.layingOut()) sooner(now_ns());  // the next slice of the layout
      PlayerState s = g_player.state();
      int64_t now = now_ns();
      if (s.playing) sooner(lyrics_.wakeAt(PlayerHost::positionAt(s, now), now));
    }
    return t;
  }

  void draw(Env& e, float w, float h) override {
    PlayerState s = g_player.state();
    shown_ = shownOf(s);
    const Color dim = 0x78000000;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, e.p.dark);
    std::string path = s.index >= 0 ? std::string(s.path) : std::string();
    int art = e.dp(140);
    if (!path.empty()) {
      // the backdrop: the artwork shrunk to 6 x 6 pixels, stretched over the screen (ArtColor.blurred)
      float a = 0;
      if (const Texture* t = e.covers.get(artOf(path, 400), 6, 6, -art, e.now, &a)) e.c.image(*t, {0, 0, w, h}, {0, 0, 1, 1}, 0, withAlpha(white, a));
    }
    e.c.fill({0, 0, w, h}, dim);
    if (lyricsOn()) {
      drawLyricsPage(e, w, h, s, path);
      if (scrub_.active) drawBubble(e, w, h, scrub_.target, scrub_.from);
      if (options_) drawOptions(e, w, h, s);
      e.c.layer(0);
      return;
    }

    // ---- header: NOW PLAYING, "3 of 12", the status icons ----
    int headH = e.dp(24);
    e.c.layer(4);
    Label np = label(e, "NOW PLAYING", 11, true);
    float hx = float(e.dp(12));
    drawLabel(e, np, hx, float((headH - np.h) / 2), whiteA(200));
    float iconsLeft = statusIcons(e, w - e.dp(10), headH, whiteA(200));
    if (s.count > 1) {
      char pos[32];
      snprintf(pos, sizeof(pos), "%d of %d", s.index + 1, s.count);
      float px = hx + np.w + e.dp(8);
      Label pl = label(e, pos, 11, false, iconsLeft - px);
      drawLabel(e, pl, px, float((headH - pl.h) / 2), whiteA(160));
    }

    // ---- body: the artwork and the column beside it ----
    float ax = float(e.dp(12)), ay = float(headH + e.dp(4));
    Rect ar{ax, ay, float(art), float(art)};
    int note = e.dp(44);
    e.icons.draw(e.c, Icon::Note, {ax + float((art - note) / 2), ay + float((art - note) / 2), float(note), float(note)},
                 whiteA(120));
    e.c.layer(5);
    e.c.shape(ar, e.dpf(6), whiteA(70));
    if (!path.empty()) {
      float a = 0;
      if (const Texture* t = e.covers.get(artOf(path, 400), art, art, 0, e.now, &a)) {
        e.c.image(*t, ar, {0, 0, 1, 1}, e.dpf(6), withAlpha(white, a));
      }
    }
    e.c.layer(6);
    float cx = ax + art + e.dp(14), cw = w - e.dp(12) - cx;
    float cy = ay + e.dp(2), bottom = ay + art;
    SongInfo song = path.empty() ? SongInfo{"Nothing playing", ""} : songInfo(path);
    auto parts = firstAndRest(song.subtitle);
    for (const Label& l : wrap(e, song.title, 17, true, cw, 2)) {
      drawLabel(e, l, cx, cy, white);
      cy += l.h;
    }
    Label artist = label(e, parts.first, 13, false, cw);
    cy += e.dp(4);
    drawLabel(e, artist, cx, cy, whiteA(230));
    cy += artist.h + e.dp(2);
    Label album = label(e, parts.second, 11, false, cw);
    drawLabel(e, album, cx, cy, whiteA(160));
    // from the bottom: the modes row, the song up next, "UP NEXT"
    int iconW = e.dp(15), iconH = e.dp(13);
    std::string source = path.empty() ? "" : isStream(path) ? "Streaming \xc2\xb7 AAC" : upperExt(path) + " \xc2\xb7 Library";
    float srcX = cx + iconW + e.dp(12) + iconW;
    Label src = label(e, source, 10, false, cx + cw - srcX);
    int modesH = std::max(iconH, src.h);
    float my = bottom - modesH;
    e.icons.draw(e.c, Icon::Shuffle, {cx, my + float((modesH - iconH) / 2), float(iconW), float(iconH)},
                 s.shuffle ? white : whiteA(90));
    e.icons.draw(e.c, Icon::Repeat, {cx + iconW + e.dp(12), my + float((modesH - iconH) / 2), float(iconW), float(iconH)},
                 s.repeat ? white : whiteA(90));
    drawLabel(e, src, cx + cw - src.w, my + float((modesH - src.h) / 2), whiteA(170));
    std::string next = "\xe2\x80\x94";
    if (s.nextIndex >= 0 && s.nextIndex < int(g_player.queue().size())) {
      SongInfo n = songInfo(g_player.queue()[size_t(s.nextIndex)]);
      std::string by = firstAndRest(n.subtitle).first;
      next = n.title;
      if (!by.empty()) next = next.empty() ? by : next + " \xc2\xb7 " + by;
    }
    Label nl = label(e, next, 11, false, cw);
    float ny = my - e.dp(8) - nl.h;
    drawLabel(e, nl, cx, ny, whiteA(220));
    Label up = label(e, "UP NEXT", 9, true, cw);
    drawLabel(e, up, cx, ny - e.dp(1) - up.h, whiteA(150));

    // ---- bottom: the progress and the times ----
    int64_t d = s.durationMs;
    int64_t pos = scrub_.active ? scrub_.target : s.positionMs;
    Label el = label(e, formatTime(int(pos / 1000)), 11, false);
    float timesH = float(el.h);
    float by = h - e.dp(6) - timesH;
    float barH = float(e.dp(12));
    float barY = by - e.dp(2) - barH;
    float bx = float(e.dp(10)), bw = w - 2 * bx;
    e.c.layer(5);
    progressBar(e, {bx, barY, bw, barH}, whiteA(70), white, d > 0 ? float(s.positionMs) / d : 0, 9, 3,
                scrub_.fraction(d));
    e.c.layer(6);
    float tx = bx + e.dp(4), tw = bw - 2 * e.dp(4);
    // weights 1 : 2 : 1, shared out as LinearLayout does
    int total = int(tw), w1 = total * 1 / 4, w2 = (total - w1) * 2 / 3, w3 = total - w1 - w2;
    drawLabel(e, el, tx, by, whiteA(220));
    Label rem = label(e, "-" + formatTime(int(std::max<int64_t>(0, d / 1000 - pos / 1000))), 11, false);
    drawLabel(e, rem, tx + w1 + w2 + w3 - rem.w, by, whiteA(220));
    std::string state = scrub_.active ? "Turn to move \xc2\xb7 center jumps"
                        : s.buffering ? "Loading"
                        : s.playing   ? "Playing"
                                      : "Paused";
    int ic = e.dp(9), gap = e.dp(5);
    Label st = label(e, state, 11, false);
    int groupW = (scrub_.active ? 0 : ic + gap) + st.w;
    int groupH = std::max(scrub_.active ? 0 : ic, st.h);
    float gx = tx + w1 + float((w2 - groupW) / 2);
    float gy = by + float((int(timesH) - groupH) / 2);
    if (!scrub_.active) {
      e.icons.draw(e.c, s.playing ? Icon::Play : Icon::Pause, {gx, gy + float((groupH - ic) / 2), float(ic), float(ic)}, white);
      gx += ic + gap;
    }
    drawLabel(e, st, gx, gy + float((groupH - st.h) / 2), white);

    if (scrub_.active) drawBubble(e, w, h, scrub_.target, scrub_.from);
    if (options_) drawOptions(e, w, h, s);
    e.c.layer(0);
  }

 private:
  static bool& lyricsOn() {
    static bool on = false;  // the lyrics page stays open for the next visit to Now Playing
    return on;
  }
  void showLyrics(bool on) {
    lyricsOn() = on;
    if (on) {
      PlayerState s = g_player.state();
      loadLyrics(s.index >= 0 ? std::string(s.path) : std::string());
    }
  }
  void loadLyrics(const std::string& path) {
    if (path.empty()) {
      lyricsFor_.clear();
      loadPending_ = false;
      lyrics_.showMessage("Nothing playing");
      return;
    }
    if (path == lyricsFor_) return;
    lyricsFor_ = path;
    for (auto& c : cache())
      if (c.first == path) {
        loadPending_ = false;
        lyrics_.show(c.second);
        return;
      }
    lyrics_.showMessage("Finding lyrics\xe2\x80\xa6");
    if (isStream(path)) return loadOnline(path);
    loadPending_ = true;
    g_lyricsLoader.request(path);
  }
  // a YouTube Music song: the server's /lyrics (LyricsSource.find), kept 30 minutes
  void loadOnline(const std::string& path) {
    const Item* it = streamItem(path);
    if (!it) return lyrics_.showMessage("No lyrics found for this song");
    std::string q = "/lyrics?title=" + Api::escape(it->title) + "&artist=" + Api::escape(firstAndRest(it->subtitle).first);
    if (it->durationSec >= 0) q += "&duration=" + std::to_string(it->durationSec);
    q += "&id=" + Api::escape(it->id);
    requests_.get(q, 30 * 60000, [this, path, q](int status, const std::string& body) {
      if (path != lyricsFor_) return;
      Lyrics l;
      if (status == 200 && parseLyrics(body, l)) {
        remember(path, l);
        lyrics_.show(l);
        return;
      }
      lyricsFor_.clear();  // try again next time
      lyrics_.showMessage(!g_app->status().online ? "Lyrics need Wi-Fi.\nConnect in Settings \xe2\x80\xba Wi-Fi."
                                                  : "Couldn't get lyrics: " + httpProblem(status, q));
    });
  }
  // LyricsSource's cache: the last 24 songs
  static std::vector<std::pair<std::string, Lyrics>>& cache() {
    static std::vector<std::pair<std::string, Lyrics>> c;
    return c;
  }
  static void remember(const std::string& path, const Lyrics& l) {
    cache().push_back({path, l});
    if (cache().size() > 24) cache().erase(cache().begin());
  }

  // the lyrics page: a slim header with the song, the lyrics, a thin progress line
  void drawLyricsPage(Env& e, float w, float h, const PlayerState& s, const std::string& path) {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, 0x5a000000);  // extra dimming: lyrics must read on any artwork
    int headH = e.dp(32), art = e.dp(22);
    float ax = float(e.dp(12));
    Rect ar{ax, float((headH - art) / 2), float(art), float(art)};
    e.c.layer(5);
    e.c.shape(ar, e.dpf(3), whiteA(70));
    if (!path.empty()) {
      float a = 0;
      if (const Texture* t = e.covers.get(artOf(path, 0), art, art, 0, e.now, &a)) e.c.image(*t, ar, {0, 0, 1, 1}, e.dpf(3), withAlpha(white, a));
    }
    e.c.layer(6);
    Label tag = label(e, "LYRICS", 9, true);
    float tagX = w - e.dp(10) - tag.w;
    drawLabel(e, tag, tagX, float((headH - tag.h) / 2), whiteA(150));
    float nx = ax + art + e.dp(8), nw = tagX - e.dp(8) - nx;
    SongInfo song = path.empty() ? SongInfo{} : songInfo(path);
    Label t = label(e, song.title, 12, true, nw), a = label(e, firstAndRest(song.subtitle).first, 10, false, nw);
    float ny = float((headH - t.h - a.h) / 2);
    drawLabel(e, t, nx, ny, white);
    drawLabel(e, a, nx, ny + t.h, whiteA(170));
    int progH = e.dp(10), progB = e.dp(4), side = e.dp(6);
    Rect lr{0, float(headH), w, h - headH - progH - progB};
    int64_t pos = PlayerHost::positionAt(s, e.now);
    lyrics_.draw(e, lr, pos, s.durationMs);
    e.c.layer(5);
    progressBar(e, {float(side), lr.bottom(), w - 2 * side, float(progH)}, whiteA(60), white,
                s.durationMs > 0 ? float(s.positionMs) / s.durationMs : 0, 0, 2, scrub_.fraction(s.durationMs));
  }

  // what draw() shows, to tell whether a tick changed it
  struct Shown {
    std::string path;
    int index = -1, next = -1, count = 0, playing = 0, buffering = 0, shuffle = 0, repeat = 0;
    int64_t sec = -1, durSec = -1, bar = -1;
    bool scrubbing = false;
    bool operator==(const Shown& o) const {
      return path == o.path && index == o.index && next == o.next && count == o.count && playing == o.playing &&
             buffering == o.buffering && shuffle == o.shuffle && repeat == o.repeat && sec == o.sec && durSec == o.durSec &&
             bar == o.bar && scrubbing == o.scrubbing;
    }
  };
  Shown shownOf(const PlayerState& s) const {
    Shown v;
    v.path = s.path, v.index = s.index, v.next = s.nextIndex, v.count = s.count, v.playing = s.playing;
    v.buffering = s.buffering, v.shuffle = s.shuffle, v.repeat = s.repeat;
    v.sec = (scrub_.active ? scrub_.target : s.positionMs) / 1000, v.durSec = s.durationMs / 1000;
    // the thumb moves a quarter pixel at a time: the bar is redrawn when it would move
    v.bar = s.durationMs > 0 ? s.positionMs * 4 * 452 / s.durationMs : 0;
    v.scrubbing = scrub_.active;
    return v;
  }

  // ScrubBubble: the target time large, the change and "center to jump" under it, in the middle
  void drawBubble(Env& e, float w, float h, int64_t target, int64_t from) {
    Label t = label(e, formatTime(int(target / 1000)), 26, true);
    int64_t d = target - from;
    std::string delta = (d >= 0 ? "+" : "\xe2\x88\x92") + formatTime(int(std::llabs(d) / 1000)) + "   center to jump";
    Label dl = label(e, delta, 14, false);
    int padX = e.dp(16), padY = e.dp(8), inner = std::max(t.w, dl.w);
    int bw = inner + 2 * padX, bh = padY + t.h + e.dp(2) + dl.h + padY;
    float x = float((int(w) - bw) / 2), y = float((int(h) - bh) / 2);
    e.c.layer(7);
    e.c.shape({x, y, float(bw), float(bh)}, e.dpf(10), 0xe6141414);
    drawLabel(e, t, x + padX + float((inner - t.w) / 2), y + padY, white);
    drawLabel(e, dl, x + padX + float((inner - dl.w) / 2), y + padY + t.h + e.dp(2), e.p.darkMuted);
  }

  // the options panel: 190 units wide, four 28-unit rows, the chosen one white
  std::string optionLabel(int i, const PlayerState& s) const {
    switch (i) {
      case 0: return "Scrub through the song";
      case 1: return "Up next";
      case 2: return std::string("Shuffle: ") + (s.shuffle ? "On" : "Off");
      default: return std::string("Repeat: ") + (s.repeat == 1 ? "All" : s.repeat == 2 ? "One" : "Off");
    }
  }
  void chooseOption() {
    PlayerState s = g_player.state();
    switch (cursor_) {
      case 0:
        options_ = false;
        scrub_.begin();
        break;
      case 1:
        options_ = false;
        g_app->push(makeQueue());
        break;
      case 2: g_player.setShuffle(!s.shuffle); shuffleShown_ = !s.shuffle; break;
      default: g_player.setRepeat(s.repeat == 0 ? 1 : s.repeat == 1 ? 2 : 0); repeatShown_ = s.repeat == 0 ? 1 : s.repeat == 1 ? 2 : 0; break;
    }
    optionsAt_ = now_ns();
  }
  void drawOptions(Env& e, float w, float h, PlayerState s) {
    // the player takes a moment to publish a change: until it does, show what was chosen
    if (now_ns() - optionsAt_ < 500 * kMs) {
      if (shuffleShown_ >= 0) s.shuffle = shuffleShown_;
      if (repeatShown_ >= 0) s.repeat = repeatShown_;
    } else {
      shuffleShown_ = repeatShown_ = -1;
    }
    int pw = e.dp(190), rowH = e.dp(28), pad = e.dp(5);
    int ph = pad + 4 * rowH + pad;
    float x = float((int(w) - pw) / 2), y = float((int(h) - ph) / 2);
    e.c.layer(7);
    e.c.shape({x, y, float(pw), float(ph)}, e.dpf(10), 0xf21e1e20);
    for (int i = 0; i < 4; i++) {
      bool on = i == cursor_;
      Rect r{x + pad, y + pad + float(i * rowH), float(pw - 2 * pad), float(rowH)};
      if (on) e.c.shape(r, e.dpf(6), white);
      Label l = label(e, optionLabel(i, s), 13, on, r.w - 2 * e.dp(12));
      drawLabel(e, l, r.x + e.dp(12), r.y + float((rowH - l.h) / 2), on ? e.p.dark : white);
    }
  }

  Scrubber scrub_;
  LyricsView lyrics_;
  Requests requests_;
  std::string lyricsFor_;
  bool loadPending_ = false;
  int64_t now_ = 0;
  bool options_ = false;
  int cursor_ = 0;
  int shuffleShown_ = -1, repeatShown_ = -1;
  int64_t optionsAt_ = 0;
  Shown shown_;
};

}  // namespace

std::unique_ptr<Screen> makeNowPlayingPreview() { return std::make_unique<NowPlayingScreen>(); }

void openNowPlaying() {
  if (!g_player.active()) return;
  g_app->push(std::make_unique<NowPlayingScreen>());
}

}  // namespace rondo::shell
