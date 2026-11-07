// The full-screen video (VideoPlayerScreen in VideoScreens.kt, VideoPlayer.dc.html), ported 1:1: the
// picture fit to the screen on black, the overlays for 3 s after any input (and while paused, buffering,
// scrubbing or choosing a quality). Wheel = volume. Center = scrub (the wheel then moves through the
// video in steps that scale with its length, center again jumps); Next/Prev = ±10 s, or ±10 % while
// scrubbing; holding Next/Prev seeks faster the longer it is held; holding center = quality; Menu leaves.
//
// The picture is rondo-player's (player/video.h) on the VideoSurface; its sound is the player's queue.
#include "../platform/video_surface.h"
#include "app.h"
#include "screens.h"
#include "scrub.h"

namespace rondo::shell {
namespace {

constexpr Color white = 0xffffffff;
inline Color whiteA(int a) { return uint32_t(a) << 24 | 0xffffff; }
inline Color blackA(int a) { return uint32_t(a) << 24; }

const int kChoices[4] = {0, 720, 480, 360};  // 0: Auto (adaptive, capped at 360p)

std::vector<std::string> pictures(const Item& item, int height) { return videoPictures(item, height); }

class VideoPlayerScreen : public Screen {
 public:
  explicit VideoPlayerScreen(const Item& item) : item_(item), local_(!item.id.empty() && item.id[0] == '/') {}
  ~VideoPlayerScreen() override {
    if (started_) g_player.stopVideo();
  }
  std::string title() const override { return item_.title; }
  bool statusBar() const override { return false; }
  void onShow() override {
    if (!started_) {
      started_ = true;
      if (VideoSurface* v = VideoSurface::get()) v->clear();  // the last video's picture is not this one's
      g_player.playVideo(videoSound(item_), pictures(item_, g_prefs.videoQuality));
    }
    showOverlay();
  }

  bool onAction(Action a, int steps) override {
    showOverlay();
    if (panel_) {
      switch (a) {
        case Action::Up: cursor_ = (cursor_ + 3) % 4; break;
        case Action::Down: cursor_ = (cursor_ + 1) % 4; break;
        case Action::Select:
          setQuality(kChoices[cursor_]);
          panel_ = false;
          break;
        case Action::Back: panel_ = false; break;
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
        else g_app->changeVolume(dir);
        break;
      }
      case Action::Select:
        if (scrubbing) scrub_.commit();
        else scrub_.begin();
        break;
      case Action::LongSelect:
        if (!local_) {
          scrub_.cancel();
          cursor_ = 0;
          for (int i = 0; i < 4; i++)
            if (kChoices[i] == g_prefs.videoQuality) cursor_ = i;
          panel_ = true;
        }
        break;
      case Action::Back:
        if (!scrubbing) return false;  // Menu leaves
        scrub_.cancel();
        break;
      case Action::Next:
        if (scrubbing) scrub_.jump(+1);
        else g_player.seekBy(10000);
        break;
      case Action::Prev:
        if (scrubbing) scrub_.jump(-1);
        else g_player.seekBy(-10000);
        break;
      case Action::LongNext: scrub_.hold(+1); break;
      case Action::LongPrev: scrub_.hold(-1); break;
      case Action::SeekEnd: scrub_.commit(); break;
      case Action::PlayPause: g_player.togglePlay(); break;
    }
    return true;
  }

  bool tick(int64_t now) override {
    bool jumped = scrub_.tick(now);
    Shown s = shown(now);
    bool changed = jumped || !(s == shown_);
    shown_ = s;
    return changed;
  }
  int64_t wakeAt() const override {
    int64_t w = scrub_.deadline();
    if (shown_.overlay && !shown_.keep && overlayUntil_ && (!w || overlayUntil_ < w)) w = overlayUntil_;
    return w;
  }

  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, 0xff000000);
    VideoSurface* v = VideoSurface::made();
    if (v && v->hasPicture() && v->width() > 0 && v->height() > 0) {
      // AspectRatioFrameLayout's RESIZE_MODE_FIT
      float s = std::min(w / float(v->width()), h / float(v->height()));
      float vw = floorf(v->width() * s + 0.5f), vh = floorf(v->height() * s + 0.5f);
      e.c.video(v->texture(), {floorf((w - vw) / 2), floorf((h - vh) / 2), vw, vh}, v->transform());
    }
    PlayerState s = g_player.state();
    shown_ = shown(e.now, &s);
    if (shown_.overlay) {
      drawTop(e, w);
      drawBottom(e, w, h, s);
    }
    if (panel_) drawPanel(e, w, h);
    if (scrub_.active) drawScrubBubble(e, w, h, scrub_.target, scrub_.from);
    e.c.layer(0);
  }

