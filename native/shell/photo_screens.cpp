// Library › Photos: the pictures on the card and the internal storage (JPEG and PNG, turned the way
// their camera says), by folder, as a grid of thumbnails (the highlight glides), and each one full
// screen. In the viewer the wheel (or Next and Prev) slides from one to the next, center shows its name
// and date, Play starts and stops a slideshow (a photo every 4 s, each fading into the next).
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <GLES2/gl2.h>

#include <algorithm>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "../ui/image.h"
#include "app.h"
#include "main_queue.h"
#include "screens.h"

namespace rondo::shell {

namespace {

constexpr ui::Color white = 0xffffffff;
inline ui::Color whiteA(int a) { return uint32_t(a) << 24 | 0xffffff; }

struct Photo {
  std::string path;
  int64_t modified = 0;  // s since 1970
};

// album art beside songs isn't a photo
bool isCoverName(const char* n) {
  static const char* const covers[] = {"cover.", "folder.", "front.", "albumart", "back."};
  for (const char* c : covers)
    if (!strncasecmp(n, c, strlen(c))) return true;
  return false;
}

void walk(const std::string& dir, int depth, std::vector<Photo>& out) {
  if (depth > 8 || out.size() > 20000) return;
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> dirs;
  while (dirent* e = readdir(d)) {
    const char* n = e->d_name;
    if (n[0] == '.' || !strcmp(n, "Android") || !strcmp(n, "LOST.DIR") || !strcmp(n, "System Volume Information")) continue;
    if (depth == 0 && !strcasecmp(n, "Themes")) continue;  // the stock firmware's theme pictures, on the card's top
    std::string p = dir + "/" + n;
    struct stat st;
    if (stat(p.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) dirs.push_back(p);
    else if (ui::isPhotoName(n) && !isCoverName(n) && st.st_size >= 4096) out.push_back({p, int64_t(st.st_mtime)});
  }
  closedir(d);
  for (const std::string& p : dirs) walk(p, depth + 1, out);
}

std::string folderName(const std::string& dir) {
  for (const char* r : {"/storage/sdcard0", "/storage/sdcard1"})
    if (dir == r) return std::string(r) == "/storage/sdcard0" ? "Card" : "Internal storage";
  return dir.substr(dir.rfind('/') + 1);
}

std::string dateOf(int64_t s) {
  time_t t = time_t(s);
  struct tm tm;
  localtime_r(&t, &tm);
  char b[64];
  strftime(b, sizeof(b), "%b %e, %Y", &tm);
  return b;
}

// ---- the viewer ----

// One photo decoded at the screen's size on a thread of its own: the newest asked wins
class PhotoDecoder {
 public:
  using Done = std::function<void(const std::string& path, std::shared_ptr<ui::Pixels>)>;
  explicit PhotoDecoder(int w, int h) : w_(w), h_(h) {
    thread_ = std::thread([this] { run(); });
  }
  ~PhotoDecoder() {
    {
      std::lock_guard<std::mutex> l(mu_);
      stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
  }
  void ask(const std::string& path, Done done) {
    std::lock_guard<std::mutex> l(mu_);
    want_ = path, done_ = std::move(done);
    cv_.notify_one();
  }

 private:
  void run() {
    for (;;) {
      std::string path;
      Done done;
      {
        std::unique_lock<std::mutex> l(mu_);
        cv_.wait(l, [this] { return stop_ || !want_.empty(); });
        if (stop_) return;
        path.swap(want_), done.swap(done_);
      }
      auto px = std::make_shared<ui::Pixels>();
      std::vector<uint8_t> bytes;
      if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (48 << 20)) {
          bytes.resize(size_t(n));
          if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        }
        fclose(f);
      }
      int pw, ph;
      if (!bytes.empty() && ui::photoSize(bytes.data(), bytes.size(), pw, ph) && pw > 0 && ph > 0) {
        float scale = std::min(1.f, std::min(float(w_) / pw, float(h_) / ph));
        int tw = std::max(1, int(pw * scale + 0.5f)), th = std::max(1, int(ph * scale + 0.5f));
        if (!ui::decodePhoto(bytes.data(), bytes.size(), tw, th, *px)) px->w = 0;
      }
      done(path, px);
    }
  }
  int w_, h_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::string want_;
  Done done_;
  bool stop_ = false;
  std::thread thread_;
};

class PhotoViewerScreen : public Screen {
 public:
  PhotoViewerScreen(std::shared_ptr<std::vector<Photo>> photos, int at) : photos_(std::move(photos)), at_(at) {}
  ~PhotoViewerScreen() override {
    *alive_ = false;
    decoder_.reset();
    for (auto& kv : textures_) ui::destroyTexture(kv.second);
  }
  std::string title() const override { return "Photos"; }
  bool statusBar() const override { return false; }
  bool keepScreenOn() const override { return slideshow_; }

