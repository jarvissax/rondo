// uitest: the N1 test scene, the native UI engine with real content, run on the Y1 with the Java layer
// and SurfaceFlinger stopped (`stop zygote; stop surfaceflinger`).
//
// A menu leads to the card's songs (today's Rondo index: titles, artists and the covers inside the
// files), a YouTube-Music-style dark list and a now-playing screen with a blurred backdrop. Screens
// slide like the iPod's (push and pop, 250 ms). Every frame goes into the frame log.
//
//   uitest [--index path] [--fonts dir] [--regular ttf] [--seconds n] [--csv path] [--scale 1.4]
//          [--thumbs dir|none] [--loaders n] [--trace ms] [--config path] [--library db] [--restore-android 1]
//
// Holding Menu for 2 s quits; with --restore-android 1 it then starts SurfaceFlinger and the Java layer.
//
// SIGUSR1 saves a screenshot (/data/local/tmp/rondo/shot.png); SIGINT or SIGTERM prints the frame
// report and exits.
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../core/clock.h"
#include "../../platform/platform.h"
#include "../../ui/frame_log.h"
#include "../../ui/gfx.h"
#include "../../ui/image.h"
#include "../../ui/list.h"
#include "../../ui/motion.h"
#include "../../ui/text.h"
#include "../../ui/keyboard.h"
#include "../../ui/theme.h"
#include "../../core/config.h"
#include "../../core/library.h"

using namespace rondo;
using namespace rondo::ui;

namespace {

volatile sig_atomic_t g_stop = 0, g_shot = 0;
rondo::Config g_config;
rondo::Library g_library;
std::unordered_map<std::string, int> g_songIndex;  // path -> index in the songs (for covers)

// ---- the card library, from today's Rondo index (a JSON array of flat objects) ----

struct Song {
  std::string path, title, artist;
};

void appendUtf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) {
    s += char(cp);
  } else if (cp < 0x800) {
    s += char(0xc0 | (cp >> 6)), s += char(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    s += char(0xe0 | (cp >> 12)), s += char(0x80 | ((cp >> 6) & 0x3f)), s += char(0x80 | (cp & 0x3f));
  } else {
    s += char(0xf0 | (cp >> 18)), s += char(0x80 | ((cp >> 12) & 0x3f));
    s += char(0x80 | ((cp >> 6) & 0x3f)), s += char(0x80 | (cp & 0x3f));
  }
}

// A JSON string starting at p (the opening quote); returns the position after it.
const char* jsonString(const char* p, const char* end, std::string& out) {
  out.clear();
  for (p++; p < end && *p != '"'; p++) {
    if (*p != '\\') {
      out += *p;
      continue;
    }
    if (++p >= end) break;
    switch (*p) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'u': {
        if (end - p < 5) return end;
        uint32_t cp = uint32_t(strtoul(std::string(p + 1, 4).c_str(), nullptr, 16));
        p += 4;
        if (cp >= 0xd800 && cp < 0xdc00 && end - p > 6 && p[1] == '\\' && p[2] == 'u') {
          uint32_t lo = uint32_t(strtoul(std::string(p + 3, 4).c_str(), nullptr, 16));
          cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
          p += 6;
        }
        appendUtf8(out, cp);
        break;
      }
      default: out += *p;
    }
  }
  return p + 1;
}

std::vector<Song> loadSongs(const char* path) {
  std::vector<Song> songs;
  FILE* f = fopen(path, "rb");
  if (!f) return songs;
  std::string data;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
  fclose(f);
  const char* p = data.data();
  const char* end = p + data.size();
  Song cur;
  bool video = false;
  std::string key, value;
  while (p < end) {
    if (*p == '{') {
      cur = Song();
      video = false;
      p++;
    } else if (*p == '}') {
      if (!video && !cur.path.empty()) songs.push_back(cur);
      p++;
    } else if (*p == '"') {
      p = jsonString(p, end, key);
      while (p < end && (*p == ' ' || *p == ':')) p++;
      if (p < end && *p == '"') {
        p = jsonString(p, end, value);
        if (key == "path") cur.path = value;
        else if (key == "title") cur.title = value;
        else if (key == "artist") cur.artist = value;
      } else if (key == "video") {
        video = p < end && *p == 't';
      }
    } else {
      p++;
    }
  }
  std::sort(songs.begin(), songs.end(), [](const Song& a, const Song& b) {
    return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
  });
  return songs;
}

// ---- look ----

// The accent's lit top, as today's Rondo computes it (Theme.kt lit()).
Color lit(Color c) {
  float r = ((c >> 16) & 0xff) / 255.f, g = ((c >> 8) & 0xff) / 255.f, b = (c & 0xff) / 255.f;
  float mx = std::max({r, g, b}), mn = std::min({r, g, b});
  float l = (mx + mn) / 2, h = 0, s = 0;
  if (mx != mn) {
    float d = mx - mn;
    s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    h = (mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4) * 60;
  }
  float h2 = fmodf(fmodf(h - 8, 360) + 360, 360);
  float s2 = std::clamp(s * 100 + 12, 0.f, 100.f) / 100;
  float l2 = std::clamp(l * 100 + std::min(20.f, std::max(0.f, 90 - l * 100) * 0.45f), 0.f, 100.f) / 100;
  float a = s2 * std::min(l2, 1 - l2);
  auto f = [&](int n) {
    float k = fmodf(n + h2 / 30, 12);
    return uint32_t(std::clamp((l2 - a * std::max(-1.f, std::min({k - 3, 9 - k, 1.f}))) * 255, 0.f, 255.f));
  };
  return rgb(f(0) << 16 | f(8) << 8 | f(4));
}

