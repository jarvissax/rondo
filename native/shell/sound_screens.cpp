// Settings › Sound (new in 2.0, Emin's request of Nov 7: the stock Y1 is quiet and flat): an
// equalizer, a loudness boost and volume leveling, each output keeping its own (the speaker, wired
// headphones, every Bluetooth device). rondo-player does the work (player/dsp.h); this sends it what
// the output in use needs, again whenever that output changes.
#include <math.h>
#include <stdio.h>

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
    if (o.key == out_.key && note == note_) return false;
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
    on.tile = &tiles::equalizer;
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
            return std::string(v == "custom" ? "your own bands" : "");
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
    list_.setRows(std::move(rows), true);
  }
  Output out_;
  std::string note_;
};

}  // namespace

std::unique_ptr<Screen> makeSound() { return std::unique_ptr<Screen>(new SoundScreen()); }

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