  bool onAction(Action a, int steps) override {
    switch (a) {
      case Action::Up:
      case Action::Prev: go(at_ - std::max(1, a == Action::Up ? steps : 1), false); return true;
      case Action::Down:
      case Action::Next: go(at_ + std::max(1, a == Action::Down ? steps : 1), false); return true;
      case Action::Select: info_ = !info_; return true;
      case Action::PlayPause:
        slideshow_ = !slideshow_;
        nextSlide_ = now_ns() + 4000 * kMs;
        g_app->toast(slideshow_ ? "Slideshow" : "Slideshow stopped");
        return true;
      default: return false;
    }
  }
  bool tick(int64_t now) override {
    if (slideshow_ && now >= nextSlide_) {
      nextSlide_ = now + 4000 * kMs;
      go(at_ + 1 < int(photos_->size()) ? at_ + 1 : 0, true);
    }
    bool a = arrived_;
    arrived_ = false;
    return a;
  }
  // every frame while a photo slides or fades in
  bool step(float) override {
    int64_t now = now_ns();
    return (moveAt_ && now - moveAt_ < kMoveNs + 20 * kMs) || (shownAt_ && now - shownAt_ < 400 * kMs);
  }
  int64_t wakeAt() const override { return slideshow_ ? nextSlide_ : 0; }

  void draw(Env& e, float w, float h) override {
    if (!decoder_) {  // the first draw knows the screen's size
      decoder_.reset(new PhotoDecoder(int(w), int(h)));
      want();
    }
    e.c.fill({0, 0, w, h}, rgb(0x000000));
    auto show = [&](int index, float dx, float alpha) {
      auto it = textures_.find(index);
      if (it == textures_.end() || !it->second.id || alpha <= 0) return;
      const ui::Texture& tex = it->second;
      float sc = std::min(w / tex.w, h / tex.h);
      float dw = tex.w * sc, dh = tex.h * sc;
      e.c.image(tex, {(w - dw) / 2 + dx, (h - dh) / 2, dw, dh}, {0, 0, 1, 1}, 0, withAlpha(white, alpha));
    };
    float t = moveAt_ ? std::min(1.f, float(e.now - moveAt_) / float(kMoveNs)) : 1.f;
    float ease = 1.f - (1.f - t) * (1.f - t) * (1.f - t);  // fast, then settling
    // a photo decoded after the turn fades in where it is
    float in = shownAt_ ? std::min(1.f, float(e.now - shownAt_) / 300e6f) : 1.f;
    e.c.layer(1);
    if (t < 1 && from_ >= 0 && from_ != at_) {
      if (fade_) show(from_, 0, 1 - ease);
      else show(from_, -dir_ * ease * w, 1);
    }
    e.c.layer(2);
    if (t < 1 && !fade_) show(at_, dir_ * (1 - ease) * w, in);
    else show(at_, 0, fade_ && t < 1 ? ease * in : in);
    bool have = textures_.count(at_) && textures_[at_].id;
    if (!have) {
      bool failed = failed_.count(at_) != 0;
      Label l = label(e, failed ? "Rondo can't open this picture" : "Opening\xe2\x80\xa6", 13, false, w);
      float dx = t < 1 && !fade_ ? dir_ * (1 - ease) * w : 0;
      drawLabel(e, l, (w - l.w) / 2 + dx, (h - l.h) / 2, whiteA(150));
    }
    if (info_ || !have) {
      const Photo& p = (*photos_)[size_t(at_)];
      e.c.layer(5);
      float bh = float(e.dp(44));
      e.c.fillGradient({0, h - bh, w, bh}, 0x00000000, 0xcc000000);
      e.c.layer(6);
      std::string name = p.path.substr(p.path.rfind('/') + 1);
      Label l1 = label(e, name, 13, true, w - e.dp(110));
      Label l2 = label(e, dateOf(p.modified), 11, false, w - e.dp(110));
      drawLabel(e, l1, float(e.dp(12)), h - bh + e.dp(6), white);
      drawLabel(e, l2, float(e.dp(12)), h - bh + e.dp(6) + l1.h, whiteA(170));
      char c[32];
      snprintf(c, sizeof(c), "%d of %d", at_ + 1, int(photos_->size()));
      Label l3 = label(e, c, 11, false, w);
      drawLabel(e, l3, w - l3.w - e.dp(12), h - bh + e.dp(6) + l1.h, whiteA(170));
    }
  }

