// Settings › Sound's model (sound.h).
#include "sound.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <yyjson.h>

#include <algorithm>

namespace rondo {

const float kEqFreqs[kEqBands] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

namespace {

// Android's equalizer presets (the music effect bundle every Android 4.x player offered, the stock
// Y1's among them): five bands at 60, 230, 910, 3600 and 14000 Hz, Q 0.96.
struct FiveBand {
  const char* id;
  float gains[5];
};
const FiveBand kAndroid[] = {
    {"normal", {3, 0, 0, 0, 3}},        {"classical", {5, 3, -2, 4, 4}}, {"dance", {6, 0, 2, 4, 1}},
    {"folk", {3, 0, 0, 2, -1}},         {"heavymetal", {4, 1, 9, 3, 0}}, {"hiphop", {5, 3, 0, 1, 3}},
    {"jazz", {4, 2, -2, 2, 5}},         {"pop", {-1, 2, 5, 1, -2}},      {"rock", {5, 3, -1, 3, 5}},
};
const float kAndroidFreqs[5] = {60, 230, 910, 3600, 14000};

// Rondo's own, on the ten bands
struct TenBand {
  const char* id;
  float gains[kEqBands];
};
const TenBand kRondo[] = {
    {"bass", {6, 5, 4, 2, 0, 0, 0, 0, 0, 0}},
    {"treble", {0, 0, 0, 0, 0, 0, 2, 4, 5, 6}},
    {"vocal", {-2, -2, -1, 0, 2, 4, 4, 3, 1, 0}},
    {"loudness", {5, 4, 2, 0, -1, -1, 0, 2, 4, 4}},  // the low and high ends ears lose at low volume
    // the Y1's own speaker: the voices' range comes forward (what it can't play, below 180 Hz, is cut on
    // the speaker whatever the preset: resolveSound)
    {"speaker", {0, 0, 0, 0, 1, 2, 3, 3, 2, 1}},
};

const TenBand* rondoPreset(const std::string& id) {
  for (const TenBand& t : kRondo)
    if (id == t.id) return &t;
  return nullptr;
}
const FiveBand* androidPreset(const std::string& id) {
  for (const FiveBand& f : kAndroid)
    if (id == f.id) return &f;
  return nullptr;
}
const UserPreset* userPreset(const SoundConfig& c, const std::string& id) {
  if (id.compare(0, 5, "user:") != 0) return nullptr;
  for (const UserPreset& u : c.userPresets)
    if (u.name == id.substr(5)) return &u;
  return nullptr;
}

}  // namespace

const std::vector<EqPreset>& eqPresets() {
  static const std::vector<EqPreset> all = {
      {"flat", "Flat", "as it comes"},
      {"speaker", "Y1 speaker", "made for the speaker"},
      {"loudness", "Loudness", "for low volume"},
      {"bass", "Bass boost", ""},
      {"treble", "Treble boost", ""},
      {"vocal", "Vocal", ""},
      {"normal", "Normal", ""},
      {"classical", "Classical", ""},
      {"dance", "Dance", ""},
      {"folk", "Folk", ""},
      {"heavymetal", "Heavy Metal", ""},
      {"hiphop", "Hip Hop", ""},
      {"jazz", "Jazz", ""},
      {"pop", "Pop", ""},
      {"rock", "Rock", ""},
  };
  return all;
}

std::string presetName(const SoundConfig& c, const SoundProfile& p) {
  if (p.preset == "custom") return "Custom";
  if (const UserPreset* u = userPreset(c, p.preset)) return u->name;
  for (const EqPreset& e : eqPresets())
    if (p.preset == e.id) return e.name;
  return "Flat";
}

std::array<float, kEqBands> presetGains(const SoundConfig& c, const SoundProfile& p) {
  std::array<float, kEqBands> g{};
  if (p.preset == "custom") return p.custom;
  if (const UserPreset* u = userPreset(c, p.preset)) return u->gains;
  if (const TenBand* t = rondoPreset(p.preset)) {
    std::copy(t->gains, t->gains + kEqBands, g.begin());
    return g;
  }
  if (const FiveBand* f = androidPreset(p.preset)) {  // read off at the ten bands, for the editor
    for (int i = 0; i < kEqBands; i++) {
      float lf = log2f(kEqFreqs[i]), best = 1e9f;
      for (int k = 0; k < 5; k++)
        if (fabsf(log2f(kAndroidFreqs[k]) - lf) < best) best = fabsf(log2f(kAndroidFreqs[k]) - lf), g[size_t(i)] = f->gains[k];
    }
  }
  return g;
}

SoundSettings resolveSound(const SoundConfig& c, const SoundProfile& p, bool speaker) {
  SoundSettings s;
  if (const FiveBand* f = androidPreset(p.preset)) {
    for (int k = 0; k < 5; k++) s.filters.push_back({FilterType::Peak, kAndroidFreqs[k], f->gains[k], 0.96f});
  } else {
    std::array<float, kEqBands> g = presetGains(c, p);
    for (int i = 0; i < kEqBands; i++) s.filters.push_back({FilterType::Peak, kEqFreqs[i], g[size_t(i)], 1.41f});
  }
  s.gainDb = float(p.boostDb);
  s.leveling = c.leveling;
  s.levelingTargetDb = 4;  // ReplayGain's -18 LUFS reference, raised to the -14 LUFS of the streaming services
  if (speaker && s.active()) s.filters.insert(s.filters.begin(), {FilterType::HighPass, 180, 0, 0.707f});
  return s;
}

SoundConfig loadSound(const std::string& path) {
  SoundConfig c;
  yyjson_doc* doc = yyjson_read_file(path.c_str(), 0, nullptr, nullptr);
  if (!doc) return c;
  yyjson_val* root = yyjson_doc_get_root(doc);
  yyjson_val* v = yyjson_obj_get(root, "leveling");
  if (yyjson_is_bool(v)) c.leveling = yyjson_get_bool(v);
  auto gains = [](yyjson_val* arr, std::array<float, kEqBands>& out) {
    size_t i, n;
    yyjson_val* e;
    yyjson_arr_foreach(arr, i, n, e) if (i < size_t(kEqBands) && yyjson_is_num(e)) out[i] =
        std::max(-12.f, std::min(12.f, float(yyjson_get_num(e))));
  };
  yyjson_val* outs = yyjson_obj_get(root, "outputs");
  size_t i, n;
  yyjson_val *k, *o;
  yyjson_obj_foreach(outs, i, n, k, o) {
    SoundProfile p;
    yyjson_val* pr = yyjson_obj_get(o, "preset");
    if (yyjson_is_str(pr)) p.preset = yyjson_get_str(pr);
    gains(yyjson_obj_get(o, "custom"), p.custom);
    yyjson_val* b = yyjson_obj_get(o, "boostDb");
    if (yyjson_is_int(b)) p.boostDb = std::max(0, std::min(12, int(yyjson_get_int(b))));
    c.outputs[yyjson_get_str(k)] = p;
  }
  yyjson_val* users = yyjson_obj_get(root, "userPresets");
  yyjson_val* u;
  yyjson_arr_foreach(users, i, n, u) {
    yyjson_val* nm = yyjson_obj_get(u, "name");
    if (!yyjson_is_str(nm)) continue;
    UserPreset up;
    up.name = yyjson_get_str(nm);
    gains(yyjson_obj_get(u, "gains"), up.gains);
    c.userPresets.push_back(up);
  }
  yyjson_doc_free(doc);
  return c;
}

bool saveSound(const std::string& path, const SoundConfig& c) {
  yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
  yyjson_mut_val* root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_bool(doc, root, "leveling", c.leveling);
  auto gains = [&](const std::array<float, kEqBands>& g) {
    yyjson_mut_val* a = yyjson_mut_arr(doc);
    for (float x : g) yyjson_mut_arr_add_real(doc, a, double(x));
    return a;
  };
  yyjson_mut_val* outs = yyjson_mut_obj(doc);
  for (const auto& [key, p] : c.outputs) {
    yyjson_mut_val* o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "preset", p.preset.c_str());
    yyjson_mut_obj_add_val(doc, o, "custom", gains(p.custom));
    yyjson_mut_obj_add_int(doc, o, "boostDb", p.boostDb);
    yyjson_mut_obj_add(outs, yyjson_mut_strcpy(doc, key.c_str()), o);
  }
  yyjson_mut_obj_add_val(doc, root, "outputs", outs);
  yyjson_mut_val* users = yyjson_mut_arr(doc);
  for (const UserPreset& u : c.userPresets) {
    yyjson_mut_val* o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "name", u.name.c_str());
    yyjson_mut_obj_add_val(doc, o, "gains", gains(u.gains));
    yyjson_mut_arr_append(users, o);
  }
  yyjson_mut_obj_add_val(doc, root, "userPresets", users);
  size_t len = 0;
  char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &len);
  yyjson_mut_doc_free(doc);
  if (!json) return false;
  // atomic: a temporary file, synced, then renamed over the old one
  std::string tmp = path + ".tmp";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  bool ok = fd >= 0 && write(fd, json, len) == ssize_t(len) && write(fd, "\n", 1) == 1 && fsync(fd) == 0;
  if (fd >= 0) close(fd);
  free(json);
  ok = ok && rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) unlink(tmp.c_str());
  return ok;
}

}  // namespace rondo
