// Library › Files: the card and the internal storage as they are, every folder and file. Center opens
// (a folder, a song with the folder's songs after it, a video, a photo with the folder's others), hold
// center for what can be done with it: rename, copy or move to another folder (on either storage),
// delete. Each folder starts with New folder. The work is file_ops.h's, shared with the dashboard.
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <memory>

#include "../core/fold.h"
#include "../ui/image.h"
#include "app.h"
#include "file_ops.h"
#include "screens.h"

namespace rondo::shell {

namespace {

std::string sizeText(int64_t n) {
  char b[32];
  if (n < 1000) snprintf(b, sizeof(b), "%d B", int(n));
  else if (n < 1000 * 1000) snprintf(b, sizeof(b), "%.0f KB", n / 1e3);
  else if (n < int64_t(1000) * 1000 * 1000) snprintf(b, sizeof(b), "%.1f MB", n / 1e6);
  else snprintf(b, sizeof(b), "%.1f GB", n / 1e9);
  return b;
}
std::string dayText(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  char b[32];
  strftime(b, sizeof(b), "%b %e, %Y", &tm);
  return b;
}
std::string extOf(const std::string& name) {
  size_t dot = name.rfind('.');
  std::string e = dot == std::string::npos ? "" : name.substr(dot + 1);
  for (char& c : e) c = char(tolower((unsigned char)c));
  return e;
}
bool isVideoName(const std::string& n) {
  std::string x = extOf(n);
  return x == "mp4" || x == "m4v" || x == "mkv" || x == "3gp" || x == "webm";
}
std::string nameOf(const std::string& p) { return p.substr(p.rfind('/') + 1); }
std::string parentOf(const std::string& p) { return p.substr(0, p.rfind('/')); }
std::string kindText(const std::string& name) {
  if (isAudioExtension(extOf(name))) return "Song";
  if (isVideoName(name)) return "Video";
  if (ui::isPhotoName(name)) return "Photo";
  std::string x = extOf(name);
  if (x == "lrc") return "Lyrics";
  if (x == "srt" || x == "ass" || x == "vtt") return "Subtitles";
  if (x == "m3u" || x == "m3u8") return "Playlist file";
  return x.empty() ? "File" : x + " file";
}

struct Entry {
  std::string name, path;
  bool dir = false;
  int64_t size = 0;
  time_t modified = 0;
};
std::vector<Entry> listFolder(const std::string& dir) {
  std::vector<Entry> out;
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;  // hidden, and an upload's .part
    Entry x;
    x.name = e->d_name, x.path = dir + "/" + e->d_name;
    struct stat st;
    if (stat(x.path.c_str(), &st) != 0) continue;
    x.dir = S_ISDIR(st.st_mode), x.size = int64_t(st.st_size), x.modified = st.st_mtime;
    out.push_back(std::move(x));
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    if (a.dir != b.dir) return a.dir;
    return fold(a.name) < fold(b.name);
  });
  return out;
}

// Opening a file: a song plays with the folder's songs after it, a photo shows with the folder's others
void openFile(const std::string& path) {
  std::string dir = parentOf(path), name = nameOf(path);
  if (isAudioExtension(extOf(name))) {
    std::vector<std::string> songs;
    int at = 0;
    for (const Entry& e : listFolder(dir)) {
      if (e.dir || !isAudioExtension(extOf(e.name))) continue;
      if (e.path == path) at = int(songs.size());
      songs.push_back(e.path);
    }
    g_player.playQueue(songs, at);
    openNowPlaying();
  } else if (isVideoName(name)) {
    Item i;
    i.kind = "video";
    i.id = path, i.thumb = path;
    const Track* t = g_library.find(path);
    i.title = t ? t->title : name;
    i.durationSec = t ? t->durationSec : -1;
    openVideo(i);
  } else if (ui::isPhotoName(name)) {
    std::vector<std::string> photos;
    int at = 0;
    for (const Entry& e : listFolder(dir)) {
      if (e.dir || !ui::isPhotoName(e.name)) continue;
      if (e.path == path) at = int(photos.size());
      photos.push_back(e.path);
    }
    g_app->push(makePhotoViewer(photos, at));
  } else {
    g_app->toast("Rondo can't open " + kindText(name) + "s");
  }
}
bool canOpen(const std::string& name, bool dir) {
  return dir || isAudioExtension(extOf(name)) || isVideoName(name) || ui::isPhotoName(name);
}
void report(const files::Result& r, const char* verb) {
  if (!r.errors.empty()) g_app->toast(r.errors.front() + (r.errors.size() > 1 ? " (and " + std::to_string(r.errors.size() - 1) + " more)" : ""));
  else if (r.done) g_app->toast(std::string(verb) + " " + std::to_string(r.done) + (r.done == 1 ? " item" : " items"));
}

