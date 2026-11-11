// Settings › Sound (new in 2.0: the stock Y1 is quiet and flat): an
// equalizer, a loudness boost and volume leveling, each output keeping its own (the speaker, wired
// headphones, every Bluetooth device). rondo-player does the work (player/dsp.h); this sends it what
// the output in use needs, again whenever that output changes.
#include <math.h>
#include <stdio.h>
#include <yyjson.h>

#include <algorithm>
#include <array>
#include <memory>


#include "../core/sound.h"
#include "../platform/power.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {
namespace {

SoundConfig g_sound;
std::string g_soundPath;
bool g_loaded = false;
int64_t g_dirtyAt = 0;  // a change not written yet (the wheel moves bands fast)
std::string g_sent;     // what the player has now (encoded)

struct Output {
  std::string key, label;
};

// the output music plays on: Bluetooth headphones connected, else wired headphones in, else the speaker
Output currentOutput() {
  // development: RONDO_SOUND_OUTPUT=headphones, or bt:<a name> (a Bluetooth device by that name)
  if (const char* o = getenv("RONDO_SOUND_OUTPUT")) {
    std::string v = o;
    if (v.compare(0, 3, "bt:") == 0) return {"bt:test", v.substr(3)};
    if (v == "headphones") return {"headphones", "Headphones"};
  }
  CoreState s = g_core.state();
  for (int i = 0; i < s.btCount; i++)
    if (s.bt[i].a2dp == 2) return {std::string("bt:") + s.bt[i].address, btName(s.bt[i])};
  if (power::headset() != 0) return {"headphones", "Headphones"};
  return {"speaker", "Speaker"};
}

void load() {
  if (g_loaded) return;
  g_loaded = true;
  g_soundPath = g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1) + "sound.json";
  g_sound = loadSound(g_soundPath);
}

SoundProfile& profile(const std::string& key) { return g_sound.outputs[key]; }

void apply() {
  load();
  Output o = currentOutput();
  auto it = g_sound.outputs.find(o.key);
  SoundSettings s = resolveSound(g_sound, it == g_sound.outputs.end() ? SoundProfile() : it->second, o.key == "speaker");
  std::vector<uint8_t> b = encodeSound(s);
  std::string enc(b.begin(), b.end());
  if (enc == g_sent) return;
  g_sent = enc;
  g_player.sound(s);
}

void changed() {
  g_dirtyAt = now_ns();
  apply();
}

void saveNow() {
  if (!g_dirtyAt) return;
  g_dirtyAt = 0;
  if (!saveSound(g_soundPath, g_sound)) fprintf(stderr, "rondo: sound: can't write %s\n", g_soundPath.c_str());
}

// Volume leveling's measuring of the library: allowed while leveling is on and the Y1 charges
void leveling(int64_t now) {
  static bool told = false, allowed = false;
  static size_t songs = SIZE_MAX;
  static int64_t lastList = 0;
  if (!told) {
    told = true;
    g_player.loudnessFile(g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1) + "loudness.tsv");
  }
  if (now - lastList > 30 * kSec) {  // the library's songs, again when their number changes
    lastList = now;
    std::vector<const Track*> all = g_library.songs();
    std::vector<std::string> paths;
    for (const Track* t : all)
      if (!t->gone) paths.push_back(t->path);
    if (paths.size() != songs) songs = paths.size(), g_player.analyzeList(paths);
  }
  bool on = g_sound.leveling && power::battery().plugged;
  if (on != allowed) allowed = on, g_player.analyzeAllowed(on);
}

std::string levelingNote() {
  PlayerState s = g_player.state();
  if (!g_sound.leveling || s.loudnessTotal <= 0) return "Songs at one loudness, measured as they play";
  std::string n = std::to_string(s.loudnessKnown) + " of " + std::to_string(s.loudnessTotal) + " songs measured";
  if (s.loudnessKnown < s.loudnessTotal) n += power::battery().plugged ? ", measuring" : ", the rest while charging";
  return n;
}

