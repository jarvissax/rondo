// YouTube Shorts (ShortsScreen.kt in 1.0.x), from YouTube Home's first row: one short at a time,
// looping, the vertical video on the left and what it is on the right. Next and Previous move between
// shorts (the swipe of the app), the wheel is the volume as in every player, center pauses, Menu leaves.
// The list is the server's /video/shorts: popular shorts, then YouTube's endless "watch next" chain.
// The next shorts' details and streams are asked for while this one plays, so moving on is quick.
// 2.0: moving between shorts is the phone's swipe: the short slides up and away as the next one comes
// in from below (Previous: the other way), and the details beside it cross-fade.
#include <set>
#include <unordered_map>

#include "../platform/video_surface.h"
#include "music_list.h"

namespace rondo::shell {
namespace {

constexpr Color white = 0xffffffff;

class ShortsScreen : public Screen {
 public:
  ShortsScreen() { load(); }
  ~ShortsScreen() override {
    destroyTexture(outgoing_);
    if (index_ < 0) return;
    if (savedRepeat_ >= 0) g_player.setRepeat(savedRepeat_);
    g_player.stopVideo();
  }
  std::string title() const override { return "Shorts"; }
  bool statusBar() const override { return false; }
  bool keepScreenOn() const override { return true; }  // the screen stays on here only (1.0.x)

  bool onAction(Action a, int steps) override {
    switch (a) {
      case Action::Up:
      case Action::Down: g_app->changeVolume(a == Action::Down ? 1 : -1); break;
      case Action::Next: play(nextReady(), 1); break;
      case Action::Prev: play(index_ - 1, -1); break;
      case Action::Select:
      case Action::PlayPause: g_player.togglePlay(); break;
      case Action::Back: return false;
      default: break;
    }
    return true;
  }

  bool tick(int64_t now) override {
    PlayerState s = g_player.state();
    // a short that won't play (removed, age-restricted, a format the server can't stream): on to the
    // next one in the same direction
    if (const Item* it = current()) {
      bool ours = s.video && videoSound(*it) == s.path;
      if (ours && (s.error[0] || s.videoFailed) && skippedAt_ != index_) {
        skippedAt_ = index_;
        play(index_ + step_, step_);
        return true;
      }
    }
    Shown v = shown(s, now);
    bool changed = !(v == shown_);
    shown_ = v;
    return changed;
  }

  bool step(float) override { return sliding(now_ns()); }