// Copy to…, Move to…: the folders of both storages, walked here (center goes in, Menu goes back up);
// the first row puts them in the folder shown
class PickFolderScreen : public ShellListScreen {
 public:
  PickFolderScreen(std::vector<std::string> paths, bool copy, std::string start)
      : ShellListScreen(copy ? "Copy to" : "Move to"), paths_(std::move(paths)), copy_(copy), dir_(std::move(start)) {
    render();
  }
  bool onAction(Action a, int steps) override {
    if (a == Action::Back && !dir_.empty()) {  // up a folder, then the storages, then out
      std::string up = files::isRoot(dir_) ? "" : parentOf(dir_);
      from_ = dir_, dir_ = up;
      render();
      return true;
    }
    return ShellListScreen::onAction(a, steps);
  }

 private:
  void render() {
    std::vector<Row> rows;
    if (dir_.empty()) {
      for (const char* r : files::kRoots) {
        if (!files::isDir(r)) continue;
        Row row;
        row.title = files::rootName(r);
        row.subtitle = sizeText(files::freeBytes(r)) + " free";
        row.tile = std::string(r) == files::kRoots[0] ? &tiles::files : &tiles::system;
        row.chevron = true;
        std::string p = r;
        row.onSelect = [this, p] { go(p); };
        rows.push_back(std::move(row));
      }
    } else {
      Row here;
      here.title = std::string(copy_ ? "Copy here" : "Move here");
      here.subtitle = files::isRoot(dir_) ? files::rootName(dir_) : nameOf(dir_);
      here.tile = copy_ ? &tiles::copyTo : &tiles::moveTo;
      here.onSelect = [this] { put(); };
      rows.push_back(std::move(here));
      Row head;
      head.header = true;
      head.title = "Folders";
      rows.push_back(head);
      for (const Entry& e : listFolder(dir_)) {
        if (!e.dir) continue;
        Row row;
        row.title = e.name;
        row.tile = &tiles::folders;
        row.chevron = true;
        // a folder being moved can't hold itself
        row.enabled = std::find(paths_.begin(), paths_.end(), e.path) == paths_.end();
        std::string p = e.path;
        row.onSelect = [this, p] { go(p); };
        rows.push_back(std::move(row));
      }
    }
    list_.setRows(std::move(rows), false);
    // back up a level: the folder come from stays selected
    for (size_t k = 0; k < list_.rows().size() && !from_.empty(); k++)
      if (list_.rows()[k].title == nameOf(from_) || list_.rows()[k].title == files::rootName(from_)) list_.selectIndex(int(k));
    from_.clear();
  }
  void go(const std::string& p) {
    dir_ = p;
    render();
  }
  void put() {
    std::string to = dir_;
    bool copy = copy_;
    bool started = files::transfer(paths_, to, copy, [copy](const files::Result& r) { report(r, copy ? "Copied" : "Moved"); });
    if (!started) return g_app->toast("Wait for the copy under way to finish");
    if (!files::transferStatus().empty()) g_app->toast(copy ? "Copying in the background" : "Moving in the background");
    g_app->pop();  // this
    g_app->pop();  // the item's menu
  }
  std::vector<std::string> paths_;
  bool copy_;
  std::string dir_, from_;
};

// Hold center on a file or folder: what can be done with it
class FileMenuScreen : public ShellListScreen {
 public:
  explicit FileMenuScreen(std::string path) : ShellListScreen(nameOf(path)), path_(std::move(path)) { render(); }