struct Look {
  float s = 1.4f;  // today's text scale: mockup units to pixels
  Color accent = rgb(0x1c5ccc);
  Color accentTop = lit(rgb(0x1c5ccc));
  // the light shell
  Color shellBg = rgb(0xffffff), barTop = rgb(0xfbfbfb), barBottom = rgb(0xcfd1d6), barLine = rgb(0x9fa2a8);
  Color rowLine = rgb(0xe6e6e6), text = rgb(0x111111), muted = rgb(0x7a7a7f), chevron = rgb(0x9a9aa0);
  // YouTube Music
  Color dark = rgb(0x0f0f0f), raised = rgb(0x232323), darkMuted = rgb(0xaaaaaa), focus = rgb(0xf1f1f1);
  Color focusText = rgb(0x0f0f0f), focusMuted = rgb(0x4d4d4d);
  int title, subtitle, bold, small, darkTitle, darkSub, npTitle, npArtist, chevronFont;
  float u(float v) const { return v * s; }
};

// ---- covers on the GPU ----

class Covers {
 public:
  Covers(ImageLoader& loader, const std::vector<Song>& songs) : loader_(loader), songs_(songs) {}

  // The cover of song i at size w x h (blurred: a backdrop), asked for when missing. alpha is its
  // fade-in (150 ms after it arrives).
  const Texture* get(int i, int w, int h, int blurRadius, int64_t now, float* alpha) {
    uint64_t key = (uint64_t(i) << 32) | (uint64_t(w) << 20) | (uint64_t(h) << 8) | uint64_t(blurRadius);
    Entry& e = map_[key];
    e.used = ++clock_;
    if (e.state == kNone && loader_.request({key, songs_[i].path, w, h, blurRadius})) e.state = kLoading;
    if (e.state != kReady) return nullptr;
    float a = clamp01(float(now - e.readyAt) / (150 * kMs));
    if (a < 1) fading_ = true;
    if (alpha) *alpha = a;
    return &e.tex;
  }

  // Queues song i at this size for the thumbnail cache, filled while the UI is still.
  void prefetch(int i, int w, int h, int blurRadius) { prefetch_.push_back({i, w, h, blurRadius}); }
  // Hands queued prefetches to the helper as its queue has room; true while some are left.
  bool sendPrefetches() {
    for (int n = 0; n < 64 && sent_ < prefetch_.size(); n++, sent_++) {
      const Ask& a = prefetch_[sent_];
      uint64_t key = (uint64_t(a.i) << 32) | (uint64_t(a.w) << 20) | (uint64_t(a.h) << 8) | uint64_t(a.blur);
      if (!loader_.prefetch({key, songs_[a.i].path, a.w, a.h, a.blur})) break;
    }
    return sent_ < prefetch_.size();
  }

  // Uploads up to `max` decoded covers; returns true when more are waiting or fading.
  int uploadedLast = 0;
  bool pump(int64_t now, int max) {
    ImageLoader::Result r;
    int uploaded = 0;
    while (uploaded < max && loader_.poll(r)) {
      auto it = map_.find(r.id);
      if (it == map_.end()) {
        loader_.release(r);
        continue;
      }
      Entry& e = it->second;
      if (r.dropped) {
        e.state = kNone;
        continue;
      }
      if (!r.ok) {
        e.state = kMissing;
        continue;
      }
      e.tex = take(r.w, r.h);
      glBindTexture(GL_TEXTURE_2D, e.tex.id);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE, r.rgba);
      loader_.release(r);
      e.bytes = size_t(r.w) * r.h * 4;
      bytes_ += e.bytes;
      e.state = kReady;
      // a cover that was asked for long ago shows at once; one arriving while on screen fades in
      e.readyAt = now;
      uploaded++;
    }
    evict();
    bool more = fading_ || uploaded == max;
    uploadedLast = uploaded;
    fading_ = false;
    return more;
  }
  size_t bytes() const { return bytes_; }
  // Textures made ahead of need (GPU memory allocated now, filled when a cover arrives): creating one
  // in a frame costs the driver an allocation.
  void reserve(int w, int h, int n) {
    auto& p = pool_[key(w, h)];
    while (int(p.size()) < n) p.push_back(createTexture(w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, true));
  }
  int created() const { return created_; }

 private:
  enum State : uint8_t { kNone, kLoading, kReady, kMissing };
  struct Entry {
    State state = kNone;
    Texture tex;
    int64_t readyAt = 0;
    uint32_t used = 0;
    size_t bytes = 0;
  };
  void evict() {
    const size_t kBudget = 20u << 20;  // of the 24 MB texture budget
    while (bytes_ > kBudget) {
      auto oldest = map_.end();
      for (auto it = map_.begin(); it != map_.end(); ++it)
        if (it->second.state == kReady && (oldest == map_.end() || it->second.used < oldest->second.used)) oldest = it;
      if (oldest == map_.end()) break;
      bytes_ -= oldest->second.bytes;
      pool_[key(oldest->second.tex.w, oldest->second.tex.h)].push_back(oldest->second.tex);
      map_.erase(oldest);
    }
  }

  static uint32_t key(int w, int h) { return uint32_t(w) << 16 | uint32_t(h); }
  Texture take(int w, int h) {
    auto& p = pool_[key(w, h)];
    if (p.empty()) {
      created_++;
      return createTexture(w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, true);
    }
    Texture t = p.back();
    p.pop_back();
    return t;
  }
  std::unordered_map<uint32_t, std::vector<Texture>> pool_;
  struct Ask {
    int i, w, h, blur;
  };
  std::vector<Ask> prefetch_;
  size_t sent_ = 0;
  int created_ = 0;
  ImageLoader& loader_;
  const std::vector<Song>& songs_;
  std::unordered_map<uint64_t, Entry> map_;
  uint32_t clock_ = 0;
  size_t bytes_ = 0;
  bool fading_ = false;
};

