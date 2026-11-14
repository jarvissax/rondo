// Library › Organize the card (and the dashboard's Organize): the card's songs and videos into the
// folders Jellyfin expects (core/organize.h), shown first and moved only when asked. The moves run on
// a thread of their own; once they're done, on the UI thread, the library's entries (their plays and
// ratings), the playlists and the queue follow the files.
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../core/organize.h"
#include "app.h"
#include "fill_in.h"
#include "main_queue.h"
#include "screens.h"

namespace rondo::shell {

namespace {

const char* const kRoots[] = {"/storage/sdcard0", "/storage/sdcard1"};

struct Job {
  std::mutex mu;
  bool running = false, finished = false;
  int done = 0, total = 0;
  OrganizeResult result;
};
Job g_job;

}  // namespace

std::vector<OrganizeItem> organizeItems(const std::string& root) {
  std::vector<OrganizeItem> items;
  for (const Track& t : g_library.tracks()) {
    if (t.gone || t.path.compare(0, root.size() + 1, root + "/") != 0) continue;
    OrganizeItem i;
    i.path = t.path, i.title = t.title, i.artist = t.artist, i.albumArtist = t.albumArtist, i.album = t.album;
    i.track = t.track, i.disc = t.disc, i.year = t.year, i.video = t.video;
    items.push_back(std::move(i));
  }
  return items;
}

std::vector<std::string> organizeRoots() {
  std::vector<std::string> roots;
  for (const char* root : kRoots) {
    struct stat st;
    if (stat(root, &st) == 0 && S_ISDIR(st.st_mode)) roots.push_back(root);
  }
  return roots;
}

// Files or folders that moved on the card (organized, renamed or moved on the dashboard): the library's
// entries (their plays and ratings), the playlists and the queue follow them. A folder takes everything
// in it along. UI thread.
void filesMoved(const std::vector<std::pair<std::string, std::string>>& moves) {
  std::unordered_map<std::string, std::string> moved;  // track paths: from -> to
  for (const auto& mv : moves) {
    const std::string &from = mv.first, &to = mv.second;
    for (const Track& t : g_library.tracks()) {
      if (t.gone) continue;
      if (t.path == from) moved[t.path] = to;
      else if (t.path.compare(0, from.size() + 1, from + "/") == 0) moved[t.path] = to + t.path.substr(from.size());
    }
  }
  for (const auto& kv : moved) g_library.moved(kv.first, kv.second);
  for (const Playlist& p : g_playlists.all()) {
    std::vector<std::string> paths = p.paths;
    bool changed = false;
    for (std::string& s : paths)
      if (auto it = moved.find(s); it != moved.end()) s = it->second, changed = true;
    if (changed) g_playlists.update(p.name, nullptr, &paths);
  }
  std::vector<std::string> queue = g_player.queue();
  bool changed = false;
  for (std::string& s : queue)
    if (auto it = moved.find(s); it != moved.end()) s = it->second, changed = true;
  if (changed) {
    PlayerState s = g_player.state();
    bool shuffle = s.shuffle;
    g_player.restoreQueue(queue, s.index, s.positionMs, s.playing != 0);
    if (shuffle) g_player.setShuffle(true);
  }
  fprintf(stderr, "rondo: %zu files of the library moved\n", moved.size());
}

namespace {
void follow(const std::vector<OrganizeMove>& done) {
  std::vector<std::pair<std::string, std::string>> moves;
  for (const OrganizeMove& m : done)
    if (m.kind == OrganizeMove::Song || m.kind == OrganizeMove::Video) moves.push_back({m.from, m.to});
  filesMoved(moves);
}
}  // namespace

std::vector<std::pair<std::string, OrganizePlan>> organizePlans(const OrganizeNames& names) {
  std::vector<std::pair<std::string, OrganizePlan>> plans;
  for (const std::string& root : organizeRoots()) {
    std::vector<OrganizeItem> items = organizeItems(root);
    if (!items.empty()) plans.push_back({root, planOrganize(items, root, names)});
  }
  return plans;
}

// "" when started, else why not
std::string organizeStart(const OrganizeNames& names) {
  if (g_library.scanning()) return "The card is being read: try again in a moment";
  if (fillInWriting()) return "The dashboard's Fill in is saving into the songs: try again when it's done";
  std::vector<std::pair<std::string, OrganizePlan>> plans = organizePlans(names);
  int total = 0;
  for (auto& p : plans) total += int(p.second.moves.size());
  {
    std::lock_guard<std::mutex> l(g_job.mu);
    if (g_job.running) return "Organizing already";
    if (!total) return "Everything is where it belongs";
    g_job.running = true, g_job.finished = false, g_job.done = 0, g_job.total = total, g_job.result = OrganizeResult();
  }
  auto work = new std::vector<std::pair<std::string, OrganizePlan>>(std::move(plans));
  pthread_t t;
  pthread_create(&t, nullptr, [](void* p) -> void* {
    std::unique_ptr<std::vector<std::pair<std::string, OrganizePlan>>> plans(static_cast<std::vector<std::pair<std::string, OrganizePlan>>*>(p));
    OrganizeResult all;
    int before = 0;
    for (auto& kv : *plans) {
      OrganizeResult r = applyOrganize(kv.second, kv.first, [before](int done, int) {
        std::lock_guard<std::mutex> l(g_job.mu);
        g_job.done = before + done;
      });
      before += int(kv.second.moves.size());
      all.done.insert(all.done.end(), r.done.begin(), r.done.end());
      all.errors.insert(all.errors.end(), r.errors.begin(), r.errors.end());
    }
    auto done = std::make_shared<std::vector<OrganizeMove>>(all.done);
    postToMain([done] { follow(*done); });
    std::lock_guard<std::mutex> l(g_job.mu);
    g_job.result = std::move(all);
    g_job.running = false, g_job.finished = true;
    return nullptr;
  }, work);
  pthread_detach(t);
  return "";
}

OrganizeStatus organizeStatus() {
  std::lock_guard<std::mutex> l(g_job.mu);
  OrganizeStatus s;
  s.running = g_job.running, s.finished = g_job.finished, s.done = g_job.done, s.total = g_job.total;
  s.moved = int(g_job.result.done.size());
  s.errors = g_job.result.errors;
  return s;
}

namespace {

std::string shortPath(const std::string& p) {
  for (const char* r : kRoots) {
    std::string root = r;
    if (p.compare(0, root.size() + 1, root + "/") == 0) return p.substr(root.size() + 1);
  }
  return p;
}

// the moves themselves, file by file
class MovesScreen : public ShellListScreen {
 public:
  explicit MovesScreen(std::vector<OrganizeMove> moves) : ShellListScreen("The moves") {
    std::vector<Row> rows;
    for (const OrganizeMove& m : moves) {
      Row r;
      std::string to = shortPath(m.to);
      r.title = to.substr(to.rfind('/') + 1);
      r.subtitle = to.substr(0, to.rfind('/'));  // a row to scroll past (nothing happens on center)
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows));
  }
};

class OrganizingScreen : public ShellListScreen {
 public:
  // pops: the screens Done closes (this one, and the summary when it came from there)
  explicit OrganizingScreen(int pops = 2) : ShellListScreen("Organizing"), pops_(pops) { render(); }
  bool onAction(Action a, int steps) override {
    if (organizeStatus().running) return true;  // nothing until the moves are done
    return ShellListScreen::onAction(a, steps);
  }
  bool tick(int64_t now) override {
    OrganizeStatus s = organizeStatus();
    if (s.done == shownDone_ && s.running == shownRunning_) return false;
    render();
    return true;
  }