 private:
  void render() {
    bool dir = files::isDir(path_);
    std::string name = nameOf(path_);
    std::vector<Row> rows;
    Row head;
    head.header = true;
    struct stat st;
    head.title = dir ? "Folder" : kindText(name) + (stat(path_.c_str(), &st) == 0 ? " \xc2\xb7 " + sizeText(st.st_size) : "");
    rows.push_back(head);
    auto add = [&](const char* title, const Tile& tile, bool chevron, std::function<void()> on) {
      Row r;
      r.title = title, r.tile = &tile, r.chevron = chevron, r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    std::string p = path_;
    if (canOpen(name, dir)) add("Open", tiles::openFile, true, [p, dir] {
        g_app->pop();
        if (dir) g_app->push(makeFiles(p));
        else openFile(p);
      });
    add("Rename", tiles::renameTo, true, [p, name] {
      g_app->push(makeTextEntry("Rename " + name, name, "Rename", 120, [p](const std::string& typed) { return files::renameEntry(p, typed); },
                                [](const std::string&) { g_app->pop(); }));
    });
    add("Copy to\xe2\x80\xa6", tiles::copyTo, true, [p] { g_app->push(std::unique_ptr<Screen>(new PickFolderScreen({p}, true, parentOf(p)))); });
    add("Move to\xe2\x80\xa6", tiles::moveTo, true, [p] { g_app->push(std::unique_ptr<Screen>(new PickFolderScreen({p}, false, parentOf(p)))); });
    add("Delete", tiles::del, true, [p, name, dir] { g_app->push(confirmDelete(p, name, dir)); });
    list_.setRows(std::move(rows), true);
  }
  // Are you sure? Cancel first, so a stray center press keeps it
  static std::unique_ptr<Screen> confirmDelete(const std::string& p, const std::string& name, bool dir) {
    struct Confirm : ShellListScreen {
      Confirm(const std::string& p, const std::string& name, bool dir) : ShellListScreen(dir ? "Delete this folder?" : "Delete this file?") {
        Row q;
        q.header = true;
        q.title = "\xe2\x80\x9c" + name + "\xe2\x80\x9d" + (dir ? " and all it holds" : "") + " can't be brought back";
        Row cancel;
        cancel.title = "Cancel";
        cancel.onSelect = [] { g_app->pop(); };
        Row del;
        del.title = "Delete";
        del.tile = &tiles::del;
        del.onSelect = [p] {
          files::Result r = files::remove({p});
          g_app->pop();  // this
          g_app->pop();  // the menu
          report(r, "Deleted");
        };
        list_.setRows({q, cancel, del});
      }
    };
    return std::unique_ptr<Screen>(new Confirm(p, name, dir));
  }
  std::string path_;
};

// A folder, or (no path) the two storages
class FilesScreen : public ShellListScreen {
 public:
  explicit FilesScreen(std::string dir)
      : ShellListScreen(dir.empty() ? "Files" : files::isRoot(dir) ? files::rootName(dir) : nameOf(dir)), dir_(std::move(dir)) {
    render();
  }
  void onShow() override { render(); }  // back from a rename, a delete, a copy
  bool tick(int64_t) override {
    std::string s = files::transferStatus();
    if (s == status_) return false;
    render();  // the progress, and when it ends what came
    return true;
  }
  int64_t wakeAt() const override { return status_.empty() ? 0 : now_ns() + 500 * kMs; }

 private:
  void render() {
    status_ = files::transferStatus();
    std::vector<Row> rows;
    if (!status_.empty()) {
      Row busy;
      busy.header = true;
      busy.title = status_;
      rows.push_back(busy);
    }
    if (dir_.empty()) {
      for (const char* r : files::kRoots) {
        if (!files::isDir(r)) continue;
        Row row;
        row.title = files::rootName(r);
        int64_t free = files::freeBytes(r), total = files::totalBytes(r);
        row.subtitle = sizeText(free) + " free of " + sizeText(total);
        row.tile = std::string(r) == files::kRoots[0] ? &tiles::files : &tiles::system;
        row.chevron = true;
        std::string p = r;
        row.onSelect = [p] { g_app->push(makeFiles(p)); };
        rows.push_back(std::move(row));
      }
      if (rows.empty()) return list_.setMessage("No card or internal storage.\nIs the USB drive on?");
      list_.setRows(std::move(rows), true);
      return;
    }
    Row add;
    add.title = "New folder";
    add.tile = &tiles::newFolder;
    std::string dir = dir_;
    add.onSelect = [dir] {
      g_app->push(makeTextEntry("New folder", "", "Make", 120, [dir](const std::string& typed) { return files::makeFolder(dir, typed); },
                                [](const std::string&) {}));
    };
    rows.push_back(std::move(add));
    std::vector<Entry> entries = listFolder(dir_);
    Row head;
    head.header = true;
    head.title = entries.empty() ? "An empty folder"
                                 : std::to_string(entries.size()) + (entries.size() == 1 ? " item" : " items") + " \xc2\xb7 hold center for more";
    rows.push_back(head);
    for (const Entry& e : entries) {
      Row r;
      r.title = e.name;
      std::string p = e.path;
      if (e.dir) {
        r.tile = &tiles::folders;
        r.chevron = true;
        r.subtitle = dayText(e.modified);
        r.onSelect = [p] { g_app->push(makeFiles(p)); };
      } else {
        bool media = isAudioExtension(extOf(e.name)) || isVideoName(e.name) || ui::isPhotoName(e.name);
        if (media) r.thumb = p;  // its cover, a frame, the picture
        else r.tile = &tiles::files;
        r.subtitle = kindText(e.name) + " \xc2\xb7 " + sizeText(e.size);
        if (const Track* t = g_library.find(p); t && !t->title.empty() && t->title != e.name) r.subtitle = t->title + " \xc2\xb7 " + sizeText(e.size);
        r.onSelect = [p] { openFile(p); };
      }
      r.onLongSelect = [p] { g_app->push(std::unique_ptr<Screen>(new FileMenuScreen(p))); };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows), true);
  }
  std::string dir_, status_;
};

}  // namespace

std::unique_ptr<Screen> makeFiles(const std::string& dir) { return std::unique_ptr<Screen>(new FilesScreen(dir)); }

}  // namespace rondo::shell