 private:
  static constexpr int64_t kMoveNs = 320 * kMs;
  void go(int i, bool fade) {
    int n = int(photos_->size());
    i = std::max(0, std::min(n - 1, i));
    if (i == at_) return;
    dir_ = i > at_ ? 1 : -1;
    if (fade) dir_ = 1;
    from_ = at_, at_ = i, fade_ = fade;
    moveAt_ = now_ns();
    shownAt_ = 0;
    keep();
    want();
  }
  // the photos kept as textures: the one shown, the one it came from (sliding out) and its neighbors
  void keep() {
    for (auto it = textures_.begin(); it != textures_.end();) {
      int k = it->first;
      if (std::abs(k - at_) <= 1 || k == from_) {
        ++it;
        continue;
      }
      ui::destroyTexture(it->second);
      it = textures_.erase(it);
    }
  }
  // the next to decode: this one, then the one after, then the one before (the newest asked wins)
  void want() {
    if (!decoder_) return;
    int n = int(photos_->size());
    for (int k : {at_, at_ + dir_, at_ - dir_}) {
      if (k < 0 || k >= n || textures_.count(k) || failed_.count(k)) continue;
      ask(k);
      return;
    }
  }
  void ask(int k) {
    auto alive = alive_;
    std::string path = (*photos_)[size_t(k)].path;
    decoder_->ask(path, [this, alive, k](const std::string&, std::shared_ptr<ui::Pixels> px) {
      postToMain([this, alive, k, px] {  // the texture made on the UI thread
        if (!*alive) return;
        if (px->w > 0) {
          if (textures_.count(k)) ui::destroyTexture(textures_[k]);
          textures_[k] = ui::createTexture(px->w, px->h, GL_RGBA, GL_UNSIGNED_BYTE, px->rgba.data(), true);
          if (k == at_) shownAt_ = now_ns();  // what was waited for: in, softly
        } else {
          failed_.insert(k);
        }
        keep();
        want();
        arrived_ = true;  // tick draws it
      });
    });
  }
  std::shared_ptr<std::vector<Photo>> photos_;
  int at_, from_ = -1, dir_ = 1;
  std::unique_ptr<PhotoDecoder> decoder_;
  std::map<int, ui::Texture> textures_;
  std::set<int> failed_;
  int64_t moveAt_ = 0, shownAt_ = 0, nextSlide_ = 0;
  bool info_ = false, slideshow_ = false, fade_ = false, arrived_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// ---- the grid ----

class PhotoGridScreen : public Screen {
 public:
  PhotoGridScreen(std::string title, std::shared_ptr<std::vector<Photo>> photos) : title_(std::move(title)), photos_(std::move(photos)) {}
  std::string title() const override { return title_; }
  bool onAction(Action a, int steps) override {
    int n = int(photos_->size());
    if (!n) return false;
    switch (a) {
      case Action::Up: sel_ = std::max(0, sel_ - std::max(1, steps)); return true;
      case Action::Down: sel_ = std::min(n - 1, sel_ + std::max(1, steps)); return true;
      case Action::Prev: sel_ = std::max(0, sel_ - kCols); return true;
      case Action::Next: sel_ = std::min(n - 1, sel_ + kCols); return true;
      case Action::Select: g_app->push(std::unique_ptr<Screen>(new PhotoViewerScreen(photos_, sel_))); return true;
      default: return false;
    }
  }
  bool step(float dt) override {  // the rows glide to keep the selection in view, the highlight to it
    bool moving = false;
    auto glide = [&](float& v, float to) {
      float d = to - v;
      if (std::fabs(d) < 0.5f) {
        v = to;
        return;
      }
      v += d * std::min(1.f, dt * 16.f);
      moving = true;
    };
    aim();  // where the selection is now: a press moves the highlight on that frame, not the next
    glide(scroll_, want_);
    if (hx_ >= 0) glide(hx_, tx_), glide(hy_, ty_);
    return moving;
  }
  // the highlight's and the scroll's targets for the selection, from the last drawn layout
  void aim() {
    if (cw_ <= 0) return;
    tx_ = pad_ + (sel_ % kCols) * (cw_ + gap_), ty_ = pad_ + (sel_ / kCols) * (ch_ + gap_);
    float top = ty_, bottom = top + ch_;
    if (top - want_ < pad_) want_ = top - pad_;
    if (bottom - want_ > h_ - pad_) want_ = bottom - h_ + pad_;
    want_ = std::max(0.f, want_);
  }
  void draw(Env& e, float w, float h) override {
    e.c.fill({0, 0, w, h}, e.p.shellBg);
    int n = int(photos_->size());
    if (!n) {
      Label l = label(e, "No photos here", 14, false, w);
      drawLabel(e, l, (w - l.w) / 2, (h - l.h) / 2, e.p.muted);
      return;
    }
    float pad = float(e.dp(6)), gap = float(e.dp(4));
    float cw = (w - 2 * pad - gap * (kCols - 1)) / kCols, ch = cw * 0.75f;
    int tw = std::min(200, int(cw)), th = std::min(150, int(ch));
    pad_ = pad, gap_ = gap, cw_ = cw, ch_ = ch, h_ = h;
    aim();  // the selected row in view
    int first = std::max(0, int((scroll_ - pad) / (ch + gap)) * kCols), last = std::min(n, first + kCols * (int(h / (ch + gap)) + 2));
    for (int i = first; i < last; i++) {
      float x = pad + (i % kCols) * (cw + gap), y = pad + (i / kCols) * (ch + gap) - scroll_;
      ui::Rect r{x, y, cw, ch};
      e.c.layer(1);
      e.c.shape(r, e.dpf(4), e.p.rowLine);
      float a = 1;
      if (const ui::Texture* t = e.covers.get((*photos_)[size_t(i)].path, tw, th, 0, e.now, &a)) {
        e.c.layer(2);
        e.c.image(*t, r, {0, 0, 1, 1}, e.dpf(4), withAlpha(white, a));
      }
    }
    // the highlight, where it glides (in the grid's own coordinates: it scrolls with the rows)
    if (hx_ < 0) hx_ = tx_, hy_ = ty_;
    {
      ui::Rect r{hx_, hy_ - scroll_, cw, ch};
      e.c.layer(3);
      float b = e.dpf(3);
      e.c.shape({r.x - b, r.y - b, r.w + 2 * b, b}, 0, e.p.accent);
      e.c.shape({r.x - b, r.y + r.h, r.w + 2 * b, b}, 0, e.p.accent);
      e.c.shape({r.x - b, r.y, b, r.h}, 0, e.p.accent);
      e.c.shape({r.x + r.w, r.y, b, r.h}, 0, e.p.accent);
    }
  }