  void draw(Env& e, float w, float h) override {
    canvas_ = &e.c;
    PlayerState s = g_player.state();
    shown_ = shown(s, e.now);
    int bw = int(h) * 9 / 16;  // a 9:16 box as tall as the screen
    boxW_ = bw, boxH_ = int(h);
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, e.p.dark);
    e.c.fill({0, 0, float(bw), h}, 0xff000000);
    const Item* it = current();
    // the swipe: the incoming short from below (Next) or above (Previous), the outgoing one ahead of it
    bool moving = sliding(e.now);
    float t = moving ? slide_.at(e.now) : 1, off = moving ? (1 - t) * h * float(dir_) : 0;
    e.c.pushClip({0, 0, float(bw), h});
    if (moving) {
      e.c.pushOffset(0, off - h * float(dir_));
      e.c.layer(1);
      if (outgoingFrame_) {
        e.c.image(outgoing_, outgoingRect_, {0, 1, 1, -1});
      } else if (!outgoing_.thumb.empty()) {
        float a = 0;
        if (const Texture* tx = e.covers.get(outgoing_.thumb, bw, int(h), 0, e.now, &a)) e.c.image(*tx, {0, 0, float(bw), h}, {0, 0, 1, 1});
      }
      e.c.popOffset();
    }
    e.c.pushOffset(0, off);
    VideoSurface* v = VideoSurface::made();
    if (shown_.picture) {
      e.c.layer(1);
      e.c.video(v->texture(), videoRect(*v, bw, h), v->transform());
    } else if (it) {  // the short's first frame, until its video draws
      float a = 0;
      if (const Texture* tx = e.covers.get(it->thumb, bw, int(h), 0, e.now, &a)) {
        e.c.layer(1);
        e.c.image(*tx, {0, 0, float(bw), h}, {0, 0, 1, 1}, 0, withAlpha(white, moving ? 1 : a));
      }
    }
    e.c.layer(2);
    if (shown_.waiting && !moving) {
      Label l = label(e, "Loading\xe2\x80\xa6", 12, false);
      drawLabel(e, l, centerText(0, float(bw), l.w), float((int(h) - l.h) / 2), e.p.darkMuted);
    }
    if (shown_.paused) {
      int p = e.dp(30);
      e.icons.draw(e.c, Icon::Pause, {float((bw - p) / 2), float((int(h) - p) / 2), float(p), float(p)}, 0xdcffffff);
    }
    e.c.popOffset();
    e.c.popClip();
    // the next three's first frames, loading while this one plays
    for (int k = index_ + 1; k >= 1 && k <= index_ + 3 && k < int(items_.size()); k++) {
      float a;
      e.covers.get(items_[size_t(k)].thumb, bw, int(h), 0, e.now, &a);
    }
    e.c.layer(2);
    int ph = e.dp(8);
    progressBar(e, {0, h - ph, float(bw), float(ph)}, 0x50ffffff, e.p.red, moving ? 0 : shown_.fraction, 0, 3);
    drawHeader(e, float(bw), w);
    // the details cross-fade: the outgoing short's out in the first half, the new one's in the second
    if (moving) {
      float out = clamp01(1 - t * 2), in = clamp01(t * 2 - 1);
      if (out > 0) drawDetails(e, float(bw), w, h, outgoing_.title, outgoing_.subtitle, outgoing_.pos, out);
      if (in > 0) drawDetails(e, float(bw), w, h, detailTitle(), detailSubtitle(), position(), in);
    } else {
      drawDetails(e, float(bw), w, h, detailTitle(), detailSubtitle(), position(), 1);
    }
    e.c.layer(0);
  }

 private:
  struct Details {
    std::string title, subtitle;
  };
  struct Shown {
    int index = -1, count = 0;
    bool more = false, picture = false, waiting = true, paused = true, described = false;
    float fraction = 0;
    int bar = 0;
    bool operator==(const Shown& o) const {
      return index == o.index && count == o.count && more == o.more && picture == o.picture && waiting == o.waiting &&
             paused == o.paused && described == o.described && bar == o.bar;
    }
  };

  const Item* current() const { return index_ >= 0 && index_ < int(items_.size()) ? &items_[size_t(index_)] : nullptr; }

  // what the screen shows now (onTick in 1.0.x); before the first short, its build state
  // A picture of the short now playing is on the surface: one taken after its video rendered its first.
  // The one before keeps sending its last pictures for a moment after a switch; shown, they looked like
  // it starting again without sound.
  bool ownPicture(const PlayerState& s, VideoSurface* vs) const {
    if (!vs || !vs->hasPicture()) return false;
    // never longer than 1.5 s: whatever is on the surface then is shown (this check can only hide an
    // old picture for a moment, never keep a short from showing)
    if (now_ns() - askedNs_ > 1500 * kMs) return true;
    if (armedAt_ < 0) {
      bool started = s.video && s.videoFrames > 0 && expectPath_ == s.path;
      if (!started) return false;
      armedAt_ = int64_t(vs->latches());  // the next picture taken is this short's
      armedNs_ = now_ns();
      return false;
    }
    // paused on its first picture, no newer one comes: by then the old pictures are long gone
    return int64_t(vs->latches()) > armedAt_ || now_ns() - armedNs_ > 400 * kMs;
  }
  Shown shown(const PlayerState& s, int64_t now) const {
    Shown v;
    v.index = index_, v.count = int(items_.size()), v.more = !next_.empty();
    const Item* it = current();
    if (!it) {
      v.waiting = !message_;
      return v;  // (1.0.x shows the pause sign while the list loads)
    }
    VideoSurface* vs = VideoSurface::made();
    v.picture = ownPicture(s, vs) && vs->width() > 0 && vs->height() > 0;
    v.waiting = !v.picture;
    v.paused = v.picture && !s.playing;
    v.described = details_.count(it->id) > 0;
    int64_t d = s.durationMs > 0 ? s.durationMs : std::max(0, it->durationSec) * 1000LL;
    int64_t pos = PlayerHost::positionAt(s, now);
    v.fraction = d > 0 ? std::min(1.f, float(pos) / d) : 0;
    v.bar = int(v.fraction * 800);
    return v;
  }