std::string boostName(int db) { return db ? "+" + std::to_string(db) + " dB" : "Off"; }

std::string freqName(float f) {
  return f >= 1000 ? std::to_string(int(f / 1000)) + "k" : std::to_string(int(f));
}

// The equalizer's ten bands, on the wheel: it changes the band picked, Prev/Next pick another;
// center saves them as a preset of your own, held it sets them back to flat. The sound follows as
// you turn.
class EqualizerScreen : public Screen {
 public:
  explicit EqualizerScreen(Output o) : out_(std::move(o)) {
    SoundProfile& p = profile(out_.key);
    gains_ = presetGains(g_sound, p);
  }
  std::string title() const override { return "Equalizer"; }
  void onHide() override { saveNow(); }
  bool onAction(Action a, int steps) override {
    switch (a) {
      case Action::Up:
      case Action::Down: {
        float& g = gains_[size_t(band_)];
        float to = std::max(-12.f, std::min(12.f, roundf(g) + (a == Action::Down ? 1.f : -1.f) * float(steps)));
        if (to == g) return true;
        g = to;
        useCustom();
        return true;
      }
      case Action::Next:
      case Action::LongNext: band_ = std::min(kEqBands - 1, band_ + 1); return true;
      case Action::Prev:
      case Action::LongPrev: band_ = std::max(0, band_ - 1); return true;
      case Action::Select: savePreset(); return true;
      case Action::LongSelect:
        gains_.fill(0);
        useCustom();
        return true;
      default: return false;
    }
  }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    int pad = e.dp(12);
    // the top line: the output and the preset, the band picked and its gain
    Label where = label(e, out_.label + " \xc2\xb7 " + presetName(g_sound, profile(out_.key)), 12, false, w / 2);
    char v[32];
    float g = gains_[size_t(band_)];
    float hz = kEqFreqs[band_];
    std::string band = hz >= 1000 ? std::to_string(int(hz / 1000)) + " kHz" : std::to_string(int(hz)) + " Hz";
    snprintf(v, sizeof(v), "%s  %s%d dB", band.c_str(), g > 0 ? "+" : "", int(g));
    Label value = label(e, v, 12, true, w / 2);
    float top = float(e.dp(6));
    e.c.layer(1);
    drawLabel(e, where, float(pad), top, p.muted);
    drawLabel(e, value, w - pad - value.w, top, p.text);
    // the graph: ±12 dB, a column a band
    Label hint = label(e, "wheel: gain \xc2\xb7 Prev/Next: band \xc2\xb7 center: save as preset \xc2\xb7 hold: flat", 9, false, w);
    Label freq = label(e, "16k", 10, false);
    float gTop = top + where.h + e.dp(10), gBottom = h - hint.h - e.dp(8) - freq.h - e.dp(8);
    float gh = gBottom - gTop, mid = gTop + gh / 2, colW = (w - 2 * pad) / kEqBands;
    auto yOf = [&](float db) { return mid - db / 12.f * (gh / 2); };
    e.c.layer(0);
    for (float db : {-12.f, -6.f, 0.f, 6.f, 12.f}) e.c.fill({float(pad), roundf(yOf(db)), w - 2 * pad, db == 0 ? 2.f : 1.f}, p.rowLine);
    // the curve the filters make together, across the bands
    SoundSettings s = resolveSound(g_sound, profile(out_.key), out_.key == "speaker");
    e.c.layer(1);
    float x0 = float(pad) + colW / 2, x1 = w - pad - colW / 2;
    for (float x = x0; x <= x1; x += 2) {
      float hz = 31.f * powf(2.f, (x - x0) / (x1 - x0) * 9.f), db = 0;
      for (const Filter& f : s.filters) db += Dsp::responseDb(f, hz);
      db = std::max(-13.f, std::min(13.f, db));
      e.c.shape({x - 1.5f, yOf(db) - 1.5f, 3, 3}, 1.5f, withAlpha(p.accent, 0.45f));
    }
    // the bands
    e.c.layer(2);
    for (int i = 0; i < kEqBands; i++) {
      float cx = float(pad) + colW * (float(i) + 0.5f), gy = yOf(gains_[size_t(i)]);
      bool picked = i == band_;
      Color c = picked ? p.accent : p.muted;
      float bw = float(e.dp(picked ? 8 : 6));
      float y0 = std::min(mid, gy), y1 = std::max(mid, gy);
      if (y1 - y0 >= 1) e.c.shape({cx - bw / 2, y0, bw, y1 - y0}, bw / 2, withAlpha(c, picked ? 1.f : 0.7f));
      float k = float(e.dp(picked ? 7 : 5));
      e.c.shape({cx - k, gy - k, 2 * k, 2 * k}, k, c);
      Label f = label(e, freqName(kEqFreqs[i]), 10, picked, colW);
      drawLabel(e, f, cx - f.w / 2.f, gBottom + e.dp(8), picked ? p.text : p.muted);
    }
    e.c.layer(3);
    drawLabel(e, hint, centerText(0, w, hint.w), h - hint.h - e.dp(4), p.muted);
  }

 private:
  void useCustom() {
    SoundProfile& p = profile(out_.key);
    p.custom = gains_;
    p.preset = "custom";
    changed();
  }
  void savePreset() {
    std::array<float, kEqBands> gains = gains_;
    std::string key = out_.key;
    g_app->push(makeTextEntry(
        "Name this preset", "", "Save", "Kept with the presets, for every output", 30, nullptr,
        [](const std::string& name, std::function<void(const std::string&)> answer) {
          if (name.empty()) return answer("Type a name");
          for (const EqPreset& e : eqPresets())
            if (name == e.name) return answer("That's a built-in preset's name");
          answer("");
        },
        [gains, key](const std::string& name) {
          bool found = false;
          for (UserPreset& u : g_sound.userPresets)
            if (u.name == name) u.gains = gains, found = true;
          if (!found) g_sound.userPresets.push_back({name, gains});
          profile(key).preset = "user:" + name;
          changed();
          saveNow();
        }));
  }
  Output out_;
  std::array<float, kEqBands> gains_{};
  int band_ = 5;  // 1 kHz
};