// ---- screens ----

struct Ctx {
  Canvas& c;
  Text& text;
  Look& look;
  Covers& covers;
  const std::vector<Song>& songs;
  float w, h;      // the content area under the status bar
  int64_t now;     // when this frame appears
};

class App;

class Screen {
 public:
  virtual ~Screen() = default;
  virtual std::string title() const = 0;
  virtual bool dark() const { return false; }
  virtual void key(Key k, App& app) = 0;
  // true: Menu goes to the screen (the keyboard uses it), not straight to going back
  virtual bool ownsMenu() const { return false; }
  virtual bool step(float dt) = 0;  // true while moving
  virtual void draw(Ctx& x) = 0;
};

class App {
 public:
  App(Look& look, const std::vector<Song>& songs) : look_(look), songs_(songs) {}
  void push(std::unique_ptr<Screen> s, int64_t now) {
    if (transition_.running(now)) finish();
    stack_.push_back(std::move(s));
    transition_.begin(0, 1, now, 250 * kMs);
    popping_ = false;
  }
  void pop(int64_t now) {
    if (stack_.size() < 2) return;
    if (transition_.running(now)) finish();
    leaving_ = std::move(stack_.back());
    stack_.pop_back();
    transition_.begin(0, 1, now, 250 * kMs);
    popping_ = true;
  }
  void key(Key k, int64_t now) {
    now_ = now;
    if (k == Key::Menu && !stack_.back()->ownsMenu()) {
      pop(now);
      return;
    }
    stack_.back()->key(k, *this);
  }
  int64_t now() const { return now_; }
  const std::vector<Song>& songs() const { return songs_; }
  Look& look() { return look_; }

  bool step(float dt, int64_t now) {
    now_ = now;
    bool moving = false;
    for (auto& s : stack_) moving |= s->step(dt);
    if (transition_.running(now)) moving = true;
    else if (leaving_) leaving_.reset();
    return moving;
  }

  void draw(Ctx& x) {
    const float W = x.c.width();
    const float bar = roundf(look_.u(30));
    Screen* top = stack_.back().get();
    Screen* under = popping_ ? leaving_.get() : (stack_.size() > 1 ? stack_[stack_.size() - 2].get() : nullptr);
    bool moving = transition_.running(x.now) && under;
    float p = moving ? transition_.at(x.now) : 1;
    x.h = x.c.height() - bar;
    // the content slides; the status bar stays, its title cross-fading
    auto content = [&](Screen* s, float dx) {
      x.c.pushOffset(dx, bar);
      x.c.pushClip({0, 0, W, x.h});
      s->draw(x);
      x.c.popClip();
      x.c.popOffset();
    };
    if (moving) {
      // push: the new screen comes from the right, pushing the old one out to the left; pop reverses it
      float off = popping_ ? W * p : -W * p;
      Screen* outgoing = popping_ ? leaving_.get() : under;
      content(outgoing, roundf(off * 4) / 4);
      content(top, roundf((popping_ ? off - W : off + W) * 4) / 4);
    } else {
      content(top, 0);
    }
    statusBar(x, moving ? (popping_ ? leaving_.get() : under) : nullptr, top, p);
  }

 private:
  void finish() {
    transition_.duration = 0;
    leaving_.reset();
  }

  void statusBar(Ctx& x, Screen* from, Screen* to, float p) {
    const float W = x.c.width();
    const float H = roundf(look_.u(30));
    x.c.layer(6);
    bool dark = to->dark();
    float darkness = from ? (from->dark() ? 1 - p : 0) + (to->dark() ? p : 0) : (dark ? 1 : 0);
    Color top = mix(look_.barTop, rgb(0x202020), darkness), bottom = mix(look_.barBottom, rgb(0x141414), darkness);
    x.c.fillGradient({0, 0, W, H}, top, bottom);
    x.c.fill({0, H - 1, W, 1}, mix(look_.barLine, rgb(0x2a2a2a), darkness));
    x.c.layer(7);
    Color ink = mix(look_.text, rgb(0xf1f1f1), darkness);
    auto title = [&](Screen* s, float alpha, float dx) {
      const TextRun& r = x.text.run(look_.bold, s->title(), W - look_.u(120));
      x.c.pushAlpha(alpha);
      x.text.draw(x.c, r, roundf((W - r.width) / 2) + dx, roundf((H + x.text.ascent(look_.bold) * 0.72f) / 2), ink);
      x.c.popAlpha();
    };
    if (from) {
      title(from, 1 - p, 0);
      title(to, p, 0);
    } else {
      title(to, 1, 0);
    }
    // battery
    float bw = look_.u(22), bh = look_.u(11), bx = W - look_.u(12) - bw, by = roundf((H - bh) / 2);
    x.c.shape({bx, by, bw, bh}, look_.u(2.5f), ink);
    x.c.shape({bx + 1.5f, by + 1.5f, bw - 3, bh - 3}, look_.u(1.5f), bottom);
    x.c.shape({bx + 3, by + 3, (bw - 6) * battery_, bh - 6}, 1, mix(rgb(0x30c050), rgb(0x30d060), darkness));
    x.c.shape({bx + bw + 1, by + bh / 2 - look_.u(2), look_.u(2), look_.u(4)}, 1, ink);
    x.c.layer(0);
  }

 public:
  float battery_ = 1;