  // the video fitted into the box
  static Rect videoRect(VideoSurface& v, int bw, float h) {
    float sc = std::min(bw / float(v.width()), h / float(v.height()));
    float vw = floorf(v.width() * sc + 0.5f), vh = floorf(v.height() * sc + 0.5f);
    return {floorf((bw - vw) / 2), floorf((h - vh) / 2), vw, vh};
  }
  bool sliding(int64_t now) const { return dir_ != 0 && slide_.running(now); }

  // what the side shows for the current short
  std::string detailTitle() const {
    const Item* it = current();
    if (!it) return title_;
    auto d = details_.find(it->id);
    return d != details_.end() && !d->second.title.empty() ? d->second.title : !it->title.empty() ? it->title : "\xe2\x80\xa6";
  }
  std::string detailSubtitle() const {
    const Item* it = current();
    if (!it) return subtitle_;
    auto d = details_.find(it->id);
    return d != details_.end() && !d->second.subtitle.empty() ? d->second.subtitle : it->subtitle;
  }
  std::string position() const {
    if (!current()) return "";
    return std::to_string(index_ + 1) + " of " + std::to_string(items_.size()) + (next_.empty() ? "" : "+");
  }

  int headHeight(Env& e) {
    Label name = label(e, "Shorts", 14, true);
    int icon = e.dp(14);
    int statusH = std::max(e.dp(11), label(e, "0", 10, true).h);
    return std::max(std::max(icon, name.h), statusH);
  }
  // the header: the Shorts sign, the name, the status icons
  void drawHeader(Env& e, float x0, float w) {
    float left = x0 + e.dp(11), right = w - e.dp(10), top = float(e.dp(7));
    Label name = label(e, "Shorts", 14, true);
    int icon = e.dp(14), headH = headHeight(e);
    e.c.layer(2);
    e.icons.draw(e.c, Icon::Shorts, {left, top + float((headH - icon) / 2), float(icon), float(icon)}, e.p.red);
    float statusX = right - e.dp(18);  // the battery alone
    drawLabel(e, label(e, "Shorts", 14, true, std::max(0.f, statusX - (left + icon + e.dp(5)))), left + icon + e.dp(5),
              top + float((headH - name.h) / 2), white);
    e.c.pushOffset(0, top);
    batteryIcon(e, right, headH, e.p.darkMuted);
    e.c.popOffset();
  }

