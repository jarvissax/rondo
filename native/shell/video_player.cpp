// The full-screen video (VideoPlayerScreen in VideoScreens.kt, VideoPlayer.dc.html), ported 1:1: the
// picture fit to the screen on black, the overlays for 3 s after any input (and while paused, buffering,
// scrubbing or choosing a quality). Wheel = volume. Center = scrub (the wheel then moves through the
// video in steps that scale with its length, center again jumps); Next/Prev = ±10 s, or ±10 % while
// scrubbing; holding Next/Prev seeks faster the longer it is held; Menu leaves. Since 2.0 holding center
// opens the options: QUALITY (YouTube's), SUBTITLES (the video's tracks, or the files beside a card
// video) and DOWNLOAD (a YouTube video to the card, at a height picked there).
//
// The picture is rondo-player's (player/video.h) on the VideoSurface; its sound is the player's queue.
// The subtitles are drawn over it in the look Settings › Display › Subtitles sets (subtitles.h).
#include <stdio.h>

#include <functional>

#include "../platform/video_surface.h"
#include "app.h"
#include "downloads.h"
#include "screens.h"
#include "scrub.h"
#include "subtitles.h"

namespace rondo::shell {
namespace {

constexpr Color white = 0xffffffff;
inline Color whiteA(int a) { return uint32_t(a) << 24 | 0xffffff; }
inline Color blackA(int a) { return uint32_t(a) << 24; }

const int kChoices[4] = {0, 720, 480, 360};  // 0: Auto (adaptive, capped at 360p)

std::vector<std::string> pictures(const Item& item, int height) { return videoPictures(item, height); }

class VideoPlayerScreen : public Screen {
 public:
  explicit VideoPlayerScreen(const Item& item) : item_(item), local_(!item.id.empty() && item.id[0] == '/') {
    if (local_) {
      tracks_ = subtitleFiles(item_.id);
      tracksKnown_ = true;
    }
  }
  ~VideoPlayerScreen() override {
    if (started_) g_player.stopVideo();
  }
  std::string title() const override { return item_.title; }
  bool statusBar() const override { return false; }
  bool keepScreenOn() const override { return true; }  // the screen stays on here only (1.0.x)
  void onShow() override {
    if (!started_) {
      started_ = true;
      if (VideoSurface* v = VideoSurface::get()) v->clear();  // the last video's picture is not this one's
      g_player.playVideo(videoSound(item_), pictures(item_, g_prefs.videoQuality));
      if (!local_) {
        requests_.get("/captions/" + Api::escape(item_.id), 600000, [this](int status, const std::string& body) {
          std::vector<CaptionTrack> found;
          if (status == 200) parseCaptions(body, found);
          for (const CaptionTrack& c : found) tracks_.push_back({c.id, c.lang, c.name, c.autoGen, ""});
          tracksKnown_ = true;
          if (g_prefs.subtitles) showTrack(pickTrack(tracks_));
        });
      } else if (g_prefs.subtitles) {
        showTrack(pickTrack(tracks_));
      }
    }
    showOverlay();
  }