 private:
  Look& look_;
  const std::vector<Song>& songs_;
  std::vector<std::unique_ptr<Screen>> stack_;
  std::unique_ptr<Screen> leaving_;
  Tween transition_;
  bool popping_ = false;
  int64_t now_ = 0;
};

// How much of row i the selection bar covers (0..1): text colors follow the bar as it glides.
float coverage(const WheelList& l, int i, float rowY) {
  float d = fabsf(l.barY() - rowY) / l.rowHeight();
  return clamp01(1 - d);
}

// The iPod-style shell list: light rows, a gliding accent bar.
class ShellList : public Screen {
 public:
  struct Item {
    std::string label, detail;
    int song = -1;  // a cover to show
    std::unique_ptr<Screen> (*open)(App&, int) = nullptr;
  };
  ShellList(std::string title, std::vector<Item> items) : title_(std::move(title)), items_(std::move(items)) {}
  std::string title() const override { return title_; }
  void key(Key k, App& app) override {
    if (k == Key::WheelCw || k == Key::WheelCcw)
      list_.move(spin_.pulse(k == Key::WheelCw ? 1 : -1, app.now(), g_config.behavior.wheelAcceleration));
    else if (k == Key::Center && items_[list_.selected()].open)
      app.push(items_[list_.selected()].open(app, list_.selected()), app.now());
  }
  bool step(float dt) override { return list_.step(dt); }
  void draw(Ctx& x) override {
    const Look& L = x.look;
    float rowH = roundf(L.u(36));
    if (list_.count() != int(items_.size()) || viewH_ != x.h) {
      viewH_ = x.h;
      list_.configure(int(items_.size()), rowH, x.h);
    }
    x.c.layer(0);
    x.c.fill({0, 0, x.w, x.h}, L.shellBg);
    float scroll = list_.scroll();
    // the bar
    x.c.layer(1);
    float by = list_.barY();
    x.c.fillGradient({0, by, x.w, rowH}, L.accentTop, L.accent);
    bool covers = false;
    for (auto& it : items_) covers |= it.song >= 0;
    float textX = covers ? roundf(L.u(8 + 26 + 9)) : roundf(L.u(14));
    for (int i = list_.first(); i <= list_.last(); i++) {
      const Item& it = items_[i];
      float y = i * rowH - scroll;
      float sel = coverage(list_, i, y);
      x.c.layer(0);
      x.c.fill({L.u(8), y + rowH - 1, x.w - L.u(8), 1}, L.rowLine);
      if (it.song >= 0) {
        float a = 1;
        float t = roundf(L.u(26));
        Rect r = {roundf(L.u(8)), y + roundf((rowH - t) / 2), t, t};
        const Texture* tex = x.covers.get(it.song, int(t), int(t), 0, x.now, &a);
        x.c.layer(2);
        if (!tex || a < 1) x.c.shape(r, L.u(4), L.rowLine);  // placeholder, under the fade-in
        x.c.layer(3);
        if (tex) x.c.image(*tex, r, {0, 0, 1, 1}, L.u(4), withAlpha(0xffffffff, a));
      }
      x.c.layer(4);
      float right = x.w - L.u(10 + 8 + 5);
      Color ink = mix(L.text, rgb(0xffffff), sel), sub = mix(L.muted, rgb(0xdcdcdc), sel);
      if (it.detail.empty()) {
        int f = L.title;
        const TextRun& r = x.text.run(f, it.label, right - textX);
        x.text.draw(x.c, r, textX, y + roundf((rowH + x.text.ascent(f) * 0.7f) / 2), ink);
      } else {
        const TextRun& t = x.text.run(L.small, it.label, right - textX);
        const TextRun& s = x.text.run(L.subtitle, it.detail, right - textX);
        float a1 = x.text.ascent(L.small) * 0.72f, a2 = x.text.ascent(L.subtitle) * 0.72f;
        float gap = L.u(3);
        float top = y + roundf((rowH - (a1 + gap + a2 + L.u(2))) / 2);
        x.text.draw(x.c, t, textX, top + roundf(a1), ink);
        x.text.draw(x.c, s, textX, top + roundf(a1 + gap + a2 + L.u(2)), sub);
      }
      if (it.open) {
        const TextRun& ch = x.text.run(L.chevronFont, "\xe2\x80\xba");
        x.text.draw(x.c, ch, x.w - L.u(10) - ch.width, y + roundf((rowH + x.text.ascent(L.chevronFont) * 0.55f) / 2),
                    mix(L.chevron, rgb(0xffffff), sel));
      }
    }
  }

 private:
  std::string title_;
  std::vector<Item> items_;
  WheelList list_;
  WheelSpin spin_;
  float viewH_ = 0;
};