// AutoEq's correction as the server's /eq/preset gives it, into a profile: false without filters
bool parseAutoEq(const std::string& body, SoundProfile& p) {
  yyjson_doc* d = yyjson_read(body.data(), body.size(), 0);
  if (!d) return false;
  yyjson_val* r = yyjson_doc_get_root(d);
  yyjson_val* pre = yyjson_obj_get(r, "preampDb");
  p.autoEq.clear();
  size_t i, n;
  yyjson_val* e;
  yyjson_arr_foreach(yyjson_obj_get(r, "filters"), i, n, e) {
    yyjson_val* t = yyjson_obj_get(e, "type");
    std::string type = yyjson_is_str(t) ? yyjson_get_str(t) : "";
    Filter flt;
    flt.type = type == "LSC" ? FilterType::LowShelf : type == "HSC" ? FilterType::HighShelf
               : type == "LPQ" ? FilterType::LowPass : type == "HPQ" ? FilterType::HighPass : FilterType::Peak;
    flt.freq = float(yyjson_get_num(yyjson_obj_get(e, "freq")));
    flt.gainDb = float(yyjson_get_num(yyjson_obj_get(e, "gainDb")));
    flt.q = float(yyjson_get_num(yyjson_obj_get(e, "q")));
    if (flt.freq > 0 && p.autoEq.size() < 16) p.autoEq.push_back(flt);
  }
  p.autoEqPreamp = yyjson_is_num(pre) ? float(yyjson_get_num(pre)) : 0;
  yyjson_doc_free(d);
  return !p.autoEq.empty();
}

