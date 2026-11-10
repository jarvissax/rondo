// Settings › Software update (SoftwareUpdateScreen.kt in 1.0.x), ported 1:1: a newer Rondo over Wi-Fi,
// without flashing the firmware again. The server says which release is newest and passes its bundle
// through from GitHub (the Y1 talks only to its server); core/bundle.h checks its signature and puts it
// in place, and Rondo restarts by itself with everything kept. A release that needs a newer firmware
// installs only with Innioasis Updater CE.
#include <thread>

#include <yyjson.h>

#include "../core/bundle.h"
#include "app.h"
#include "main_queue.h"
#include "screens.h"

namespace rondo::shell {
namespace {

struct UpdateInfo {
  std::string version;
  int64_t size = 0;
  int firmware = 1;
};

bool parseUpdate(const std::string& json, UpdateInfo& out) {
  yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
  if (!doc) return false;
  yyjson_val* r = yyjson_doc_get_root(doc);
  yyjson_val* v = yyjson_obj_get(r, "version");
  out.version = yyjson_is_str(v) ? yyjson_get_str(v) : "";
  out.size = yyjson_get_sint(yyjson_obj_get(r, "size"));
  yyjson_val* f = yyjson_obj_get(r, "firmware");
  out.firmware = yyjson_is_num(f) ? int(yyjson_get_num(f)) : 1;
  yyjson_doc_free(doc);
  return true;
}

// what installs run with: the firmware's Rondo (/system/rondo and /data/rondo), not a development run
}  // namespace
bool updatesAvailable() { return !g_development && firmwareLevel() >= kFirmwareNeeded; }
namespace {

std::string dataDir() { return g_prefsPath.substr(0, g_prefsPath.rfind('/')); }

class SoftwareUpdateScreen : public ShellListScreen {
 public:
  SoftwareUpdateScreen() : ShellListScreen("Software update") { check(); }
  ~SoftwareUpdateScreen() override { *alive_ = false; }

  bool tick(int64_t) override {
    if (state_ == kDownloading) {
      CoreState c = g_core.state();
      if (c.downloadState == 2) {
        state_ = kInstalling;
        render();
        startInstall();
        return true;
      }
      if (c.downloadState == 3) {
        state_ = kFailed;
        why_ = "The download didn't finish: " + (c.downloadStatus ? "HTTP " + std::to_string(c.downloadStatus) : std::string("no connection"));
        render();
        return true;
      }
      int64_t total = c.downloadTotal > 0 ? c.downloadTotal : info_.size;
      int percent = total > 0 ? int(c.downloadDone * 100 / total) : 0;
      if (percent != percent_) {
        percent_ = percent;
        render();
        return true;
      }
      return false;
    }
    if (state_ == kInstalling && installDone_) {
      installDone_ = false;
      if (!installOk_) {
        state_ = kFailed;
        why_ = installWhy_;
        render();
        return true;
      }
      // Rondo ends; init starts it again, and /system/rondo/start runs the new one
      printf("rondo: installed %s: restarting\n", installed_.c_str());
      resumeSaveNow();
      requestRestart();
      return true;
    }
    return false;
  }
  int64_t wakeAt() const override { return state_ == kDownloading || state_ == kInstalling ? now_ns() + 250 * kMs : 0; }

 private:
  enum State { kChecking, kNoServer, kFailed, kUpToDate, kAvailable, kNeedsFirmware, kDownloading, kInstalling };