// A YouTube-Music-style list: dark, 42-unit rows with 34-unit covers, a light focus card.
class DarkList : public Screen {
 public:
  explicit DarkList(int count) : count_(count) {}
  std::string title() const override { return "Your mixes"; }
  bool dark() const override { return true; }
  void key(Key k, App& app) override;
  bool step(float dt) override { return list_.step(dt); }
  void draw(Ctx& x) override {
    const Look& L = x.look;
    float rowH = roundf(L.u(42));
    if (list_.count() != count_ || viewH_ != x.h) {
      viewH_ = x.h;
      list_.configure(count_, rowH, x.h);
    }
    x.c.layer(0);
    x.c.fill({0, 0, x.w, x.h}, L.dark);
    float scroll = list_.scroll();
    float m = roundf(L.u(5));
    x.c.layer(1);
    x.c.shape({m, list_.barY() + roundf(L.u(2)), x.w - 2 * m, rowH - roundf(L.u(4))}, L.u(7), L.focus);
    for (int i = list_.first(); i <= list_.last(); i++) {
      int song = i % int(x.songs.size());
      float y = i * rowH - scroll;
      float sel = coverage(list_, i, y);
      float art = roundf(L.u(34));
      Rect r = {m + roundf(L.u(4)), y + roundf((rowH - art) / 2), art, art};
      x.c.layer(2);
      float a = 1;
      const Texture* cov = x.covers.get(song, int(art), int(art), 0, x.now, &a);
      if (!cov || a < 1) x.c.shape(r, L.u(3), L.raised);
      x.c.layer(3);
      if (cov)
        x.c.image(*cov, r, {0, 0, 1, 1}, L.u(3), withAlpha(0xffffffff, a));
      x.c.layer(4);
      float tx = r.right() + roundf(L.u(8)), right = x.w - m - L.u(8);
      const Song& s = x.songs[song];
      const TextRun& t = x.text.run(L.darkTitle, s.title, right - tx);
      const TextRun& sub = x.text.run(L.darkSub, s.artist + " \xe2\x80\xa2 Mix", right - tx);
      float a1 = x.text.ascent(L.darkTitle) * 0.72f, a2 = x.text.ascent(L.darkSub) * 0.72f, gap = L.u(5);
      float top = y + roundf((rowH - (a1 + gap + a2)) / 2);
      x.text.draw(x.c, t, tx, top + roundf(a1), mix(rgb(0xffffff), L.focusText, sel));
      x.text.draw(x.c, sub, tx, top + roundf(a1 + gap + a2), mix(L.darkMuted, L.focusMuted, sel));
    }
  }

 private:
  int count_;
  WheelList list_;
  WheelSpin spin_;
  float viewH_ = 0;
};

// Now playing: the cover over its own blurred, darkened backdrop; Next and Prev change the song with a
// cross-fade.
class NowPlaying : public Screen {
 public:
  explicit NowPlaying(int song) : song_(song), prev_(song) {}
  std::string title() const override { return "Now Playing"; }
  bool dark() const override { return true; }
  void key(Key k, App& app) override {
    int n = int(app.songs().size());
    if (k == Key::Next || k == Key::WheelCw || k == Key::Prev || k == Key::WheelCcw) {
      prev_ = song_;
      song_ = (song_ + ((k == Key::Next || k == Key::WheelCw) ? 1 : n - 1)) % n;
      fade_.begin(0, 1, app.now(), 180 * kMs, easeInOutCubic);
    }
  }
  bool step(float dt) override {
    now_ += int64_t(dt * 1e9);
    return fade_.running(now_) || waiting_;
  }
  void draw(Ctx& x) override {
    now_ = x.now;
    const Look& L = x.look;
    float p = fade_.at(x.now);
    float art = roundf(L.u(130));
    if (prefetched_ != song_) {  // the neighbors' pictures, decoded while nothing moves
      prefetched_ = song_;
      int n = int(x.songs.size());
      for (int d : {1, -1}) {
        x.covers.prefetch((song_ + d + n) % n, int(art), int(art), 0);
        x.covers.prefetch((song_ + d + n) % n, 48, 36, 3);
      }
    }
    waiting_ = false;
    x.c.layer(0);
    x.c.fill({0, 0, x.w, x.h}, rgb(0x101010));
    auto backdrop = [&](int song, float alpha) {
      float a = 1;
      const Texture* t = x.covers.get(song, 48, 36, 3, x.now, &a);
      if (!t) {
        waiting_ = true;
        return;
      }
      x.c.image(*t, {0, 0, x.w, x.h}, {0, 0, 1, 1}, 0, withAlpha(0xff8a8a8a, alpha * a));
    };
    if (p < 1) backdrop(prev_, 1 - p);
    backdrop(song_, p);
    float ax = roundf(L.u(18)), ay = roundf((x.h - art) / 2 - L.u(6));
    x.c.layer(2);
    auto cover = [&](int song, float alpha) {
      float a = 1;
      const Texture* t = x.covers.get(song, int(art), int(art), 0, x.now, &a);
      if (t) x.c.image(*t, {ax, ay, art, art}, {0, 0, 1, 1}, L.u(6), withAlpha(0xffffffff, alpha * a));
      else waiting_ = true;
    };
    x.c.shape({ax, ay, art, art}, L.u(6), withAlpha(rgb(0x303030), 1));
    if (p < 1) cover(prev_, 1 - p);
    cover(song_, p);
    x.c.layer(3);
    float tx = ax + art + roundf(L.u(16)), right = x.w - L.u(14);
    auto info = [&](int song, float alpha) {
      const Song& s = x.songs[song];
      x.c.pushAlpha(alpha);
      const TextRun& t = x.text.run(L.npTitle, s.title, right - tx);
      const TextRun& a = x.text.run(L.npArtist, s.artist, right - tx);
      float base = ay + roundf(art * 0.42f);
      x.text.draw(x.c, t, tx, base, rgb(0xffffff));
      x.text.draw(x.c, a, tx, base + roundf(L.u(20)), rgb(0xcfcfcf));
      x.c.popAlpha();
    };
    if (p < 1) info(prev_, 1 - p);
    info(song_, p);
    // the progress bar, a fixed 37 % for the test
    float py = ay + art - roundf(L.u(14)), pw = right - tx;
    x.c.shape({tx, py, pw, roundf(L.u(3))}, L.u(1.5f), withAlpha(0xffffffff, 0.3f));
    x.c.shape({tx, py, roundf(pw * 0.37f), roundf(L.u(3))}, L.u(1.5f), rgb(0xffffff));
    x.c.shape({tx + roundf(pw * 0.37f) - L.u(6), py + L.u(1.5f) - L.u(6), L.u(12), L.u(12)}, L.u(6), rgb(0xffffff));
  }