 private:
  void render() {
    OrganizeStatus s = organizeStatus();
    shownDone_ = s.done, shownRunning_ = s.running;
    if (s.running) {
      list_.setMessage("Moving " + std::to_string(s.done) + " of " + std::to_string(s.total) + "\xe2\x80\xa6\nKeep Rondo on.");
      return;
    }
    std::vector<Row> rows;
    Row h;
    h.header = true;
    h.title = std::to_string(s.moved) + " files moved" + (s.errors.empty() ? "" : ", " + std::to_string(s.errors.size()) + " couldn't be");
    rows.push_back(h);
    for (size_t i = 0; i < s.errors.size() && i < 30; i++) {
      Row r;
      r.title = s.errors[i];
      r.enabled = false;
      rows.push_back(r);
    }
    Row ok;
    ok.title = "Done";
    int pops = pops_;
    ok.onSelect = [pops] {
      for (int i = 0; i < pops; i++) g_app->pop();
    };
    rows.push_back(ok);
    list_.setRows(std::move(rows));
  }
  int pops_;
  int shownDone_ = -1;
  bool shownRunning_ = true;
};

class OrganizeConfirmScreen : public ShellListScreen {
 public:
  explicit OrganizeConfirmScreen(int files) : ShellListScreen("Organize the card") {
    Row q;
    q.header = true;
    q.title = "Move " + std::to_string(files) + " files?";
    Row cancel;
    cancel.title = "Cancel";
    cancel.onSelect = [] { g_app->pop(); };
    Row go;
    go.title = "Organize";
    go.subtitle = "Playlists, plays and ratings go along";
    go.tile = &tiles::organize;
    go.onSelect = [] {
      std::string why = organizeStart();
      g_app->pop();
      if (!why.empty()) return g_app->toast(why);
      g_app->push(std::unique_ptr<Screen>(new OrganizingScreen()));
    };
    list_.setRows({q, cancel, go});
  }
};

class OrganizeScreen : public ShellListScreen {
 public:
  OrganizeScreen() : ShellListScreen("Organize the card") { render(); }
  // worked out again only when the library changed (back from the moves' list it hasn't)
  void onShow() override {
    if (generation_ != g_library.generation()) render();
  }