  void drawDetails(Env& e, float x0, float w, float h, const std::string& titleText, const std::string& subText,
                   const std::string& posText, float alpha) {
    float left = x0 + e.dp(11), right = w - e.dp(10), top = float(e.dp(7)), bottom = h - e.dp(8);
    float iw = right - left;
    int headH = headHeight(e);
    std::vector<Label> title = wrap(e, titleText, 15, true, iw, 4), sub = wrap(e, subText, 11, false, iw, 2);
    Label pos = label(e, posText, 10, false, iw);
    Label hint1 = label(e, "Next / Prev: another short", 9, false, iw);
    Label hint2 = label(e, "center: pause \xc2\xb7 wheel: volume", 9, false, iw);
    auto height = [&](const std::vector<Label>& ls, float sp, bool bold) {
      int t = 0;
      for (const Label& l : ls) t += l.h;
      return ls.empty() ? label(e, " ", sp, bold).h : t;
    };
    int titleH = height(title, 15, true), subH = height(sub, 11, false);
    int content = headH + titleH + e.dp(4) + subH + pos.h + e.dp(3) + hint1.h + hint2.h;
    int rest = std::max(0, int(bottom - top) - content);
    int gap1 = rest / 2, gap2 = rest - gap1;  // the two spacers share what is left
    e.c.layer(2);
    e.c.pushAlpha(alpha);
    float y = top + headH + gap1;
    for (const Label& l : title) drawLabel(e, l, left, y, white), y += l.h;
    if (title.empty()) y += titleH;
    y += e.dp(4);
    for (const Label& l : sub) drawLabel(e, l, left, y, e.p.darkMuted), y += l.h;
    if (sub.empty()) y += subH;
    y += gap2;
    drawLabel(e, pos, left, y, e.p.darkMuted);
    y += pos.h + e.dp(3);
    drawLabel(e, hint1, left, y, e.p.darkFaint);
    drawLabel(e, hint2, left, y + hint1.h, e.p.darkFaint);
    e.c.popAlpha();
  }

  void load() {
    title_ = "Loading Shorts\xe2\x80\xa6";
    requests_.get("/video/shorts", 120000, [this](int status, const std::string& body) {
      if (status != 200) return message("Couldn't load Shorts", offlineOr(httpProblem(status, "/video/shorts")));
      ItemsPage page;
      parseItems(body, page);
      for (Item& i : page.items) items_.push_back(std::move(i));
      next_ = page.next;
      if (items_.empty()) return message("No Shorts right now", "Try again later");
      if (index_ < 0) play(0, 1);
    });
  }
  void message(const std::string& head, const std::string& detail) {
    title_ = head, subtitle_ = detail;
    message_ = true;
  }
  // more of the chain when few shorts are left
  void more() {
    if (next_.empty() || loadingMore_) return;
    loadingMore_ = true;
    requests_.get("/video/shorts?next=" + Api::escape(next_), 0, [this](int status, const std::string& body) {
      loadingMore_ = false;
      if (status != 200) return;  // the token is kept: the next short tries again
      ItemsPage page;
      parseItems(body, page);
      for (Item& n : page.items)
        if (std::none_of(items_.begin(), items_.end(), [&](const Item& i) { return i.id == n.id; })) items_.push_back(std::move(n));
      next_ = page.next;
    });
  }

  void play(int i, int direction) {
    if (i < 0 || i >= int(items_.size())) return;
    // the one leaving: its picture as it is now (or its first frame), and its details, to slide away
    if (index_ >= 0 && i != index_ && canvas_ && boxW_ > 0) {
      const Item* was = current();
      outgoing_.thumb = was ? was->thumb : "";
      outgoing_.title = detailTitle(), outgoing_.subtitle = detailSubtitle(), outgoing_.pos = position();
      outgoingFrame_ = false;
      VideoSurface* v = VideoSurface::made();
      if (shown_.picture && v && v->width() > 0 && v->height() > 0) {
        outgoingRect_ = videoRect(*v, boxW_, float(boxH_));
        outgoingFrame_ = canvas_->captureVideo(v->texture(), v->transform(), int(outgoingRect_.w), int(outgoingRect_.h), outgoing_);
      }
      dir_ = direction > 0 ? 1 : -1;
      // development: RONDO_SHORTS_SLIDE_MS slows it down, to look at it frame by frame
      static const int64_t ms = g_development && getenv("RONDO_SHORTS_SLIDE_MS") ? atoi(getenv("RONDO_SHORTS_SLIDE_MS")) : 300;
      slide_.begin(0, 1, now_ns(), ms * kMs, easeOutCubic);
    }
    index_ = i;
    step_ = direction;
    const Item& item = items_[size_t(i)];
    if (savedRepeat_ < 0) savedRepeat_ = g_player.state().repeat;
    if (VideoSurface* v = VideoSurface::get()) v->clear();  // the still until this one's video draws
    expectPath_ = videoSound(item), armedAt_ = -1, askedNs_ = now_ns();
    g_player.playVideo(videoSound(item), videoPictures(item, g_prefs.videoQuality));
    g_player.setRepeat(2);  // shorts loop, as in the app
    describe(item);
    warmAhead();
    if (int(items_.size()) - i <= 5) more();
  }