// Headphone match: AutoEq's correction for the model (through the server, /eq/search and
// /eq/preset), looked up by the Bluetooth device's name or by a name typed in; the one picked is kept
// with the output.
class HeadphoneMatchScreen : public ShellListScreen {
 public:
  explicit HeadphoneMatchScreen(Output o) : ShellListScreen("Headphone match"), out_(std::move(o)) {
    if (out_.key.compare(0, 3, "bt:") == 0) lookUp(out_.label);
    else render();
  }
  ~HeadphoneMatchScreen() override { *alive_ = false; }

 private:
  struct Found {
    std::string id, name, source;
  };
  void lookUp(const std::string& q) {
    query_ = q, state_ = "Looking\xe2\x80\xa6", found_.clear();
    render();
    auto alive = alive_;
    g_core.get("/eq/search?q=" + Api::escape(q), 60000, [this, alive](int status, const std::string& body) {
      if (!*alive) return;
      found_.clear();
      if (status == 404) state_ = "This server can't look headphones up yet: update it";
      else if (status != 200) state_ = "The server didn't answer";
      else if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
        size_t i, n;
        yyjson_val* e;
        yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(d), "items"), i, n, e) {
          auto str = [e](const char* k) {
            yyjson_val* v = yyjson_obj_get(e, k);
            return std::string(yyjson_is_str(v) ? yyjson_get_str(v) : "");
          };
          found_.push_back({str("id"), str("name"), str("source")});
        }
        yyjson_doc_free(d);
        state_ = found_.empty() ? "No model by that name" : "";
      }
      fresh_ = true;  // new answers: the first of them picked
      render();
    });
  }
  void pick(Found f) {  // a copy: the row holding it goes as the list is drawn again
    state_ = "Getting " + f.name + "\xe2\x80\xa6", found_.clear();
    render();
    auto alive = alive_;
    std::string key = out_.key;
    g_core.get("/eq/preset?id=" + Api::escape(f.id), 0, [this, alive, key, f](int status, const std::string& body) {
      SoundProfile p = profile(key);
      bool ok = status == 200 && parseAutoEq(body, p);
      if (ok) {
        p.autoEqName = f.name, p.autoEqSource = f.source, p.preset = "autoeq";
        profile(key) = p;
        changed();
        saveNow();
      }
      if (!*alive) return;
      if (ok) {
        g_app->pop();
        return;
      }
      state_ = status == 0 ? "The server didn't answer" : "That model's correction didn't come";
      render();
    });
  }
  void render() {
    std::vector<Row> rows;
    const SoundProfile& p = profile(out_.key);
    if (!p.autoEq.empty()) {
      Row now;
      now.title = p.autoEqName;
      now.subtitle = "In use, measured by " + p.autoEqSource;
      now.trailing = p.preset == "autoeq" ? "Current" : "";
      std::string key = out_.key;
      now.onSelect = [key] {
        profile(key).preset = "autoeq";
        changed();
        g_app->pop();
      };
      rows.push_back(now);
    }
    if (!query_.empty()) {
      Row h;
      h.header = true;
      h.title = "For \xe2\x80\x9c" + query_ + "\xe2\x80\x9d";
      rows.push_back(h);
    }
    if (!state_.empty()) {
      Row r;
      r.title = state_;
      r.enabled = false;
      rows.push_back(r);
    }
    for (const Found& f : found_) {
      Row r;
      r.title = f.name;
      r.subtitle = "Measured by " + f.source;
      r.onSelect = [this, f] { pick(f); };
      rows.push_back(r);
    }
    Row search;
    search.title = "Search by model";
    search.subtitle = "From AutoEq's measurements of about 9,000 headphones";
    search.chevron = true;
    auto alive = alive_;
    search.onSelect = [this, alive] {
      g_app->push(makeTextEntry(
          "Headphone model", query_, "Search", "Like Sennheiser HD 650 or AirPods Pro", 60, nullptr,
          [](const std::string& q, std::function<void(const std::string&)> answer) { answer(q.empty() ? "Type a model" : ""); },
          [this, alive](const std::string& q) {
            if (*alive) lookUp(q);
          }));
    };
    rows.push_back(search);
    list_.setRows(std::move(rows), !fresh_);
    fresh_ = false;
  }
  Output out_;
  std::string query_, state_;
  bool fresh_ = false;
  std::vector<Found> found_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