 private:
  int song_, prev_, prefetched_ = -1;
  Tween fade_;
  int64_t now_ = 0;
  bool waiting_ = true;
};

void DarkList::key(Key k, App& app) {
  if (k == Key::WheelCw || k == Key::WheelCcw)
    list_.move(spin_.pulse(k == Key::WheelCw ? 1 : -1, app.now(), g_config.behavior.wheelAcceleration));
  else if (k == Key::Center)
    app.push(std::unique_ptr<Screen>(new NowPlaying(list_.selected() % int(app.songs().size()))), app.now());
}

std::unique_ptr<Screen> songsScreen(App& app, int) {
  std::vector<ShellList::Item> items;
  const auto& songs = app.songs();
  for (size_t i = 0; i < songs.size(); i++) {
    ShellList::Item it;
    it.label = songs[i].title;
    it.detail = songs[i].artist;
    it.song = int(i);
    it.open = [](App& a, int index) { return std::unique_ptr<Screen>(new NowPlaying(index)); };
    items.push_back(std::move(it));
  }
  return std::unique_ptr<Screen>(new ShellList("Songs", std::move(items)));
}

// Search: the keyboard under the query; every keystroke searches the library (about 1-4 ms), and the
// action key opens the songs found.
class SearchScreen : public Screen {
 public:
  SearchScreen() : kb_("Search", true) {}
  std::string title() const override { return "Search"; }
  bool ownsMenu() const override { return true; }
  void key(Key k, App& app) override {
    Keyboard::Result r = kb_.handle(k);
    if (r == Keyboard::Result::Close) {
      app.pop(app.now());
    } else if (r == Keyboard::Result::Typed || r == Keyboard::Result::Delete) {
      found_ = g_library.search(kb_.typed());
    } else if (r == Keyboard::Result::Action && !found_.songs.empty()) {
      std::vector<ShellList::Item> items;
      for (const Track* t : found_.songs) {
        ShellList::Item it;
        it.label = t->title;
        it.detail = t->artist;
        auto at = g_songIndex.find(t->path);
        it.song = at == g_songIndex.end() ? -1 : at->second;
        it.open = [](App& a, int index) { return std::unique_ptr<Screen>(new NowPlaying(0)); };
        items.push_back(std::move(it));
      }
      app.push(std::unique_ptr<Screen>(new ShellList("\xe2\x80\x9c" + kb_.typed() + "\xe2\x80\x9d", std::move(items))), app.now());
    }
  }
  bool step(float dt) override { return kb_.step(dt); }
  void draw(Ctx& x) override {
    const Look& L = x.look;
    x.c.layer(0);
    x.c.fill({0, 0, x.w, x.h}, L.shellBg);
    float m = roundf(L.u(10)), fieldH = roundf(L.u(26));
    x.c.shape({m, m, x.w - 2 * m, fieldH}, L.u(6), L.rowLine);
    x.c.layer(4);
    const std::string& q = kb_.typed();
    const TextRun& t = x.text.run(L.title, q.empty() ? "Songs, artists, albums" : q, x.w - 4 * m);
    float base = m + roundf((fieldH + x.text.ascent(L.title) * 0.7f) / 2);
    x.text.draw(x.c, t, 2 * m, base, q.empty() ? L.muted : L.text);
    if (!q.empty()) x.c.fill({2 * m + t.width + 1, m + roundf(L.u(5)), 2, fieldH - roundf(L.u(10))}, L.accent);
    char line[128];
    snprintf(line, sizeof(line), "%zu songs \xc2\xb7 %zu artists \xc2\xb7 %zu albums", found_.songs.size(),
             found_.artists.size(), found_.albums.size());
    const TextRun& c = x.text.run(L.subtitle, q.empty() ? "" : line, x.w - 2 * m);
    x.text.draw(x.c, c, 2 * m, m + fieldH + roundf(L.u(16)), L.muted);
    // the first results, one line each
    float y = m + fieldH + roundf(L.u(22));
    for (size_t i = 0; i < found_.songs.size() && i < 2; i++) {
      const Track* s = found_.songs[i];
      const TextRun& r = x.text.run(L.small, s->title + " \xe2\x80\x94 " + s->artist, x.w - 4 * m);
      x.text.draw(x.c, r, 2 * m, y + roundf(L.u(14)), L.text);
      y += roundf(L.u(18));
    }
    float kh = roundf(L.u(5 * 24));
    x.c.layer(1);
    kb_.draw(x.c, x.text, palette_, {roundf(L.u(4)), x.h - kh - roundf(L.u(4)), x.w - roundf(L.u(8)), kh}, int(L.u(15) + 0.5f), int(L.u(12) + 0.5f));
  }
  static Palette palette_;

 private:
  Keyboard kb_;
  Library::Found found_;
};
Palette SearchScreen::palette_;

std::unique_ptr<Screen> libraryMenu(App& app, int) {
  std::vector<ShellList::Item> items;
  ShellList::Item songs;
  songs.label = "Songs";
  songs.open = songsScreen;
  items.push_back(std::move(songs));
  ShellList::Item search;
  search.label = "Search";
  search.open = [](App& a, int) { return std::unique_ptr<Screen>(new SearchScreen()); };
  items.push_back(std::move(search));
  for (const char* label : {"Artists", "Albums", "Videos", "Playlists"}) {
    ShellList::Item it;
    it.label = label;
    items.push_back(std::move(it));
  }
  return std::unique_ptr<Screen>(new ShellList("Library", std::move(items)));
}

