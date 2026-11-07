#include "config.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <yyjson.h>

namespace rondo {

const char* const kMenuIds[6] = {"nowPlaying", "device", "videos", "music", "radio", "settings"};
const float kTextScales[3] = {1.25f, 1.4f, 1.55f};
const AccentPreset kAccentPresets[6] = {{"iPod Blue", "#1c5ccc"}, {"Rondo Red", "#ff3b4f"}, {"Mint", "#11a683"},
                                        {"Violet", "#7b4fe0"},    {"Amber", "#e8860c"},     {"Graphite", "#4a4f59"}};

const char* menuLabel(const std::string& id) {
  if (id == "music") return "YouTube Music";
  if (id == "videos") return "YouTube";
  if (id == "device") return "Library";
  if (id == "nowPlaying") return "Now Playing";
  if (id == "radio") return "FM Radio";
  if (id == "settings") return "Settings";
  return "";
}

bool parseColor(const std::string& s, unsigned* rgb) {
  // Android's Color.parseColor: #rrggbb or #aarrggbb
  if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
  for (size_t i = 1; i < s.size(); i++)
    if (!isxdigit((unsigned char)s[i])) return false;
  unsigned long v = strtoul(s.c_str() + 1, nullptr, 16);
  if (rgb) *rgb = unsigned(v & 0xffffff);
  return true;
}

void sanitize(Theme& t) {
  std::vector<MenuEntry> seen;
  for (const MenuEntry& e : t.menu) {
    bool known = std::any_of(std::begin(kMenuIds), std::end(kMenuIds), [&](const char* id) { return e.id == id; });
    bool dup = std::any_of(seen.begin(), seen.end(), [&](const MenuEntry& s) { return s.id == e.id; });
    if (known && !dup) seen.push_back(e);
  }
  // an entry added in a later version goes just before Settings, not after it
  std::vector<MenuEntry> added;
  for (const char* id : kMenuIds)
    if (std::none_of(seen.begin(), seen.end(), [&](const MenuEntry& s) { return s.id == id; })) added.push_back({id, true});
  size_t settingsAt = seen.size();
  for (size_t i = 0; i < seen.size(); i++)
    if (seen[i].id == "settings") settingsAt = i;
  std::vector<MenuEntry> menu(seen.begin(), seen.begin() + long(settingsAt));
  for (auto& a : added)
    if (a.id != "settings") menu.push_back(a);
  menu.insert(menu.end(), seen.begin() + long(settingsAt), seen.end());
  for (auto& a : added)
    if (a.id == "settings") menu.push_back(a);
  for (auto& m : menu)
    if (m.id == "settings") m.visible = true;
  t.menu = menu;
  if (!parseColor(t.accent, nullptr)) t.accent = Theme().accent;
  float best = 1.4f;
  for (float s : kTextScales)
    if (fabsf(s - t.textScale) < fabsf(best - t.textScale)) best = s;
  t.textScale = best;
  t.appearance = t.appearance == "dark" ? "dark" : "light";
}

namespace {

std::string str(yyjson_val* o, const char* key, const std::string& def) {
  yyjson_val* v = yyjson_obj_get(o, key);
  return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : def;
}
bool boolean(yyjson_val* o, const char* key, bool def) {
  yyjson_val* v = yyjson_obj_get(o, key);
  return yyjson_is_bool(v) ? yyjson_get_bool(v) : def;
}
double num(yyjson_val* o, const char* key, double def) {
  yyjson_val* v = yyjson_obj_get(o, key);
  return yyjson_is_num(v) ? yyjson_get_num(v) : def;
}

}  // namespace

Config loadConfig(const char* path) {
  Config c;
  yyjson_doc* doc = yyjson_read_file(path, 0, nullptr, nullptr);
  yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
  if (yyjson_is_obj(root)) {
    if (yyjson_val* t = yyjson_obj_get(root, "theme")) {
      c.theme.accent = str(t, "accent", c.theme.accent);
      c.theme.appearance = str(t, "appearance", c.theme.appearance);
      c.theme.textScale = float(num(t, "textScale", c.theme.textScale));
      yyjson_val* menu = yyjson_obj_get(t, "menu");
      size_t i, n;
      yyjson_val* e;
      yyjson_arr_foreach(menu, i, n, e) c.theme.menu.push_back({str(e, "id", ""), boolean(e, "visible", true)});
    }
    yyjson_val* b = yyjson_obj_get(root, "behavior");
    if (!b) b = yyjson_obj_get(root, "behaviour");
    if (b) {
      c.behavior.lyricsOnOpen = boolean(b, "lyricsOnOpen", false);
      c.behavior.wheelAcceleration = str(b, "wheelAcceleration", c.behavior.wheelAcceleration);
      c.behavior.wheelVibration = str(b, "wheelVibration", c.behavior.wheelVibration);
    }
    if (yyjson_val* s = yyjson_obj_get(root, "server")) {
      c.server.url = str(s, "url", "");
      c.server.token = str(s, "token", "");
    }
    if (yyjson_val* r = yyjson_obj_get(root, "radio")) {
      c.radio.frequency = int(num(r, "frequency", c.radio.frequency));
      c.radio.speaker = boolean(r, "speaker", false);
      c.radio.band = str(r, "band", c.radio.band);
      size_t i, n;
      yyjson_val* e;
      yyjson_arr_foreach(yyjson_obj_get(r, "presets"), i, n, e)
          c.radio.presets.push_back({int(num(e, "frequency", 0)), str(e, "name", "")});
      yyjson_arr_foreach(yyjson_obj_get(r, "found"), i, n, e) if (yyjson_is_num(e)) c.radio.found.push_back(int(yyjson_get_num(e)));
      yyjson_val *k, *v;
      yyjson_obj_iter it = yyjson_obj_iter_with(yyjson_obj_get(r, "names"));
      while ((k = yyjson_obj_iter_next(&it))) {
        v = yyjson_obj_iter_get_val(k);
        if (yyjson_is_str(v)) c.radio.names[atoi(yyjson_get_str(k))] = yyjson_get_str(v);
      }
    }
  }
  if (doc) yyjson_doc_free(doc);
  sanitize(c.theme);
  return c;
}

bool saveConfig(const char* path, const Config& c) {
  yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
  yyjson_mut_val* root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_val* t = yyjson_mut_obj_add_obj(doc, root, "theme");
  yyjson_mut_obj_add_strcpy(doc, t, "accent", c.theme.accent.c_str());
  yyjson_mut_obj_add_strcpy(doc, t, "appearance", c.theme.appearance.c_str());
  yyjson_mut_obj_add_real(doc, t, "textScale", double(c.theme.textScale));
  yyjson_mut_val* menu = yyjson_mut_obj_add_arr(doc, t, "menu");
  for (const MenuEntry& m : c.theme.menu) {
    yyjson_mut_val* e = yyjson_mut_arr_add_obj(doc, menu);
    yyjson_mut_obj_add_strcpy(doc, e, "id", m.id.c_str());
    yyjson_mut_obj_add_bool(doc, e, "visible", m.visible);
  }
  yyjson_mut_val* b = yyjson_mut_obj_add_obj(doc, root, "behavior");
  yyjson_mut_obj_add_bool(doc, b, "lyricsOnOpen", c.behavior.lyricsOnOpen);
  yyjson_mut_obj_add_strcpy(doc, b, "wheelAcceleration", c.behavior.wheelAcceleration.c_str());
  yyjson_mut_obj_add_strcpy(doc, b, "wheelVibration", c.behavior.wheelVibration.c_str());
  yyjson_mut_val* s = yyjson_mut_obj_add_obj(doc, root, "server");
  if (c.server.url.empty()) yyjson_mut_obj_add_null(doc, s, "url");
  else yyjson_mut_obj_add_strcpy(doc, s, "url", c.server.url.c_str());
  if (c.server.token.empty()) yyjson_mut_obj_add_null(doc, s, "token");
  else yyjson_mut_obj_add_strcpy(doc, s, "token", c.server.token.c_str());
  yyjson_mut_val* r = yyjson_mut_obj_add_obj(doc, root, "radio");
  yyjson_mut_obj_add_int(doc, r, "frequency", c.radio.frequency);
  yyjson_mut_obj_add_bool(doc, r, "speaker", c.radio.speaker);
  yyjson_mut_val* presets = yyjson_mut_obj_add_arr(doc, r, "presets");
  for (const Preset& p : c.radio.presets) {
    yyjson_mut_val* e = yyjson_mut_arr_add_obj(doc, presets);
    yyjson_mut_obj_add_int(doc, e, "frequency", p.frequency);
    yyjson_mut_obj_add_strcpy(doc, e, "name", p.name.c_str());
  }
  yyjson_mut_val* found = yyjson_mut_obj_add_arr(doc, r, "found");
  for (int f : c.radio.found) yyjson_mut_arr_add_int(doc, found, f);
  yyjson_mut_obj_add_strcpy(doc, r, "band", c.radio.band.c_str());
  yyjson_mut_val* names = yyjson_mut_obj_add_obj(doc, r, "names");
  for (const auto& kv : c.radio.names) {
    char key[16];
    snprintf(key, sizeof(key), "%d", kv.first);
    yyjson_mut_obj_add(names, yyjson_mut_strcpy(doc, key), yyjson_mut_strcpy(doc, kv.second.c_str()));
  }
  size_t len = 0;
  char* json = yyjson_mut_write(doc, 0, &len);
  yyjson_mut_doc_free(doc);
  if (!json) return false;
  std::string tmp = std::string(path) + ".part";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  // Rondo runs as root, the file belongs to 1.0.x's app user (they share it until 2.0 replaces 1.0.x):
  // the new file keeps the old one's owner and mode, or takes its folder's owner, or 1.0.x could no
  // longer read its own settings.
  struct stat st;
  std::string dir = std::string(path).substr(0, std::string(path).rfind('/'));
  if (fd >= 0 && (stat(path, &st) == 0 || stat(dir.c_str(), &st) == 0)) {
    if (fchown(fd, st.st_uid, st.st_gid) != 0) fprintf(stderr, "rondo: config owner not kept\n");
    if (S_ISREG(st.st_mode)) fchmod(fd, st.st_mode & 07777);
  }
  bool ok = fd >= 0 && write(fd, json, len) == ssize_t(len) && fsync(fd) == 0;
  if (fd >= 0) close(fd);
  free(json);
  if (ok) ok = rename(tmp.c_str(), path) == 0;
  else unlink(tmp.c_str());
  return ok;
}

}  // namespace rondo