class SoundScreen : public ShellListScreen {
 public:
  SoundScreen() : ShellListScreen("Sound") { render(); }
  void onShow() override {
    out_ = currentOutput();
    render();
  }
  void onHide() override { saveNow(); }
  bool tick(int64_t) override {
    Output o = currentOutput();
    std::string note = levelingNote();
    PlayerState ps = g_player.state();
    if (o.key == out_.key && note == note_ && ps.amplifier == amp_ && ps.volumeLimit == limit_) return false;
    out_ = o;
    render();
    return true;
  }

 private:
  void render() {
    load();
    if (out_.key.empty()) out_ = currentOutput();
    SoundProfile& p = profile(out_.key);
    std::string key = out_.key;
    std::vector<Row> rows;
    Row on;
    on.title = "Playing on";
    on.subtitle = "Each output keeps its own sound";
    on.trailing = out_.label;
    on.tile = &tiles::output;
    on.enabled = false;
    rows.push_back(on);
    Row eq;
    eq.title = "Equalizer";
    eq.trailing = presetName(g_sound, p);
    eq.chevron = true;
    eq.onSelect = [key] {
      std::vector<std::pair<std::string, std::string>> c;
      for (const EqPreset& e : eqPresets()) c.push_back({e.name, e.id});
      for (const UserPreset& u : g_sound.userPresets) c.push_back({u.name, "user:" + u.name});
      if (!profile(key).autoEq.empty()) c.push_back({profile(key).autoEqName, "autoeq"});
      c.push_back({"Custom", "custom"});
      g_app->push(makeChoice(
          "Equalizer", c, [key] { return profile(key).preset; },
          [key](const std::string& v) {
            profile(key).preset = v;
            changed();
          },
          [](const std::string& v) {
            for (const EqPreset& e : eqPresets())
              if (v == e.id) return std::string(e.note);
            return std::string(v == "custom" ? "your own bands" : v == "autoeq" ? "headphone match" : "");
          }));
    };
    rows.push_back(eq);
    Row edit;
    edit.title = "Edit equalizer";
    edit.subtitle = "Ten bands of your own";
    edit.chevron = true;
    Output o = out_;
    edit.onSelect = [o] { g_app->push(std::unique_ptr<Screen>(new EqualizerScreen(o))); };
    rows.push_back(edit);
    if (key != "speaker") {  // headphones, wired or Bluetooth
      Row match;
      match.title = "Headphone match";
      match.subtitle = p.autoEq.empty() ? "A correction for your model (AutoEq)" : p.autoEqName;
      match.chevron = true;
      match.onSelect = [o] { g_app->push(std::unique_ptr<Screen>(new HeadphoneMatchScreen(o))); };
      rows.push_back(match);
    }
    Row boost;
    boost.title = "Loudness boost";
    boost.subtitle = "Louder; the peaks are held, never clipped";
    boost.trailing = boostName(p.boostDb);
    boost.chevron = true;
    boost.onSelect = [key] {
      std::vector<std::pair<std::string, std::string>> c;
      for (int db : {0, 3, 6, 9, 12}) c.push_back({boostName(db), std::to_string(db)});
      g_app->push(makeChoice(
          "Loudness boost", c, [key] { return std::to_string(profile(key).boostDb); },
          [key](const std::string& v) {
            profile(key).boostDb = atoi(v.c_str());
            changed();
          },
          [](const std::string& v) {
            return std::string(v == "0" ? "as it comes" : v == "12" ? "the loudest, the least dynamic" : "");
          }));
    };
    rows.push_back(boost);
    Row h;
    h.header = true;
    h.title = "Every output";
    rows.push_back(h);
    Row level;
    level.title = "Volume leveling";
    note_ = levelingNote();
    level.subtitle = note_;
    level.trailing = g_sound.leveling ? "On" : "Off";
    level.onSelect = [this] {
      g_sound.leveling = !g_sound.leveling;
      changed();
      render();
    };
    rows.push_back(level);
    PlayerState ps = g_player.state();
    if (ps.amplifier >= 0) {  // the Y1's own amplifier (player/amplifier.h)
      static const char* const kNames[4] = {"Standard", "+2 dB", "+4 dB", "+6 dB"};
      Row amp;
      amp.title = "Amplifier";
      amp.subtitle = "The Y1's own, for headphones and the speaker";
      amp.trailing = kNames[std::max(0, std::min(3, int(ps.amplifier)))];
      amp.chevron = true;
      amp.onSelect = [] {
        std::vector<std::pair<std::string, std::string>> c;
        for (int i = 0; i < 4; i++) c.push_back({kNames[i], std::to_string(i)});
        g_app->push(makeChoice(
            "Amplifier", c, [] { return std::to_string(g_player.state().amplifier); },
            [](const std::string& v) {
              g_player.amplifier(atoi(v.c_str()), g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1) + "amplifier-shipped.bin");
            },
            [](const std::string& v) {
              return std::string(v == "0" ? "as the Y1 ships, the cleanest" : v == "3" ? "the loudest, more hiss" : "");
            }));
      };
      rows.push_back(amp);
    }
    {  // the highest step the wheel (and the dashboard) may set, on every output
      int maxVol = ps.volumeMax > 0 ? ps.volumeMax : 13;
      int limit = std::max(0, std::min(maxVol, int(ps.volumeLimit)));
      Row lim;
      lim.title = "Volume limit";
      lim.subtitle = "The loudest the wheel goes, on every output";
      lim.trailing = limit >= maxVol ? "No limit" : std::to_string(limit) + " of " + std::to_string(maxVol);
      lim.chevron = true;
      lim.onSelect = [maxVol] {
        std::vector<std::pair<std::string, std::string>> c;
        c.push_back({"No limit", std::to_string(maxVol)});
        for (int v = maxVol - 1; v >= 1; v--) c.push_back({std::to_string(v) + " of " + std::to_string(maxVol), std::to_string(v)});
        g_app->push(makeChoice(
            "Volume limit", c,
            [maxVol] { return std::to_string(std::max(0, std::min(maxVol, int(g_player.state().volumeLimit)))); },
            [](const std::string& v) { g_player.setVolumeLimit(atoi(v.c_str())); },
            [maxVol](const std::string& v) {
              int n = atoi(v.c_str());
              return std::string(n >= maxVol ? "the wheel goes all the way" : n <= maxVol / 3 ? "quiet: kind to ears" : "");
            }));
      };
      rows.push_back(lim);
      limit_ = limit;
    }
    Row ph;
    ph.header = true;
    ph.title = "Playback";
    rows.push_back(ph);
    Row xf;
    xf.title = "Crossfade";
    xf.subtitle = "Songs fade into each other";
    xf.trailing = g_prefs.crossfadeSec ? std::to_string(g_prefs.crossfadeSec) + " s" : "Off";
    xf.chevron = true;
    xf.onSelect = [] {
      std::vector<std::pair<std::string, std::string>> c = {{"Off", "0"}};
      for (int sec : {2, 4, 6, 8, 10, 12}) c.push_back({std::to_string(sec) + " seconds", std::to_string(sec)});
      g_app->push(makeChoice(
          "Crossfade", c, [] { return std::to_string(g_prefs.crossfadeSec); },
          [](const std::string& v) {
            g_prefs.crossfadeSec = atoi(v.c_str());
            savePrefsNow();
            g_player.playback(g_prefs.crossfadeSec * 1000, g_prefs.fadePause);
          },
          [](const std::string& v) {
            return std::string(v == "0" ? "one after the other, no gap" : v == "6" ? "like a DJ" : "");
          }));
    };
    rows.push_back(xf);
    Row fp;
    fp.title = "Fade on pause and play";
    fp.subtitle = "Music fades out and back in";
    fp.trailing = g_prefs.fadePause ? "On" : "Off";
    fp.onSelect = [this] {
      g_prefs.fadePause = !g_prefs.fadePause;
      savePrefsNow();
      g_player.playback(g_prefs.crossfadeSec * 1000, g_prefs.fadePause);
      render();
    };
    rows.push_back(fp);
    amp_ = ps.amplifier;
    list_.setRows(std::move(rows), true);
  }
  Output out_;
  std::string note_;
  int amp_ = -2, limit_ = -1;
};

}  // namespace