 private:
  struct Shown {
    bool overlay = false, keep = false, playing = false;
    int64_t sec = -1, durSec = 0;
    int height = 0, bar = 0, buffered = 0;
    bool operator==(const Shown& o) const {
      return overlay == o.overlay && keep == o.keep && playing == o.playing && sec == o.sec && durSec == o.durSec &&
             height == o.height && bar == o.bar && buffered == o.buffered;
    }
  };
  // what the overlays show now (a frame is drawn when it changes)
  Shown shown(int64_t now, const PlayerState* given = nullptr) const {
    PlayerState s = given ? *given : g_player.state();
    Shown v;
    bool playing = s.playing && !s.buffering;
    v.keep = panel_ || scrub_.active || !playing || s.buffering;
    v.overlay = v.keep || now < overlayUntil_;
    v.playing = playing;
    int64_t d = duration(s), pos = PlayerHost::positionAt(s, now);
    v.sec = (scrub_.active ? scrub_.target : pos) / 1000, v.durSec = d / 1000;
    v.height = s.videoHeight;
    v.bar = d > 0 ? int(pos * 900 / d) : 0;  // a half-pixel step of the thumb
    v.buffered = d > 0 ? int(s.bufferedMs * 450 / d) : 0;
    return v;
  }
  void showOverlay() { overlayUntil_ = g_app->now() + 3000 * kMs; }
  int64_t duration(const PlayerState& s) const {
    return s.durationMs > 0 ? s.durationMs : std::max(0, item_.durationSec) * 1000LL;
  }
  void setQuality(int height) {
    g_prefs.videoQuality = height;
    savePrefsNow();
    g_player.setVideoQuality(pictures(item_, height));
  }

  // the title (13, bold, two lines) and the channel over a gradient from the top
  void drawTop(Env& e, float w) {
    int padL = e.dp(10), padT = e.dp(7), padR = e.dp(10), padB = e.dp(14);
    float tw = w - padL - padR;
    std::vector<Label> title = wrap(e, item_.title, 13, true, tw, 2);
    Label sub = label(e, item_.subtitle, 10, false, tw);
    int titleH = 0;
    for (const Label& l : title) titleH += l.h;
    if (title.empty()) titleH = label(e, " ", 13, true).h;
    float topH = float(padT + titleH + e.dp(2) + sub.h + padB);
    e.c.layer(1);
    e.c.fillGradient({0, 0, w, topH}, blackA(215), 0);
    e.c.layer(3);
    float y = float(padT);
    for (const Label& l : title) drawLabel(e, l, float(padL), y, white), y += l.h;
    if (title.empty()) y += titleH;
    drawLabel(e, sub, float(padL), y + e.dp(2), whiteA(200));
  }