 private:
  void render() {
    generation_ = g_library.generation();
    std::vector<std::pair<std::string, OrganizePlan>> plans = organizePlans();
    int songs = 0, videos = 0, sidecars = 0, covers = 0, there = 0;
    std::vector<OrganizeMove> moves;
    for (auto& p : plans) {
      songs += p.second.songs, videos += p.second.videos, sidecars += p.second.sidecars, covers += p.second.covers;
      there += p.second.alreadyThere;
      moves.insert(moves.end(), p.second.moves.begin(), p.second.moves.end());
    }
    std::vector<Row> rows;
    auto info = [&](const std::string& title, const std::string& sub, const std::string& trailing) {
      Row r;
      r.title = title, r.subtitle = sub, r.trailing = trailing, r.enabled = false;
      rows.push_back(std::move(r));
    };
    Row h;
    h.header = true;
    h.title = moves.empty() ? "Everything is where it belongs" : "Folders as Jellyfin expects them";
    rows.push_back(h);
    info("Songs", "Music/Artist/Album (Year)/01 - Title", std::to_string(songs));
    info("Videos", "Videos/Shows/Series (Years)/Season 01 or Videos/Movies", std::to_string(videos));
    if (sidecars) info("Lyrics and subtitles", "With their songs and videos", std::to_string(sidecars));
    if (covers) info("Folder pictures", "With their albums", std::to_string(covers));
    if (there) info("Already in place", "", std::to_string(there));
    if (!moves.empty()) {
      Row see;
      see.title = "See the moves";
      see.chevron = true;
      see.onSelect = [moves] { g_app->push(std::unique_ptr<Screen>(new MovesScreen(moves))); };
      rows.push_back(see);
      Row go;
      go.title = "Organize";
      go.subtitle = "Moves the files on the card";
      go.tile = &tiles::organize;
      go.chevron = true;
      int n = int(moves.size());
      go.onSelect = [n] { g_app->push(std::unique_ptr<Screen>(new OrganizeConfirmScreen(n))); };
      rows.push_back(go);
    }
    list_.setRows(std::move(rows), true);
  }
  uint64_t generation_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeOrganize() {
  if (organizeStatus().running) return std::unique_ptr<Screen>(new OrganizingScreen(1));
  return std::unique_ptr<Screen>(new OrganizeScreen());
}

}  // namespace rondo::shell