 private:
  static constexpr int kCols = 4;
  std::string title_;
  std::shared_ptr<std::vector<Photo>> photos_;
  int sel_ = 0;
  float scroll_ = 0, want_ = 0;
  float hx_ = -1, hy_ = 0, tx_ = 0, ty_ = 0;  // the highlight and where it goes
  float pad_ = 0, gap_ = 0, cw_ = 0, ch_ = 0, h_ = 0;  // the layout last drawn
};

// ---- the folders ----

class PhotoFoldersScreen : public ShellListScreen {
 public:
  PhotoFoldersScreen() : ShellListScreen("Photos") {
    list_.setMessage("Looking for photos\xe2\x80\xa6");
    auto alive = alive_;
    std::thread([this, alive] {
      auto all = std::make_shared<std::vector<Photo>>();
      for (const char* r : {"/storage/sdcard0", "/storage/sdcard1"}) walk(r, 0, *all);
      postToMain([this, alive, all] {
        if (*alive) show(all);
      });
    }).detach();
  }
  ~PhotoFoldersScreen() override { *alive_ = false; }

 private:
  void show(std::shared_ptr<std::vector<Photo>> all) {
    if (all->empty()) return list_.setMessage("No photos on the card.\nCopy JPEG or PNG pictures to it.");
    // the newest first, everywhere
    std::sort(all->begin(), all->end(), [](const Photo& a, const Photo& b) { return a.modified > b.modified; });
    std::map<std::string, std::shared_ptr<std::vector<Photo>>> byFolder;
    for (const Photo& p : *all) {
      auto& v = byFolder[p.path.substr(0, p.path.rfind('/'))];
      if (!v) v = std::make_shared<std::vector<Photo>>();
      v->push_back(p);
    }
    std::vector<Row> rows;
    auto add = [&](const std::string& title, const std::string& sub, std::shared_ptr<std::vector<Photo>> v) {
      Row r;
      r.title = title, r.subtitle = sub;
      r.trailing = std::to_string(v->size());
      r.thumb = v->front().path;
      r.chevron = true;
      r.onSelect = [title, v] { g_app->push(std::unique_ptr<Screen>(new PhotoGridScreen(title, v))); };
      rows.push_back(std::move(r));
    };
    add("All photos", "The newest first", all);
    std::vector<std::pair<std::string, std::shared_ptr<std::vector<Photo>>>> folders(byFolder.begin(), byFolder.end());
    std::sort(folders.begin(), folders.end(), [](const auto& a, const auto& b) { return a.second->front().modified > b.second->front().modified; });
    for (auto& f : folders) add(folderName(f.first), dateOf(f.second->front().modified), f.second);
    list_.setRows(std::move(rows), true);
  }
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

const Tile& photoTile() { return tiles::photos; }
void findPhotos(std::vector<std::pair<std::string, int64_t>>& out) {
  std::vector<Photo> all;
  for (const char* r : {"/storage/sdcard0", "/storage/sdcard1"}) walk(r, 0, all);
  std::sort(all.begin(), all.end(), [](const Photo& a, const Photo& b) { return a.modified > b.modified; });
  for (Photo& p : all) out.push_back({std::move(p.path), p.modified});
}
std::unique_ptr<Screen> makePhotos() { return std::unique_ptr<Screen>(new PhotoFoldersScreen()); }
std::unique_ptr<Screen> makePhotoViewer(const std::vector<std::string>& paths, int at) {
  auto photos = std::make_shared<std::vector<Photo>>();
  for (const std::string& p : paths) {
    struct stat st;
    photos->push_back({p, stat(p.c_str(), &st) == 0 ? int64_t(st.st_mtime) : 0});
  }
  if (photos->empty()) photos->push_back({"", 0});
  return std::unique_ptr<Screen>(new PhotoViewerScreen(photos, std::max(0, std::min(at, int(photos->size()) - 1))));
}

}  // namespace rondo::shell