  void render() {
    std::string now = std::string("Rondo ") + kVersion;
    std::vector<Row> rows;
    auto add = [&](const std::string& title, const std::string& subtitle, const Tile* tile, bool enabled,
                   std::function<void()> on, const std::string& trailing = "") {
      Row r;
      r.title = title, r.subtitle = subtitle, r.tile = tile, r.enabled = enabled, r.trailing = trailing;
      r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    auto header = [&](const char* t) {
      Row h;
      h.header = true;
      h.title = t;
      rows.push_back(h);
    };
    // a Rondo put on past these checks (over adb) on a firmware older than it needs
    if (kFirmwareNeeded > firmwareLevel())
      add("This Rondo needs a newer firmware", "Install Rondo with Innioasis Updater CE", &tiles::update, true, nullptr);
    switch (state_) {
      case kChecking: add("Checking for updates\xe2\x80\xa6", now, &tiles::update, false, nullptr); break;
      case kNoServer:
        add(now, "This Rondo has no server to check with", &tiles::update, true, nullptr);
        header("Other ways to update");
        add("From the dashboard", "Settings \xe2\x80\xba Dashboard, then the Overview page", nullptr, true, nullptr);
        add("Over USB", "jarvissax.github.io/rondo in Chrome or Edge", nullptr, true, nullptr);
        break;
      case kFailed:
        add("Couldn't check", why_, &tiles::update, true, nullptr);
        add("Try again", "", nullptr, true, [this] { check(); });
        break;
      case kUpToDate:
        add(now, "This is the newest Rondo", &tiles::update, true, nullptr, "Up to date");
        add("Check again", "", nullptr, true, [this] { check(); });
        break;
      case kNeedsFirmware:
        add("Rondo " + info_.version + " is out", std::string("It needs a new firmware \xc2\xb7 you have ") + kVersion, &tiles::update, true, nullptr);
        header("Install it with Innioasis Updater CE");
        add("On a computer", "Choose Y1, Type A, then Rondo", nullptr, true, nullptr);
        add("Your settings", "Choose Keep Data, if the Updater offers it, to keep them", nullptr, true, nullptr);
        break;
      case kAvailable:
        add("Rondo " + info_.version + " is out", std::string("You have ") + kVersion + " \xc2\xb7 " + bytes(uint64_t(std::max<int64_t>(0, info_.size))),
            &tiles::update, true, nullptr);
        add("Install now", "Rondo restarts by itself; settings, Wi-Fi and music stay", nullptr, true, [this] { install(); });
        break;
      case kDownloading:
        add("Downloading\xe2\x80\xa6 " + std::to_string(percent_) + "%", "Keep the Y1 on Wi-Fi", &tiles::update, false, nullptr);
        break;
      case kInstalling:
        add("Installing\xe2\x80\xa6", "Rondo restarts by itself in a few seconds", &tiles::update, false, nullptr);
        break;
    }
    list_.setRows(std::move(rows), true);
  }

  void check() {
    if (!directMode() && g_config.server.url.empty()) {
      state_ = kNoServer;
      return render();
    }
    state_ = kChecking;
    render();
    auto alive = alive_;
    g_core.get("/update?firmware=" + std::to_string(firmwareLevel()), 0, [this, alive](int status, const std::string& body) {
      if (!*alive) return;
      UpdateInfo info;
      if (status != 200 || !parseUpdate(body, info)) {
        state_ = kFailed;
        why_ = g_app->status().online ? "The server didn't answer" : "No Wi-Fi connection";
      } else if (!isNewerVersion(info.version, kVersion)) {
        state_ = kUpToDate;
      } else if (info.firmware > firmwareLevel()) {
        info_ = info, state_ = kNeedsFirmware;
      } else {
        info_ = info, state_ = kAvailable;
      }
      render();
    });
  }

  void install() {
    if (!updatesAvailable()) {
      state_ = kFailed;
      why_ = "Updates need the Rondo firmware";
      return render();
    }
    state_ = kDownloading;
    percent_ = 0;
    render();
    g_core.download("/update/rondo.rondo", dataDir() + "/update.rondo");
  }

  void startInstall() {
    if (g_player.state().playing) g_player.pause();
    std::string file = dataDir() + "/update.rondo", dir = dataDir();
    auto alive = alive_;
    // checking and unpacking take a few seconds: off the UI thread (the screen shows Installing…)
    std::thread([this, alive, file, dir] {
      std::string version, why;
      bool ok = bundle::install(file, dir, version, why);
      postToMain([this, alive, ok, version, why] {
        if (!*alive) return;
        installOk_ = ok, installed_ = version, installWhy_ = why, installDone_ = true;
      });
    }).detach();
  }

  State state_ = kChecking;
  UpdateInfo info_;
  std::string why_, installed_, installWhy_;
  int percent_ = 0;
  bool installDone_ = false, installOk_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

std::unique_ptr<Screen> makeSoftwareUpdate() { return std::unique_ptr<Screen>(new SoftwareUpdateScreen()); }

}  // namespace rondo::shell
