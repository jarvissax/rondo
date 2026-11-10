// Prefs; see prefs.h.
#include "prefs.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>
#include <yyjson.h>

namespace rondo {
namespace {

const char* kAndroidSettings = "/data/data/com.android.providers.settings/databases/settings.db";
const char* kOldPrefs = "/data/data/com.jarvissax.rondo/shared_prefs/y1.xml";

// 1.0.x's SharedPreferences file: <int name="video_quality" value="720" />
void fromOldPrefs(Prefs& p) {
  FILE* f = fopen(kOldPrefs, "rb");
  if (!f) return;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  auto value = [&](const char* name) -> const char* {
    const char* at = strstr(buf, name);
    const char* v = at ? strstr(at, "value=\"") : nullptr;
    return v ? v + 7 : nullptr;
  };
  if (const char* v = value("name=\"video_quality\"")) p.videoQuality = atoi(v);
  if (const char* v = value("name=\"account_music\"")) p.accountMusic = strncmp(v, "false", 5) != 0;
  if (const char* v = value("name=\"account_videos\"")) p.accountVideos = strncmp(v, "false", 5) != 0;
}

// Android's global settings bluetooth_on or wifi_on: whether that radio was on
void fromAndroidGlobal(const char* name, bool& out) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(kAndroidSettings, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return;
  }
  sqlite3_stmt* q = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT value FROM global WHERE name = ?", -1, &q, nullptr) == SQLITE_OK &&
      sqlite3_bind_text(q, 1, name, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
    const char* value = reinterpret_cast<const char*>(sqlite3_column_text(q, 0));
    if (value) out = atoi(value) != 0;
  }
  sqlite3_finalize(q);
  sqlite3_close(db);
}

// Android's settings: the system table's screen_brightness, screen_off_timeout, sound_effects_enabled
void fromAndroid(Prefs& p) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(kAndroidSettings, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return;
  }
  sqlite3_stmt* q = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT name, value FROM system WHERE name IN ('screen_brightness', 'screen_off_timeout',"
                             " 'sound_effects_enabled')",
                         -1, &q, nullptr) == SQLITE_OK) {
    while (sqlite3_step(q) == SQLITE_ROW) {
      const char* name = reinterpret_cast<const char*>(sqlite3_column_text(q, 0));
      const char* value = reinterpret_cast<const char*>(sqlite3_column_text(q, 1));
      if (!name || !value) continue;
      int v = atoi(value);
      if (!strcmp(name, "screen_brightness")) p.brightness = v;
      else if (!strcmp(name, "screen_off_timeout")) p.screenTimeoutMs = v;
      else if (!strcmp(name, "sound_effects_enabled")) p.clickSound = v != 0;
    }
  }
  sqlite3_finalize(q);
  sqlite3_close(db);
  fromAndroidGlobal("bluetooth_on", p.bluetoothOn);
  fromAndroidGlobal("wifi_on", p.wifiOn);
}

}  // namespace

Prefs loadPrefs(const std::string& path) {
  Prefs p;
  yyjson_doc* doc = yyjson_read_file(path.c_str(), 0, nullptr, nullptr);
  if (!doc) {
    fromOldPrefs(p);
    fromAndroid(p);
    return p;
  }
  yyjson_val* root = yyjson_doc_get_root(doc);
  auto num = [&](const char* k, int def) {
    yyjson_val* v = yyjson_obj_get(root, k);
    return yyjson_is_int(v) ? int(yyjson_get_int(v)) : def;
  };
  p.videoQuality = num("videoQuality", p.videoQuality);
  p.brightness = num("brightness", p.brightness);
  p.screenTimeoutMs = num("screenTimeoutMs", p.screenTimeoutMs);
  p.fmVolume = num("fmVolume", p.fmVolume);
  auto flag = [&](const char* k, bool& out) {
    yyjson_val* v = yyjson_obj_get(root, k);
    if (yyjson_is_bool(v)) out = yyjson_get_bool(v);
  };
  flag("clickSound", p.clickSound);
  flag("accountMusic", p.accountMusic);
  flag("accountVideos", p.accountVideos);
  flag("autoTime", p.autoTime);
  flag("autoZone", p.autoZone);
  flag("clock24", p.clock24);
  // a file from before Bluetooth was native: Android's setting, once
  if (yyjson_is_bool(yyjson_obj_get(root, "bluetoothOn"))) flag("bluetoothOn", p.bluetoothOn);
  else fromAndroidGlobal("bluetooth_on", p.bluetoothOn);
  // and from before Wi-Fi's switch was remembered here
  if (yyjson_is_bool(yyjson_obj_get(root, "wifiOn"))) flag("wifiOn", p.wifiOn);
  else fromAndroidGlobal("wifi_on", p.wifiOn);
  yyjson_doc_free(doc);
  return p;
}

bool savePrefs(const std::string& path, const Prefs& p) {
  char json[512];
  auto b = [](bool v) { return v ? "true" : "false"; };
  int n = snprintf(json, sizeof(json),
                   "{\"videoQuality\":%d,\"brightness\":%d,\"screenTimeoutMs\":%d,\"clickSound\":%s,\"accountMusic\":%s,"
                   "\"accountVideos\":%s,\"bluetoothOn\":%s,\"wifiOn\":%s,\"fmVolume\":%d,\"autoTime\":%s,\"autoZone\":%s,"
                   "\"clock24\":%s}\n",
                   p.videoQuality, p.brightness, p.screenTimeoutMs, b(p.clickSound), b(p.accountMusic), b(p.accountVideos),
                   b(p.bluetoothOn), b(p.wifiOn), p.fmVolume, b(p.autoTime), b(p.autoZone), b(p.clock24));
  std::string tmp = path + ".part";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  bool ok = fd >= 0 && write(fd, json, size_t(n)) == n && fsync(fd) == 0;
  if (fd >= 0) close(fd);
  if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
  else unlink(tmp.c_str());
  return ok;
}

}  // namespace rondo