  bool onAction(Action a, int steps) override {
    showOverlay();
    if (panel_) {
      std::vector<PanelRow> rows = panelRows();
      switch (a) {
        case Action::Up:
          for (int k = 0; k < std::max(1, steps); k++) cursor_ = moveCursor(rows, cursor_, -1);
          break;
        case Action::Down:
          for (int k = 0; k < std::max(1, steps); k++) cursor_ = moveCursor(rows, cursor_, +1);
          break;
        case Action::Select:
          if (cursor_ >= 0 && cursor_ < int(rows.size()) && rows[size_t(cursor_)].pick) {
            panel_ = false;
            rows[size_t(cursor_)].pick();
          }
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
      case Action::LongSelect: {
        scrub_.cancel();
        std::vector<PanelRow> rows = panelRows();
        cursor_ = moveCursor(rows, -1, +1);
        for (size_t i = 0; i < rows.size(); i++)
          if (rows[i].checked) {  // the first section's choice in use
            cursor_ = int(i);
            break;
          }
        panel_ = true;
        break;
      }
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
    if (panel_) {  // a download's progress, the tracks arriving
      std::string key = panelKey();
      if (key != panelKey_) panelKey_ = key, jumped = true;
    }
    Shown s = shown(now);
    bool changed = jumped || !(s == shown_);
    shown_ = s;
    return changed;
  }
  int64_t wakeAt() const override {
    int64_t w = scrub_.deadline();
    if (shown_.overlay && !shown_.keep && overlayUntil_ && (!w || overlayUntil_ < w)) w = overlayUntil_;
    if (subWake_ && (!w || subWake_ < w)) w = subWake_;
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
    float subBottom = h - e.dpf(12);
    if (shown_.overlay) {
      drawTop(e, w);
      subBottom = drawBottom(e, w, h, s) - e.dpf(6);
    }
    if (!cues_.empty()) drawSubtitles(e, cues_, PlayerHost::positionAt(s, e.now), w, subBottom);
    if (panel_) drawPanel(e, w, h);
    if (scrub_.active) drawScrubBubble(e, w, h, scrub_.target, scrub_.from);
    e.c.layer(0);
  }

 private:
  struct Shown {
    bool overlay = false, keep = false, playing = false;
    int64_t sec = -1, durSec = 0;
    int height = 0, bar = 0, buffered = 0;
    uint64_t cues = 0;  // which subtitles show
    bool operator==(const Shown& o) const {
      return overlay == o.overlay && keep == o.keep && playing == o.playing && sec == o.sec && durSec == o.durSec &&
             height == o.height && bar == o.bar && buffered == o.buffered && cues == o.cues;
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
    subWake_ = 0;
    if (!cues_.empty()) {
      int64_t next = -1;
      v.cues = cuesAt(cues_, pos, &next);
      // the next change, while it plays (frames come with the picture anyway; this is for no picture)
      if (next > pos && playing) subWake_ = now + (next - pos) * kMs;
    }
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
  float drawBottom(Env& e, float w, float h, const PlayerState& s) {
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
                       : local_      ? "center: scrub \xc2\xb7 hold center: subtitles"
                                     : "center: scrub \xc2\xb7 hold center: options";
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
    return top;
  }
  std::string qualityText(const PlayerState& s) const {
    int height = s.videoHeight;
    if (!height) return "";
    std::string p = std::to_string(height) + "p";
    if (local_) return p;
    return g_prefs.videoQuality == 0 ? "Auto \xc2\xb7 " + p : p;
  }

  // The options, a list of sections: QUALITY (YouTube's: Auto and the three heights), SUBTITLES (Off
  // and the tracks), DOWNLOAD (YouTube's: the three heights, or where it stands); a check at each in use
  struct PanelRow {
    bool header = false;
    std::string text, note;  // note: on the right, dimmed
    bool checked = false;
    std::function<void()> pick;  // null: not choosable (a header, "None for this video")
  };
  std::vector<PanelRow> panelRows() const {
    std::vector<PanelRow> rows;
    auto header = [&](const char* t) {
      PanelRow r;
      r.header = true, r.text = t;
      rows.push_back(r);
    };
    VideoPlayerScreen* self = const_cast<VideoPlayerScreen*>(this);
    if (!local_) {
      header("QUALITY");
      for (int q : kChoices) {
        PanelRow r;
        r.text = q ? std::to_string(q) + "p" : "Auto";
        r.checked = g_prefs.videoQuality == q;
        r.pick = [self, q] { self->setQuality(q); };
        rows.push_back(r);
      }
    }
    header("SUBTITLES");
    {
      PanelRow off;
      off.text = "Off";
      off.checked = track_ < 0;
      off.pick = [self] {
        g_prefs.subtitles = false;
        savePrefsNow();
        self->showTrack(-1);
      };
      rows.push_back(off);
    }
    if (!tracksKnown_ || tracks_.empty()) {
      PanelRow r;
      r.text = tracksKnown_ ? "None for this video" : "Finding subtitles\xe2\x80\xa6";
      rows.push_back(r);
    }
    for (size_t i = 0; i < tracks_.size(); i++) {
      PanelRow r;
      r.text = tracks_[i].name;
      r.checked = track_ == int(i);
      r.pick = [self, i] {
        g_prefs.subtitles = true;
        g_prefs.subLang = cleanLang(self->tracks_[i].lang);
        savePrefsNow();
        self->showTrack(int(i));
      };
      rows.push_back(r);
    }
    if (!local_) {
      header("DOWNLOAD");
      const DownloadJob* job = nullptr;
      for (const DownloadJob& j : downloadJobs())
        if (j.id == item_.id && j.state != DownloadJob::Failed) job = &j;
      if (!downloadedFile(item_.id).empty()) {
        PanelRow r;
        r.text = "On the card";
        r.checked = true;
        rows.push_back(r);
      } else if (job && job->state != DownloadJob::Done) {
        PanelRow r;
        r.text = job->state == DownloadJob::Waiting ? "Waiting" : job->stage.empty() ? "Downloading" : job->stage;
        r.note = job->state == DownloadJob::Running ? std::to_string(job->percent) + "%" : "";
        rows.push_back(r);
        PanelRow c;
        c.text = "Cancel download";
        std::string id = item_.id;
        c.pick = [id] { cancelDownload(id); };
        rows.push_back(c);
      } else {
        for (int q : {720, 480, 360}) {
          PanelRow r;
          r.text = std::to_string(q) + "p";
          r.note = "to the card";
          Item it = item_;
          r.pick = [it, q] { downloadVideo(it, q); };
          rows.push_back(r);
        }
      }
    }
    return rows;
  }
  // what the panel shows that changes by itself (a track list arriving, a download going)
  std::string panelKey() const {
    std::string k;
    for (const PanelRow& r : panelRows()) k += r.text + r.note + (r.checked ? "*" : "") + "|";
    return k;
  }
  // the next row that can be picked from at, going dir (round the ends)
  static int moveCursor(const std::vector<PanelRow>& rows, int at, int dir) {
    int n = int(rows.size());
    for (int k = 1; k <= n; k++) {
      int i = ((at + dir * k) % n + n) % n;
      if (rows[size_t(i)].pick) return i;
    }
    return at;
  }

  // A track's cues: a card file read at once, a YouTube track fetched (-1: none shown)
  void showTrack(int i) {
    track_ = i < int(tracks_.size()) ? i : -1;
    cues_.clear();
    shown_.cues = 0;
    if (track_ < 0) return;
    const SubTrack& t = tracks_[size_t(track_)];
    if (!t.file.empty()) {
      FILE* f = fopen(t.file.c_str(), "rb");
      if (!f) return;
      std::string body;
      char buf[16384];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && body.size() < (8u << 20)) body.append(buf, n);
      fclose(f);
      cues_ = parseSubtitles(std::move(body));
      return;
    }
    int want = track_;
    requests_.get("/captions/" + Api::escape(item_.id) + "/" + Api::escape(t.id), 600000, [this, want](int status, const std::string& body) {
      if (status != 200 || track_ != want) return;
      cues_ = parseSubtitles(body);
    });
  }

  // 180 units wide in the middle; taller than the screen, it scrolls to keep the row picked in view
  void drawPanel(Env& e, float w, float h) {
    std::vector<PanelRow> rows = panelRows();
    int pw = std::min(e.dp(180), int(w) - 2 * e.dp(10)), padV = e.dp(5), rowH = e.dp(24), side = e.dp(12);
    Label probe = label(e, "QUALITY", 10, true);
    int headH = e.dp(3) + probe.h + e.dp(5);
    int content = 0, cursorTop = 0;
    for (size_t i = 0; i < rows.size(); i++) {
      if (int(i) == cursor_) cursorTop = content;
      content += rows[i].header ? headH : rowH;
    }
    int ph = std::min(padV + content + padV, int(h) - 2 * e.dp(8));
    int view = ph - 2 * padV;
    float scroll = float(std::max(0, std::min(content - view, cursorTop + rowH / 2 - view / 2)));
    float px = float((int(w) - pw) / 2), py = float((int(h) - ph) / 2);
    e.c.layer(5);
    e.c.shape({px, py, float(pw), float(ph)}, e.dpf(8), 0xf0212121);
    e.c.pushClip({px, py + padV, float(pw), float(view)});
    float y = py + padV - scroll;
    for (size_t i = 0; i < rows.size(); i++) {
      const PanelRow& r = rows[i];
      if (r.header) {
        e.c.layer(6);
        drawLabel(e, label(e, r.text, 10, true), px + side, y + e.dp(3), e.p.darkMuted);
        y += headH;
        continue;
      }
      if (int(i) == cursor_) {
        e.c.layer(5);
        e.c.shape({px, y, float(pw), float(rowH)}, e.dpf(4), whiteA(70));
      }
      e.c.layer(6);
      int cw = e.dp(12), ch = e.dp(10);
      Label note;
      if (!r.note.empty()) note = label(e, r.note, 10, false);
      float right = px + pw - side - (r.checked ? cw + e.dp(6) : 0) - (note.run ? note.w + e.dp(6) : 0);
      Label l = label(e, r.text, 13, false, std::max(0.f, right - (px + side)));
      drawLabel(e, l, px + side, y + float((rowH - l.h) / 2), r.pick || r.checked ? white : whiteA(150));
      if (note.run) drawLabel(e, note, px + pw - side - (r.checked ? cw + e.dp(6) : 0) - note.w, y + float((rowH - note.h) / 2), whiteA(150));
      if (r.checked) e.icons.draw(e.c, Icon::Check, {px + pw - side - cw, y + float((rowH - ch) / 2), float(cw), float(ch)}, white);
      y += rowH;
    }
    e.c.popClip();
  }

  Item item_;
  bool local_;
  bool started_ = false;
  Requests requests_;
  std::vector<SubTrack> tracks_;
  bool tracksKnown_ = false;
  int track_ = -1;            // the track shown (-1: subtitles off)
  std::vector<Cue> cues_;
  mutable int64_t subWake_ = 0;  // when the subtitles change next (ns), while playing
  std::string panelKey_;
  Scrubber scrub_;
  bool panel_ = false;
  int cursor_ = 0;
  int64_t overlayUntil_ = 0;
  Shown shown_;
};

}  // namespace

// The server's H.264 DASH streams (136 is 720p, 135 480p, 134 360p). A height the video doesn't have
// falls back to the next one down (direct mode's YouTube client often has no 720p: 480p, not 360p).
std::vector<std::string> videoPictures(const Item& item, int height) {
  if (!item.id.empty() && item.id[0] == '/') return {item.id};
  std::string base = "/s/" + item.id + "/";
  std::vector<std::string> out;
  if (height == 720) out.push_back(base + "136");
  if (height == 720 || height == 480) out.push_back(base + "135");
  out.push_back(base + "134");
  return out;
}

std::string videoSound(const Item& item) { return !item.id.empty() && item.id[0] == '/' ? item.id : "/s/" + item.id + "/140"; }

void openVideo(const Item& item) { g_app->push(std::unique_ptr<Screen>(new VideoPlayerScreen(item))); }

}  // namespace rondo::shell