std::unique_ptr<Screen> makeSound() { return std::unique_ptr<Screen>(new SoundScreen()); }

// --- the dashboard's Sound page (UI thread) ---

namespace {
bool validOutput(const std::string& k) { return k == "speaker" || k == "headphones" || (k.size() == 20 && k.compare(0, 3, "bt:") == 0); }
std::string jstr(const std::string& v) {
  std::string o = "\"";
  for (unsigned char c : v) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += char(c);
  }
  return o + "\"";
}
std::string gainsJson(const std::array<float, kEqBands>& g) {
  std::string o = "[";
  for (int i = 0; i < kEqBands; i++) {
    char b[16];
    snprintf(b, sizeof(b), "%s%.1f", i ? "," : "", double(g[i]));
    o += b;
  }
  return o + "]";
}
}  // namespace

std::string soundJson() {
  load();
  Output cur = currentOutput();
  // every output Rondo knows: the speaker, wired headphones, the paired Bluetooth devices, any kept
  std::vector<Output> outs = {{"speaker", "Speaker"}, {"headphones", "Headphones"}};
  CoreState cs = g_core.state();
  for (int i = 0; i < cs.btCount; i++)
    if (cs.bt[i].paired)  // one that never told its name: by the end of its address
      outs.push_back({std::string("bt:") + cs.bt[i].address,
                      cs.bt[i].name[0] ? btName(cs.bt[i]) : "Bluetooth device \xc2\xb7 " + std::string(cs.bt[i].address).substr(12)});
  for (const auto& kv : g_sound.outputs) {
    bool known = false;
    for (const Output& o : outs) known |= o.key == kv.first;
    if (!known && validOutput(kv.first)) outs.push_back({kv.first, "Bluetooth " + kv.first.substr(3)});
  }
  if (cur.key.compare(0, 3, "bt:") == 0) {
    bool known = false;
    for (const Output& o : outs) known |= o.key == cur.key;
    if (!known) outs.push_back(cur);
  }
  std::string o = "{\"current\":" + jstr(cur.key) + ",\"leveling\":" + (g_sound.leveling ? "true" : "false") +
                  ",\"levelingNote\":" + jstr(levelingNote()) + ",\"freqs\":[";
  for (int i = 0; i < kEqBands; i++) o += (i ? "," : "") + std::to_string(int(kEqFreqs[i]));
  o += "],\"presets\":[";
  bool first = true;
  for (const EqPreset& e : eqPresets()) {
    SoundProfile probe;
    probe.preset = e.id;
    o += std::string(first ? "" : ",") + "{\"id\":" + jstr(e.id) + ",\"name\":" + jstr(e.name) + ",\"note\":" + jstr(e.note) +
         ",\"gains\":" + gainsJson(presetGains(g_sound, probe)) + "}";
    first = false;
  }
  o += "],\"userPresets\":[";
  first = true;
  for (const UserPreset& u : g_sound.userPresets) {
    o += std::string(first ? "" : ",") + "{\"name\":" + jstr(u.name) + ",\"gains\":" + gainsJson(u.gains) + "}";
    first = false;
  }
  o += "],\"outputs\":[";
  first = true;
  for (const Output& out : outs) {
    auto it = g_sound.outputs.find(out.key);
    SoundProfile p = it == g_sound.outputs.end() ? SoundProfile() : it->second;
    o += std::string(first ? "" : ",") + "{\"key\":" + jstr(out.key) + ",\"label\":" + jstr(out.label) +
         ",\"preset\":" + jstr(p.preset) + ",\"presetName\":" + jstr(presetName(g_sound, p)) +
         ",\"gains\":" + gainsJson(presetGains(g_sound, p)) + ",\"custom\":" + gainsJson(p.custom) +
         ",\"boostDb\":" + std::to_string(p.boostDb) + ",\"match\":" +
         (p.autoEq.empty() ? std::string("null") : "{\"name\":" + jstr(p.autoEqName) + ",\"source\":" + jstr(p.autoEqSource) + "}") + "}";
    first = false;
  }
  return o + "]}";
}