// The main menu in the settings' order, without the hidden entries.
std::unique_ptr<Screen> mainMenu(App& app) {
  std::vector<ShellList::Item> items;
  for (const MenuEntry& e : g_config.theme.menu) {
    if (!e.visible) continue;
    ShellList::Item it;
    it.label = menuLabel(e.id);
    if (e.id == "nowPlaying") it.open = [](App& a, int) { return std::unique_ptr<Screen>(new NowPlaying(0)); };
    else if (e.id == "device") it.open = libraryMenu;
    else if (e.id == "music") it.open = [](App& a, int) { return std::unique_ptr<Screen>(new DarkList(120)); };
    items.push_back(std::move(it));
  }
  return std::unique_ptr<Screen>(new ShellList("Rondo", std::move(items)));
}

float readBattery() {
  FILE* f = fopen("/sys/class/power_supply/battery/capacity", "r");
  int v = 100;
  if (f) {
    if (fscanf(f, "%d", &v) != 1) v = 100;
    fclose(f);
  }
  return std::clamp(v / 100.0f, 0.0f, 1.0f);
}

void onSignal(int sig) {
  if (sig == SIGUSR1) g_shot = 1;
  else g_stop = 1;
}

}  // namespace

int main(int argc, char** argv) {
  const char* index = "/data/data/com.jarvissax.rondo/files/local-index.json";
  const char* fonts = "/system/fonts";
  const char* regular = "/data/local/tmp/rondo/fonts/Roboto-Regular.ttf";
  const char* csv = nullptr;
  const char* thumbs = "/data/local/tmp/rondo/thumbs";
  const char* configPath = "/data/data/com.jarvissax.rondo/files/config.json";
  const char* libraryDb = "/data/local/tmp/rondo/library.db";
  float scale = 0;
  bool restoreAndroid = false;
  int64_t menuDown = 0;
  double seconds = 0;
  int loaders = 2;
  int trace = 0;  // print frames whose build takes longer (ms)
  Look look;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--index")) index = argv[i + 1];
    else if (!strcmp(argv[i], "--fonts")) fonts = argv[i + 1];
    else if (!strcmp(argv[i], "--regular")) regular = argv[i + 1];
    else if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[i + 1]);
    else if (!strcmp(argv[i], "--csv")) csv = argv[i + 1];
    else if (!strcmp(argv[i], "--thumbs")) thumbs = strcmp(argv[i + 1], "none") ? argv[i + 1] : nullptr;
    else if (!strcmp(argv[i], "--scale")) scale = float(atof(argv[i + 1]));
    else if (!strcmp(argv[i], "--config")) configPath = argv[i + 1];
    else if (!strcmp(argv[i], "--library")) libraryDb = argv[i + 1];
    else if (!strcmp(argv[i], "--restore-android")) restoreAndroid = atoi(argv[i + 1]) != 0;
    else if (!strcmp(argv[i], "--loaders")) loaders = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--trace")) trace = atoi(argv[i + 1]);
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGUSR1, onSignal);
  signal(SIGHUP, SIG_IGN);  // keeps running after the adb shell that started it ends
  setpriority(PRIO_PROCESS, 0, -10);  // the render thread is above normal
  if (FILE* pf = fopen("/data/local/tmp/rondo/uitest.pid", "w")) {  // for tools/perf: kill -USR1
    fprintf(pf, "%d\n", getpid());
    fclose(pf);
  }

  // first, while this is the only thread: it forks the decoding process
  ImageLoader loader;
  if (!loader.start(loaders, thumbs)) return 1;
  int64_t t0 = now_ns();
  // the settings: 1.0.x's config.json, theme and menu included
  g_config = loadConfig(configPath);
  if (scale > 0) g_config.theme.textScale = scale;
  Palette pal = paletteFor(g_config.theme);
  SearchScreen::palette_ = pal;
  look.s = pal.scale;
  look.accent = pal.accent, look.accentTop = pal.accentTop;
  look.shellBg = pal.shellBg, look.barTop = pal.barTop, look.barBottom = pal.barBottom, look.barLine = pal.barLine;
  look.rowLine = pal.rowLine, look.text = pal.text, look.muted = pal.muted, look.chevron = pal.chevron;
  // the songs: N2's library index when there is one, else 1.0.x's index
  std::vector<Song> songs;
  if (g_library.open(libraryDb) && !g_library.tracks().empty()) {
    for (const Track* t : g_library.songs()) songs.push_back({t->path, t->title, t->artist});
  } else {
    songs = loadSongs(index);
  }
  for (size_t i = 0; i < songs.size(); i++) g_songIndex[songs[i].path] = int(i);
  if (songs.empty()) {
    fprintf(stderr, "uitest: no songs in %s\n", index);
    return 1;
  }
  int64_t tSongs = now_ns();
  auto display = openDisplay();
  if (!display) return 1;
  auto input = openInput();
  if (!input) return 1;
  int64_t tDisplay = now_ns();
  Canvas canvas;
  if (!canvas.init(display->width(), display->height())) return 1;
  Text text;
  if (!text.init(fonts, access(regular, R_OK) == 0 ? regular : nullptr)) return 1;
  look.title = text.font(Weight::Regular, look.u(15));
  look.small = text.font(Weight::Regular, look.u(13.5f));
  look.subtitle = text.font(Weight::Regular, look.u(11));
  look.bold = text.font(Weight::Bold, look.u(15));
  look.darkTitle = text.font(Weight::Regular, look.u(13.5f));
  look.darkSub = text.font(Weight::Regular, look.u(11));
  look.npTitle = text.font(Weight::Bold, look.u(16));
  look.npArtist = text.font(Weight::Regular, look.u(12.5f));
  look.chevronFont = text.font(Weight::Bold, look.u(18));
  Covers covers(loader, songs);
  {
    int t = int(roundf(look.u(26))), d = int(roundf(look.u(34))), np = int(roundf(look.u(130)));
    covers.reserve(t, t, 128);
    covers.reserve(d, d, 64);
    covers.reserve(np, np, 24);
    covers.reserve(48, 36, 24);
    // every list thumbnail, decoded once into the cache while nothing moves
    for (size_t i = 0; i < songs.size(); i++) {
      covers.prefetch(int(i), t, t, 0);
      covers.prefetch(int(i), d, d, 0);
    }
  }
  App app(look, songs);
  app.battery_ = readBattery();
  app.push(mainMenu(app), 0);
  int64_t tReady = now_ns();
  printf("uitest: %zu songs (%.0f ms), display %dx%d (%.0f ms), engine ready at %.0f ms\n", songs.size(),
         (tSongs - t0) / 1e6, display->width(), display->height(), (tDisplay - tSongs) / 1e6, (tReady - t0) / 1e6);

  FrameLog log(display->period());
  const int64_t period = display->period();
  bool moving = true, wasMoving = false;
  bool warmed = false;
  const std::vector<int> warmFonts = {look.title, look.small, look.subtitle, look.bold, look.darkTitle, look.darkSub,
                                      look.npTitle, look.npArtist};
  static const char kWarmChars[] =
      " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
      "ÀÁÄÇÉÖÜàáâäçèéêíñóöúü"
      "ĞğİıŞşƏə–—‘’“”•…";
  int64_t lastPresent = 0, lastBattery = now_ns();
  int frames = 0;
  while (!g_stop) {
    if (seconds > 0 && now_ns() - t0 > int64_t(seconds * 1e9)) break;
    if (!moving && !g_shot) {
      // idle: nothing is drawn until something happens; meanwhile the common glyphs get rasterized
      pollfd p[2] = {{input->waitFd(), POLLIN, 0}, {loader.waitFd(), POLLIN, 0}};
      if (!warmed) {
        warmed = text.warm(warmFonts, kWarmChars, 3 * kMs);
        text.flush();  // up to the GPU now, not in the next frame
        if (poll(p, 2, 0) == 0) continue;
      }
      // the cache prefetches go out as the helper's queue (10 datagrams) has room
      bool prefetching = covers.sendPrefetches();
      int r = poll(p, 2, prefetching ? 50 : 1000);
      if (r < 0 && errno == EINTR) continue;
      if (r == 0) {
        if (now_ns() - lastBattery > 60 * kSec) {
          lastBattery = now_ns();
          app.battery_ = readBattery();
          moving = true;
        }
        continue;
      }
    }
    int64_t vs = display->waitVsync();
    int64_t present = vs + period;
    input->acknowledge();
    loader.acknowledge();
    InputEvent ev;
    int64_t oldest = -1;
    while (input->poll(ev)) {
      if (oldest < 0) oldest = vs - ev.time;
      if (ev.down) app.key(ev.key, present);
      // Menu held 2 s: leave (with --restore-android, today's Rondo comes back)
      if (ev.key == Key::Menu) {
        if (ev.down) menuDown = ev.time;
        else if (menuDown && ev.time - menuDown >= 2 * kSec) g_stop = 1;
      }
    }
    float dt = wasMoving && lastPresent ? float(present - lastPresent) / 1e9f : float(period) / 1e9f;
    dt = std::min(dt, 0.1f);
    int64_t a = now_ns();
    bool anim = app.step(dt, present);
    int64_t a1 = now_ns();
    canvas.begin(rgb(0x000000));
    int64_t a2 = now_ns();
    Ctx ctx{canvas, text, look, covers, songs, float(display->width()), 0, present};
    app.draw(ctx);
    int64_t b = now_ns();
    if (trace && b - a > int64_t(trace) * kMs)
      printf("slow frame: step %.1f ms, begin %.1f, draw %.1f\n", (a1 - a) / 1e6, (a2 - a1) / 1e6, (b - a2) / 1e6);
    text.flush();
    bool more = covers.pump(present, 2);
    canvas.end();
    int64_t c = now_ns();
    if (g_shot) {
      g_shot = 0;
      bool ok = saveScreenshot("/data/local/tmp/rondo/shot.png", display->width(), display->height());
      printf("uitest: screenshot %s\n", ok ? "saved" : "failed");
    }
    display->present();
    int64_t d = now_ns();
    FrameLog::Frame f;
    f.vsync = vs;
    f.buildNs = int32_t(b - a);
    f.submitNs = int32_t(c - b);
    f.swapNs = int32_t(d - c);
    f.inputNs = int32_t(oldest);
    f.draws = int16_t(canvas.drawCalls());
    f.quads = canvas.quads();
    f.glyphs = int16_t(text.glyphsRasterizedLastFrame());
    f.continued = wasMoving;
    f.uploads = int16_t(covers.uploadedLast);
    log.add(f);
    text.frameDone();
    frames++;
    lastPresent = present;
    moving = anim || more;
    if (moving) loader.active(present);
    wasMoving = moving;
  }
  printf("uitest: covers on the GPU %.1f MB, %d textures created after startup\n", covers.bytes() / 1048576.0,
         covers.created());
  log.report(stdout, csv);
  if (restoreAndroid) {  // give the screen back to SurfaceFlinger and start the Java layer again
    display.reset();
    system("start surfaceflinger; sleep 1; start zygote");
  }
  fflush(stdout);
  _exit(0);  // the GL driver's exit handlers can hang after eglTerminate: nothing is left to clean up
  return 0;
}