  // The next three: their details and the server resolving their streams (2-7 s each with yt-dlp), so
  // Next starts them in about a second. One the server can't stream leaves the list before anyone gets
  // to it.
  void warmAhead() {
    for (int k = index_ + 1; k <= index_ + 3 && k < int(items_.size()); k++) {
      Item n = items_[size_t(k)];
      describe(n);
      if (!warmed_.insert(n.id).second) continue;
      std::string id = n.id;
      requests_.get("/mpd/" + Api::escape(id), 0, [this, id](int status, const std::string&) {
        if (status == 200) {
          ready_.insert(id);
        } else if (status == 404 || status == 400) {
          auto it = std::find_if(items_.begin(), items_.end(), [&](const Item& i) { return i.id == id; });
          if (it != items_.end() && int(it - items_.begin()) > index_) items_.erase(it);
        } else {
          warmed_.erase(id);  // the network: tried again with the next short
        }
      });
    }
  }

  // The next short, unless the server is still resolving it and one of the two after it is ready: that
  // one moves up and plays first. Order hardly matters in Shorts; waiting does.
  int nextReady() {
    int i = index_ + 1;
    if (i >= int(items_.size()) || ready_.count(items_[size_t(i)].id)) return i;
    for (int j = i + 1; j <= std::min(i + 2, int(items_.size()) - 1); j++) {
      if (!ready_.count(items_[size_t(j)].id)) continue;
      Item moved = items_[size_t(j)];
      items_.erase(items_.begin() + j);
      items_.insert(items_.begin() + i, moved);
      return i;
    }
    return i;
  }

  void describe(const Item& item) {
    if (details_.count(item.id) || describing_.count(item.id)) return;
    describing_.insert(item.id);
    std::string id = item.id;
    requests_.get("/video/short/" + Api::escape(id), 600000, [this, id](int status, const std::string& body) {
      describing_.erase(id);
      ShortInfo info;
      if (status != 200 || !parseShortInfo(body, info)) return;
      details_[id] = {info.title, info.subtitle};
    });
  }

  std::vector<Item> items_;
  std::unordered_map<std::string, Details> details_;
  std::set<std::string> describing_, warmed_, ready_;
  std::string next_;
  bool loadingMore_ = false;
  int index_ = -1, step_ = 1, skippedAt_ = -1;
  int savedRepeat_ = -1;
  std::string title_, subtitle_;
  bool message_ = false;
  Shown shown_;
  Requests requests_;
  // the swipe between shorts
  struct Outgoing : Texture {
    std::string thumb, title, subtitle, pos;
  } outgoing_;
  bool outgoingFrame_ = false;  // outgoing_ holds the picture as it was (else its first frame is drawn)
  Rect outgoingRect_{};
  Tween slide_;
  int dir_ = 0;  // 1: the next came from below, -1: the previous from above
  Canvas* canvas_ = nullptr;
  int boxW_ = 0, boxH_ = 0;
  // the short asked for last: its sound's path, and the surface's picture count when its video began
  std::string expectPath_;
  mutable int64_t armedAt_ = -1, armedNs_ = 0;
  int64_t askedNs_ = 0;  // when it was asked for
};

}  // namespace

std::unique_ptr<Screen> makeShorts() { return std::unique_ptr<Screen>(new ShortsScreen()); }

}  // namespace rondo::shell