bool soundSetPreset(const std::string& output, const std::string& preset) {
  load();
  if (!validOutput(output)) return false;
  bool ok = preset == "custom" || (preset == "autoeq" && !profile(output).autoEq.empty());
  for (const EqPreset& e : eqPresets()) ok |= preset == e.id;
  for (const UserPreset& u : g_sound.userPresets) ok |= preset == "user:" + u.name;
  if (!ok) return false;
  profile(output).preset = preset;
  changed();
  return true;
}

bool soundSetCustom(const std::string& output, const std::array<float, kEqBands>& gains) {
  load();
  if (!validOutput(output)) return false;
  SoundProfile& p = profile(output);
  for (int i = 0; i < kEqBands; i++) p.custom[i] = std::max(-12.f, std::min(12.f, gains[i]));
  p.preset = "custom";
  changed();
  return true;
}

bool soundSetBoost(const std::string& output, int db) {
  load();
  if (!validOutput(output) || (db != 0 && db != 3 && db != 6 && db != 9 && db != 12)) return false;
  profile(output).boostDb = db;
  changed();
  return true;
}

void soundSetLeveling(bool on) {
  load();
  g_sound.leveling = on;
  changed();
}

// "" when saved, else why not
std::string soundSaveUserPreset(const std::string& output, const std::string& name, const std::array<float, kEqBands>& gains) {
  load();
  if (name.empty() || name.size() > 30) return "Type a name of up to 30 characters";
  for (const EqPreset& e : eqPresets())
    if (name == e.name) return "That's a built-in preset's name";
  std::array<float, kEqBands> g;
  for (int i = 0; i < kEqBands; i++) g[i] = std::max(-12.f, std::min(12.f, gains[i]));
  bool found = false;
  for (UserPreset& u : g_sound.userPresets)
    if (u.name == name) u.gains = g, found = true;
  if (!found) g_sound.userPresets.push_back({name, g});
  if (validOutput(output)) profile(output).preset = "user:" + name;
  changed();
  saveNow();
  return "";
}

