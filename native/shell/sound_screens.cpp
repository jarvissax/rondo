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
    if (v == "headphones") return {"headphones", tr("sound.output.headphones")};
  }
  CoreState s = g_core.state();
  for (int i = 0; i < s.btCount; i++)
    if (s.bt[i].a2dp == 2) return {std::string("bt:") + s.bt[i].address, btName(s.bt[i])};
  if (power::headset() != 0) return {"headphones", tr("sound.output.headphones")};
  return {"speaker", tr("sound.output.speaker")};
}

// a user preset may not take a built-in one's name, in the language spoken or in English
bool builtInName(const std::string& name) {
  for (const EqPreset& e : eqPresets())
    if (name == tr(e.name) || name == lang::trIn("en", e.name)) return true;
  return false;
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
  if (!g_sound.leveling || s.loudnessTotal <= 0) return tr("sound.leveling.note");
  lang::Args n = {{"known", s.loudnessKnown}, {"count", s.loudnessTotal}};
  if (s.loudnessKnown >= s.loudnessTotal) return tr("sound.leveling.measured", n);
  return power::battery().plugged ? tr("sound.leveling.measuring", n) : tr("sound.leveling.restWhileCharging", n);
}

std::string boostName(int db) { return db ? "+" + std::to_string(db) + " dB" : tr("common.off"); }

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
  std::string title() const override { return tr("sound.equalizer"); }
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
    // a slider a band, as on the dashboard: its gain over it, its frequency under it, ±12 dB
    Label hint = label(e, tr("sound.eq.hint"), 9, false, w);
    Label freq = label(e, "16k", 10, false), gainL = label(e, "+12", 11, true);
    float gTop = top + where.h + e.dp(8) + gainL.h + e.dp(5), gBottom = h - hint.h - e.dp(8) - freq.h - e.dp(7);
    float gh = gBottom - gTop, colW = (w - 2 * pad) / kEqBands;
    float knob = float(e.dp(7)), span = gh - 2 * knob;  // the knob stays inside its track
    auto yOf = [&](float db) { return gTop + knob + (12.f - db) / 24.f * span; };
    // the curve the filters make together, faintly behind the sliders
    SoundSettings s = resolveSound(g_sound, profile(out_.key), out_.key == "speaker");
    e.c.layer(1);
    float x0 = float(pad) + colW / 2, x1 = w - pad - colW / 2;
    for (float x = x0; x <= x1; x += 2) {
      float hz = 31.f * powf(2.f, (x - x0) / (x1 - x0) * 9.f), db = 0;
      for (const Filter& f : s.filters) db += Dsp::responseDb(f, hz);
      db = std::max(-12.f, std::min(12.f, db));
      e.c.shape({x - 1, yOf(db) - 1, 2, 2}, 1, withAlpha(p.accent, 0.3f));
    }
    for (int i = 0; i < kEqBands; i++) {
      float cx = float(pad) + colW * (float(i) + 0.5f), gy = yOf(gains_[size_t(i)]);
      bool picked = i == band_;
      float tw = float(e.dp(picked ? 10 : 8));
      // the track; the band picked filled in its color from 0 dB to the knob
      e.c.layer(0);
      e.c.shape({cx - tw / 2, gTop, tw, gh}, tw / 2, picked ? withAlpha(p.accent, 0.22f) : p.rowLine);
      float zero = yOf(0), y0 = std::min(zero, gy), y1 = std::max(zero, gy);
      if (picked && y1 - y0 >= 1) {
        e.c.layer(1);
        e.c.shape({cx - tw / 2, y0, tw, y1 - y0}, tw / 2, withAlpha(p.accent, 0.75f));
      }
      e.c.layer(2);
      float k = picked ? knob + e.dpf(1) : knob;
      if (picked) e.c.shape({cx - k - 2, gy - k - 2, 2 * k + 4, 2 * k + 4}, k + 2, p.accent);
      e.c.shape({cx - k, gy - k, 2 * k, 2 * k}, k, 0xffffffff);
      int gdb = int(roundf(gains_[size_t(i)]));
      Label gl = label(e, (gdb > 0 ? "+" : "") + std::to_string(gdb), 11, picked, colW);
      drawLabel(e, gl, cx - gl.w / 2.f, gTop - e.dp(5) - gl.h, picked ? p.text : p.muted);
      Label fl = label(e, freqName(kEqFreqs[i]), 10, picked, colW);
      drawLabel(e, fl, cx - fl.w / 2.f, gBottom + e.dp(7), picked ? p.text : p.muted);
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
        tr("sound.eq.nameTitle"), "", tr("common.save"), tr("sound.eq.nameNote"), 30, nullptr,
        [](const std::string& name, std::function<void(const std::string&)> answer) {
          if (name.empty()) return answer(tr("common.typeName"));
          if (builtInName(name)) return answer(tr("sound.eq.builtInName"));
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
  explicit HeadphoneMatchScreen(Output o) : ShellListScreen(tr("sound.match.title")), out_(std::move(o)) {
    if (out_.key.compare(0, 3, "bt:") == 0) lookUp(out_.label);
    else render();
  }
  ~HeadphoneMatchScreen() override { *alive_ = false; }

 private:
  struct Found {
    std::string id, name, source;
  };
  void lookUp(const std::string& q) {
    query_ = q, state_ = tr("sound.match.looking"), found_.clear();
    render();
    auto alive = alive_;
    g_core.get("/eq/search?q=" + Api::escape(q), 60000, [this, alive](int status, const std::string& body) {
      if (!*alive) return;
      found_.clear();
      if (status == 404) state_ = tr("sound.match.serverTooOld");
      else if (status != 200) state_ = tr("common.serverNoAnswer");
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
        state_ = found_.empty() ? tr("sound.match.noModel") : "";
      }
      fresh_ = true;  // new answers: the first of them picked
      render();
    });
  }
  void pick(Found f) {  // a copy: the row holding it goes as the list is drawn again
    state_ = tr("sound.match.getting", {{"name", f.name}}), found_.clear();
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
      state_ = status == 0 ? tr("common.serverNoAnswer") : tr("sound.match.noCorrection");
      render();
    });
  }
  void render() {
    std::vector<Row> rows;
    const SoundProfile& p = profile(out_.key);
    if (!p.autoEq.empty()) {
      Row now;
      now.title = p.autoEqName;
      now.subtitle = tr("sound.match.inUse", {{"source", p.autoEqSource}});
      now.trailing = p.preset == "autoeq" ? tr("common.current") : "";
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
      h.title = tr("sound.match.forQuery", {{"query", query_}});
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
      r.subtitle = tr("sound.match.measuredBy", {{"source", f.source}});
      r.onSelect = [this, f] { pick(f); };
      rows.push_back(r);
    }
    Row search;
    search.title = tr("sound.match.search");
    search.subtitle = tr("sound.match.searchNote");
    search.chevron = true;
    auto alive = alive_;
    search.onSelect = [this, alive] {
      g_app->push(makeTextEntry(
          tr("sound.match.model"), query_, tr("sound.match.searchAction"), tr("sound.match.modelNote"), 60, nullptr,
          [](const std::string& q, std::function<void(const std::string&)> answer) { answer(q.empty() ? tr("sound.match.typeModel") : ""); },
          [this, alive](const std::string& q) {
            if (*alive) lookUp(q);
          }, false));
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
  SoundScreen() : ShellListScreen(tr("sound.title")) { render(); }
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
    on.title = tr("sound.playingOn");
    on.subtitle = tr("sound.playingOnNote");
    on.trailing = out_.label;
    on.tile = &tiles::output;
    on.enabled = false;
    rows.push_back(on);
    Row eq;
    eq.title = tr("sound.equalizer");
    eq.tile = &tiles::eqPreset;
    eq.trailing = presetName(g_sound, p);
    eq.chevron = true;
    eq.onSelect = [key] {
      std::vector<std::pair<std::string, std::string>> c;
      for (const EqPreset& e : eqPresets()) c.push_back({tr(e.name), e.id});
      for (const UserPreset& u : g_sound.userPresets) c.push_back({u.name, "user:" + u.name});
      if (!profile(key).autoEq.empty()) c.push_back({profile(key).autoEqName, "autoeq"});
      c.push_back({tr("sound.preset.custom"), "custom"});
      g_app->push(makeChoice(
          tr("sound.equalizer"), c, [key] { return profile(key).preset; },
          [key](const std::string& v) {
            profile(key).preset = v;
            changed();
          },
          [](const std::string& v) {
            for (const EqPreset& e : eqPresets())
              if (v == e.id) return e.note[0] ? tr(e.note) : std::string();
            return v == "custom" ? tr("sound.presetNote.custom") : v == "autoeq" ? tr("sound.presetNote.autoEq") : std::string();
          }));
    };
    rows.push_back(eq);
    Row edit;
    edit.title = tr("sound.editEq");
    edit.tile = &tiles::eqEdit;
    edit.subtitle = tr("sound.editEqNote");
    edit.chevron = true;
    Output o = out_;
    edit.onSelect = [o] { g_app->push(std::unique_ptr<Screen>(new EqualizerScreen(o))); };
    rows.push_back(edit);
    if (key != "speaker") {  // headphones, wired or Bluetooth
      Row match;
      match.title = tr("sound.match.title");
      match.tile = &tiles::match;
      match.subtitle = p.autoEq.empty() ? tr("sound.match.rowNote") : p.autoEqName;
      match.chevron = true;
      match.onSelect = [o] { g_app->push(std::unique_ptr<Screen>(new HeadphoneMatchScreen(o))); };
      rows.push_back(match);
    }
    Row boost;
    boost.title = tr("sound.boost.title");
    boost.tile = &tiles::boost;
    boost.subtitle = tr("sound.boost.note");
    boost.trailing = boostName(p.boostDb);
    boost.chevron = true;
    boost.onSelect = [key] {
      std::vector<std::pair<std::string, std::string>> c;
      for (int db : {0, 3, 6, 9, 12}) c.push_back({boostName(db), std::to_string(db)});
      g_app->push(makeChoice(
          tr("sound.boost.title"), c, [key] { return std::to_string(profile(key).boostDb); },
          [key](const std::string& v) {
            profile(key).boostDb = atoi(v.c_str());
            changed();
          },
          [](const std::string& v) {
            return v == "0" ? tr("sound.asItComes") : v == "12" ? tr("sound.boost.noteMax") : std::string();
          }));
    };
    rows.push_back(boost);
    Row h;
    h.header = true;
    h.title = tr("sound.everyOutput");
    rows.push_back(h);
    Row level;
    level.title = tr("sound.leveling.title");
    level.tile = &tiles::leveling;
    note_ = levelingNote();
    level.subtitle = note_;
    level.trailing = g_sound.leveling ? tr("common.on") : tr("common.off");
    level.onSelect = [this] {
      g_sound.leveling = !g_sound.leveling;
      changed();
      render();
    };
    rows.push_back(level);
    PlayerState ps = g_player.state();
    if (ps.amplifier >= 0) {  // the Y1's own amplifier (player/amplifier.h)
      // the amplifier's steps: Standard, +2 dB, +4 dB, +6 dB
      auto stepName = [](int i) { return i ? "+" + std::to_string(2 * i) + " dB" : tr("sound.amp.standard"); };
      Row amp;
      amp.title = tr("sound.amp.title");
      amp.tile = &tiles::amplifier;
      amp.subtitle = tr("sound.amp.note");
      amp.trailing = stepName(std::max(0, std::min(3, int(ps.amplifier))));
      amp.chevron = true;
      amp.onSelect = [stepName] {
        std::vector<std::pair<std::string, std::string>> c;
        for (int i = 0; i < 4; i++) c.push_back({stepName(i), std::to_string(i)});
        g_app->push(makeChoice(
            tr("sound.amp.title"), c, [] { return std::to_string(g_player.state().amplifier); },
            [](const std::string& v) {
              g_player.amplifier(atoi(v.c_str()), g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1) + "amplifier-shipped.bin");
            },
            [](const std::string& v) {
              return v == "0" ? tr("sound.amp.noteStandard") : v == "3" ? tr("sound.amp.noteMax") : std::string();
            }));
      };
      rows.push_back(amp);
    }
    {  // the highest step the wheel (and the dashboard) may set, on every output
      int maxVol = ps.volumeMax > 0 ? ps.volumeMax : 13;
      int limit = std::max(0, std::min(maxVol, int(ps.volumeLimit)));
      Row lim;
      lim.title = tr("sound.limit.title");
      lim.tile = &tiles::limit;
      lim.subtitle = tr("sound.limit.note");
      lim.trailing = limit >= maxVol ? tr("sound.limit.none") : tr("sound.limit.step", {{"step", limit}, {"max", maxVol}});
      lim.chevron = true;
      lim.onSelect = [maxVol] {
        std::vector<std::pair<std::string, std::string>> c;
        c.push_back({tr("sound.limit.none"), std::to_string(maxVol)});
        for (int v = maxVol - 1; v >= 1; v--) c.push_back({tr("sound.limit.step", {{"step", v}, {"max", maxVol}}), std::to_string(v)});
        g_app->push(makeChoice(
            tr("sound.limit.title"), c,
            [maxVol] { return std::to_string(std::max(0, std::min(maxVol, int(g_player.state().volumeLimit)))); },
            [](const std::string& v) { g_player.setVolumeLimit(atoi(v.c_str())); },
            [maxVol](const std::string& v) {
              int n = atoi(v.c_str());
              return n >= maxVol ? tr("sound.limit.noteNone") : n <= maxVol / 3 ? tr("sound.limit.noteQuiet") : std::string();
            }));
      };
      rows.push_back(lim);
      limit_ = limit;
    }
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
  std::vector<Output> outs = {{"speaker", tr("sound.output.speaker")}, {"headphones", tr("sound.output.headphones")}};
  CoreState cs = g_core.state();
  for (int i = 0; i < cs.btCount; i++)
    if (cs.bt[i].paired)  // one that never told its name: by the end of its address
      outs.push_back({std::string("bt:") + cs.bt[i].address,
                      cs.bt[i].name[0] ? btName(cs.bt[i]) : tr("sound.output.btDevice", {{"address", std::string(cs.bt[i].address).substr(12)}})});
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
    o += std::string(first ? "" : ",") + "{\"id\":" + jstr(e.id) + ",\"name\":" + jstr(tr(e.name)) + ",\"note\":" + jstr(e.note[0] ? tr(e.note) : "") +
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
  if (name.empty() || name.size() > 30) return tr("sound.eq.nameTooLong");
  if (builtInName(name)) return tr("sound.eq.builtInName");
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