  // the progress (buffered in light gray), then the state, the times, the hint and the quality
  void drawBottom(Env& e, float w, float h, const PlayerState& s) {
    int padL = e.dp(8), padT = e.dp(16), padR = e.dp(10), padB = e.dp(5);
    int progH = e.dp(12), icon = e.dp(9);
    int64_t d = duration(s), pos = PlayerHost::positionAt(s, e.now);
    bool playing = s.playing && !s.buffering;
    Label time = label(e, formatTime(int((scrub_.active ? scrub_.target : pos) / 1000)) + " / " + formatTime(int(d / 1000)), 11, false);
    std::string q = qualityText(s);
    Label ql;
    int qw = 0, qh = 0;
    if (!q.empty()) {
      ql = label(e, q, 10, true);
      qw = ql.w + 2 * e.dp(4), qh = ql.h + 2 * e.dp(1);
    }
    const char* hint = scrub_.active ? "center jumps \xc2\xb7 Menu cancels"
                       : local_      ? "center: scrub"
                                     : "center: scrub \xc2\xb7 hold center: quality";
    float rowX = float(padL + e.dp(4)), rowRight = w - padR;
    float hintX = rowX + icon + e.dp(7) + time.w + e.dp(8);
    float hintW = rowRight - qw - e.dp(8) - hintX;
    Label hl = label(e, hint, 10, false, std::max(0.f, hintW));
    int rowH = std::max(std::max(icon, time.h), std::max(hl.h, qh));
    float bottomH = float(padT + progH + e.dp(2) + rowH + padB);
    float top = h - bottomH;
    e.c.layer(1);
    e.c.fillGradient({0, top, w, bottomH}, 0, blackA(225));
    e.c.layer(2);
    progressBar(e, {float(padL), top + padT, w - padL - padR, float(progH)}, whiteA(90), e.p.red,
                d > 0 ? float(pos) / d : 0, 9, 3, scrub_.fraction(d), whiteA(130), d > 0 ? float(s.bufferedMs) / d : 0);
    float ry = top + padT + progH + e.dp(2);
    if (!q.empty()) e.c.shape({rowRight - qw, ry + float((rowH - qh) / 2), float(qw), float(qh)}, e.dpf(3), whiteA(70));
    e.c.layer(3);
    e.icons.draw(e.c, playing ? Icon::Play : Icon::Pause, {rowX, ry + float((rowH - icon) / 2), float(icon), float(icon)}, white);
    drawLabel(e, time, rowX + icon + e.dp(7), ry + float((rowH - time.h) / 2), white);
    if (hintW > 0) drawLabel(e, hl, centerText(hintX, hintW, hl.w), ry + float((rowH - hl.h) / 2), whiteA(170));
    if (!q.empty()) drawLabel(e, ql, rowRight - qw + e.dp(4), ry + float((rowH - qh) / 2) + e.dp(1), white);
  }
  std::string qualityText(const PlayerState& s) const {
    int height = s.videoHeight;
    if (!height) return "";
    std::string p = std::to_string(height) + "p";
    if (local_) return p;
    return g_prefs.videoQuality == 0 ? "Auto \xc2\xb7 " + p : p;
  }

  // QUALITY: 130 units wide in the middle, Auto and the three heights, a check at the one in use
  void drawPanel(Env& e, float w, float h) {
    int pw = e.dp(130), padV = e.dp(5), rowH = e.dp(26), side = e.dp(12);
    Label head = label(e, "QUALITY", 10, true);
    int headH = e.dp(3) + head.h + e.dp(5);
    int ph = padV + headH + 4 * rowH + padV;
    float px = float((int(w) - pw) / 2), py = float((int(h) - ph) / 2);
    e.c.layer(5);
    e.c.shape({px, py, float(pw), float(ph)}, e.dpf(8), 0xf0212121);
    e.c.layer(6);
    drawLabel(e, head, px + side, py + padV + e.dp(3), e.p.darkMuted);
    for (int i = 0; i < 4; i++) {
      float ry = py + padV + headH + float(i * rowH);
      if (i == cursor_) {
        e.c.layer(5);
        e.c.shape({px, ry, float(pw), float(rowH)}, e.dpf(4), whiteA(70));
        e.c.layer(6);
      }
      std::string name = kChoices[i] ? std::to_string(kChoices[i]) + "p" : "Auto";
      int cw = e.dp(12), ch = e.dp(10);
      Label l = label(e, name, 13, false, float(pw - 2 * side - cw));
      drawLabel(e, l, px + side, ry + float((rowH - l.h) / 2), white);
      if (kChoices[i] == g_prefs.videoQuality)
        e.icons.draw(e.c, Icon::Check, {px + pw - side - cw, ry + float((rowH - ch) / 2), float(cw), float(ch)}, white);
    }
  }

  Item item_;
  bool local_;
  bool started_ = false;
  Scrubber scrub_;
  bool panel_ = false;
  int cursor_ = 0;
  int64_t overlayUntil_ = 0;
  Shown shown_;
};

}  // namespace

// The server's H.264 DASH streams (136 is 720p, 135 480p, 134 360p). A height the video doesn't have
// falls back to Auto's 360p, as QualityTrackSelection does.
std::vector<std::string> videoPictures(const Item& item, int height) {
  if (!item.id.empty() && item.id[0] == '/') return {item.id};
  std::string base = "/s/" + item.id + "/";
  std::vector<std::string> out;
  if (height == 720) out.push_back(base + "136");
  if (height == 480) out.push_back(base + "135");
  out.push_back(base + "134");
  return out;
}

std::string videoSound(const Item& item) { return !item.id.empty() && item.id[0] == '/' ? item.id : "/s/" + item.id + "/140"; }

void openVideo(const Item& item) { g_app->push(std::unique_ptr<Screen>(new VideoPlayerScreen(item))); }

}  // namespace rondo::shell