bool soundDeleteUserPreset(const std::string& name) {
  load();
  auto& v = g_sound.userPresets;
  size_t before = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const UserPreset& u) { return u.name == name; }), v.end());
  if (v.size() == before) return false;
  for (auto& kv : g_sound.outputs)  // an output on it goes back to its own bands
    if (kv.second.preset == "user:" + name) kv.second.preset = "flat";
  changed();
  saveNow();
  return true;
}

bool soundSetMatch(const std::string& output, const std::string& presetBody, const std::string& name, const std::string& source) {
  load();
  if (!validOutput(output)) return false;
  SoundProfile p = profile(output);
  if (!parseAutoEq(presetBody, p)) return false;
  p.autoEqName = name, p.autoEqSource = source, p.preset = "autoeq";
  profile(output) = p;
  changed();
  saveNow();
  return true;
}

// From the loop: the output in use changing brings its sound; a change is written a second later
void soundTick() {
  apply();
  leveling(now_ns());
  if (g_dirtyAt && now_ns() - g_dirtyAt > 1000 * kMs) saveNow();
}

std::string soundState() {
  load();
  Output o = currentOutput();
  auto it = g_sound.outputs.find(o.key);
  SoundProfile p = it == g_sound.outputs.end() ? SoundProfile() : it->second;
  std::string s = presetName(g_sound, p);
  if (p.boostDb) s += " \xc2\xb7 +" + std::to_string(p.boostDb) + " dB";
  return s;
}

}  // namespace rondo::shell
