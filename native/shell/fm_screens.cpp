// FM radio (FmScreens.kt in 1.0.x), ported 1:1 over rondo-player's radio (player/fm.h): the dial screen
// laid out like Now Playing, its options (stations, scan, save, output, region, off), the stations
// (saved, then the last scan's) and a saved station's rename, order and removal.
//
// The wheel is the volume, as on Now Playing; center switches it to tuning (0.1 MHz a step) and back;
// Next/Prev seek to the next station, held they fine-tune; Play/Pause mutes; hold center for the
// options; Menu leaves (the radio keeps playing). Opening the screen turns the radio on and pauses music.
#include <stdio.h>

#include <algorithm>

#include "app.h"
#include "screens.h"

namespace rondo::shell {

namespace {

constexpr Color white = 0xffffffff;
inline Color whiteA(int a) { return uint32_t(a) << 24 | 0xffffff; }

bool japan() { return g_config.radio.band == "japan"; }
int bandMin() { return japan() ? 760 : 875; }
int bandMax() { return japan() ? 950 : 1080; }

const Preset* presetAt(int f) {
  for (const Preset& p : g_config.radio.presets)
    if (p.frequency == f) return &p;
  return nullptr;
}
std::string heardName(int f) {
  auto it = g_config.radio.names.find(f);
  return it == g_config.radio.names.end() ? "" : it->second;
}

// the frequency is the UI's (as FmRadio.state was 1.0.x's), kept in config.json like 1.0.x's
// remember(); a turn of the wheel writes it a moment later, not on every step
int64_t g_radioDirtyAt = 0;
bool g_seekAsked = false;
void remember(int f) {
  if (g_config.radio.frequency == f) return;
  g_config.radio.frequency = f;
  g_radioDirtyAt = g_app->now();
}
void tune(int f) {
  f = std::max(bandMin(), std::min(bandMax(), f));
  remember(f);
  g_player.fmTune(f);
}
// the wheel: ±0.1 MHz a step, wrapping at the band's ends
void step(int steps) {
  int span = bandMax() - bandMin() + 1;
  tune(bandMin() + ((g_config.radio.frequency - bandMin() + steps) % span + span) % span);
}
uint32_t g_seekFrom = 0;  // the seek serial when a seek was asked for
void seek(bool up) {
  PlayerState s = g_player.state();
  if (!s.fmOn || s.fmSeeking) return;
  g_seekAsked = true;
  g_seekFrom = s.fmSeekSerial;
  g_player.fmSeek(up, bandMin(), bandMax());
}
void startRadio() {
  if (g_player.state().playing) g_player.pause();
  g_player.fmStart(g_config.radio.frequency, g_prefs.fmVolume, g_config.radio.speaker);
}
void saveRadio() {
  g_radioDirtyAt = 0;
  saveConfigNow();
}

// a list that follows the radio: drawn again when its state changes
class RadioList : public ShellListScreen {
 public:
  using ShellListScreen::ShellListScreen;
  bool tick(int64_t now) override {
    PlayerState s = g_player.state();
    std::string sig = signature(s);
    if (sig == sig_) return false;
    sig_ = sig;
    render(s);
    return true;
  }
  void onShow() override { render(g_player.state()); }

 protected:
  virtual void render(const PlayerState& s) = 0;
  static std::string signature(const PlayerState& s) {
    char b[96];
    snprintf(b, sizeof(b), "%d%d%d%d%d %d %d %u", s.fmOn, s.fmStarting, s.fmScanning, s.fmSpeaker, s.fmMuted, s.fmFrequency,
             g_config.radio.frequency, s.fmScanSerial);
    return std::string(b) + s.fmName;
  }
  std::string sig_;
};

std::unique_ptr<Screen> makeStations();

// A saved station's options: the dashboard's rename, reorder and delete.
class FmPresetScreen : public RadioList {
 public:
  explicit FmPresetScreen(int f) : RadioList(fmName(f) + " MHz"), f_(f) { render(g_player.state()); }

 protected:
  void render(const PlayerState&) override {
    auto& presets = g_config.radio.presets;
    int at = -1;
    for (size_t i = 0; i < presets.size(); i++)
      if (presets[i].frequency == f_) at = int(i);
    if (at < 0) return list_.setMessage("Not saved any more");
    std::string name = presets[size_t(at)].name;
    int f = f_;
    std::vector<Row> rows;
    Row rename;
    rename.title = "Rename";
    rename.subtitle = name.empty() ? "No name yet" : name;
    rename.chevron = true;
    rename.onSelect = [f, name] {
      g_app->push(makeTextEntry("Name for " + fmName(f) + " MHz", name.empty() ? heardName(f) : name, "Save", 40,
                                [f](const std::string& typed) {
                                  for (Preset& p : g_config.radio.presets)
                                    if (p.frequency == f) p.name = typed;
                                  saveRadio();
                                  return std::string();
                                },
                                nullptr));
    };
    rows.push_back(rename);
    Row up;
    up.title = "Move up";
    up.enabled = at > 0;
    up.onSelect = [this, at] { move(at, at - 1); };
    rows.push_back(up);
    Row down;
    down.title = "Move down";
    down.enabled = at < int(presets.size()) - 1;
    down.onSelect = [this, at] { move(at, at + 1); };
    rows.push_back(down);
    Row remove;
    remove.title = "Remove from saved";
    remove.tile = &tiles::del;
    remove.onSelect = [f] {
      auto& ps = g_config.radio.presets;
      ps.erase(std::remove_if(ps.begin(), ps.end(), [f](const Preset& p) { return p.frequency == f; }), ps.end());
      saveRadio();
      g_app->pop();
    };
    rows.push_back(remove);
    list_.setRows(std::move(rows), true);
  }

 private:
  void move(int from, int to) {
    auto& ps = g_config.radio.presets;
    Preset p = ps[size_t(from)];
    ps.erase(ps.begin() + from);
    ps.insert(ps.begin() + to, p);
    saveRadio();
    render(g_player.state());
  }
  int f_;
};

// Saved stations first, then what the last scan found; center tunes and goes back to the radio.
// Hold center: rename, move or remove a saved station, or save a found one (like the dashboard).
class FmStationsScreen : public RadioList {
 public:
  FmStationsScreen() : RadioList("Stations") { render(g_player.state()); }

 protected:
  void render(const PlayerState& s) override {
    const Radio& r = g_config.radio;
    std::vector<Row> rows;
    auto header = [&](const char* t) {
      Row h;
      h.header = true;
      h.title = t;
      rows.push_back(h);
    };
    auto pick = [](int f) {
      tune(f);
      g_app->popTo("Radio");
    };
    header("Saved");
    if (r.presets.empty()) {
      Row e;
      e.title = "No saved stations";
      e.subtitle = "Hold center on the radio, then Save";
      e.enabled = false;
      rows.push_back(e);
    }
    for (const Preset& p : r.presets) {
      Row row;
      int f = p.frequency;
      row.title = fmName(f) + " MHz";
      row.subtitle = p.name.empty() ? heardName(f) : p.name;
      row.trailing = f == r.frequency ? "Playing" : "";
      row.onSelect = [pick, f] { pick(f); };
      row.onLongSelect = [f] { g_app->push(std::unique_ptr<Screen>(new FmPresetScreen(f))); };
      rows.push_back(row);
    }
    header("Found");
    if (r.found.empty()) {
      Row e;
      e.title = "Not scanned yet";
      e.subtitle = "Scan for stations in the radio's options";
      e.enabled = false;
      rows.push_back(e);
    }
    for (int f : r.found) {
      if (presetAt(f)) continue;
      Row row;
      row.title = fmName(f) + " MHz";
      row.subtitle = heardName(f);
      row.trailing = f == r.frequency ? "Playing" : "hold center: save";
      row.onSelect = [pick, f] { pick(f); };
      row.onLongSelect = [this, f] {
        std::string n = heardName(f);
        if (n.size() > 40) n.resize(40);
        g_config.radio.presets.push_back({f, n});
        saveRadio();
        render(g_player.state());
      };
      rows.push_back(row);
    }
    list_.setRows(std::move(rows), true);
  }
};

std::unique_ptr<Screen> makeStations() { return std::unique_ptr<Screen>(new FmStationsScreen()); }

// Hold center on the radio: stations, scan, save, output, off
class FmOptionsScreen : public RadioList {
 public:
  FmOptionsScreen() : RadioList("FM Radio") { render(g_player.state()); }
  bool tick(int64_t now) override {
    PlayerState s = g_player.state();
    // the scan asked for here has finished: its stations
    if (scanAsked_ && s.fmScanSerial != scanSerial_ && !s.fmScanning) {
      scanAsked_ = false;
      g_app->push(makeStations());
      return true;
    }
    return RadioList::tick(now);
  }

 protected:
  void render(const PlayerState& s) override {
    int f = g_config.radio.frequency;
    bool saved = presetAt(f) != nullptr;
    std::vector<Row> rows;
    Row stations;
    stations.title = "Stations";
    stations.trailing = std::to_string(g_config.radio.presets.size()) + " saved";
    stations.chevron = true;
    stations.onSelect = [] { g_app->push(makeStations()); };
    rows.push_back(stations);
    Row scan;
    scan.title = s.fmScanning ? "Scanning\xe2\x80\xa6" : "Scan for stations";
    scan.subtitle = s.fmScanning ? "" : "Finds every station you can receive";
    scan.enabled = s.fmOn && !s.fmScanning;
    uint32_t serial = s.fmScanSerial;
    scan.onSelect = [this, serial] {
      scanAsked_ = true;
      scanSerial_ = serial;
      g_player.fmScan(bandMin(), bandMax());
    };
    rows.push_back(scan);
    Row save;
    if (saved) {
      save.title = "Remove " + fmName(f) + " from saved";
      save.onSelect = [this, f] {
        auto& ps = g_config.radio.presets;
        ps.erase(std::remove_if(ps.begin(), ps.end(), [f](const Preset& p) { return p.frequency == f; }), ps.end());
        saveRadio();
        render(g_player.state());
      };
    } else {
      save.title = "Save " + fmName(f);
      save.subtitle = s.fmName;
      std::string name = s.fmName;
      save.onSelect = [this, f, name] {
        auto& ps = g_config.radio.presets;
        ps.push_back({f, name});
        std::stable_sort(ps.begin(), ps.end(), [](const Preset& a, const Preset& b) { return a.frequency < b.frequency; });
        saveRadio();
        render(g_player.state());
      };
    }
    rows.push_back(save);
    Row output;
    output.title = "Output";
    output.trailing = g_config.radio.speaker ? "Speaker" : "Headphones";
    output.onSelect = [this] {
      g_config.radio.speaker = !g_config.radio.speaker;
      saveRadio();
      g_player.fmSpeaker(g_config.radio.speaker);
      render(g_player.state());
    };
    rows.push_back(output);
    Row region;
    region.title = "Region";
    region.trailing = japan() ? "Japan" : "Most of the world";
    region.onSelect = [this] {
      g_config.radio.band = japan() ? "world" : "japan";
      g_config.radio.found.clear();
      saveRadio();
      tune(g_config.radio.frequency);
      render(g_player.state());
    };
    rows.push_back(region);
    Row power;
    bool on = s.fmOn || s.fmStarting;
    power.title = on ? "Turn radio off" : "Turn radio on";
    power.onSelect = [on] {
      if (on) {
        g_player.fmStop();
        g_app->pop();  // back to the (now quiet) radio screen
      } else {
        startRadio();
      }
    };
    rows.push_back(power);
    list_.setRows(std::move(rows), true);
  }

 private:
  bool scanAsked_ = false;
  uint32_t scanSerial_ = 0;
};

class FmScreen : public Screen {
 public:
  std::string title() const override { return "Radio"; }
  bool statusBar() const override { return false; }
  void onShow() override {
    PlayerState s = g_player.state();
    if (!s.fmOn && !s.fmStarting) startRadio();
  }
  void onHide() override { tuning_ = false; }

  bool onAction(Action a, int steps) override {
    PlayerState s = g_player.state();
    switch (a) {
      case Action::Up:
      case Action::Down: {
        int dir = a == Action::Down ? 1 : -1;
        if (tuning_ && s.fmOn) step(dir * steps);
        else g_app->changeVolume(dir);
        break;
      }
      case Action::Select: tuning_ = !tuning_ && s.fmOn; break;
      case Action::LongSelect:
        tuning_ = false;
        g_app->push(std::unique_ptr<Screen>(new FmOptionsScreen()));
        break;
      case Action::Next:
      case Action::Prev: seek(a == Action::Next); break;
      case Action::LongNext:
      case Action::LongPrev: step(a == Action::LongNext ? 1 : -1); break;
      case Action::SeekEnd: break;
      case Action::PlayPause:
        if (!s.fmOn && !s.fmStarting) startRadio();
        else if (s.fmOn) g_player.fmMute(!s.fmMuted);
        break;
      case Action::Back:
        if (!tuning_) return false;
        tuning_ = false;
        break;
    }
    return true;
  }
  bool tick(int64_t now) override {
    PlayerState s = g_player.state();
    char b[96];
    snprintf(b, sizeof(b), "%d%d%d%d%d%d%d%d %d %d %d", s.fmOn, s.fmStarting, s.fmMuted, s.fmSeeking, s.fmScanning, s.fmStereo,
             s.fmSignal, s.fmError, s.fmFrequency, g_config.radio.frequency, tuning_);
    std::string sig = std::string(b) + s.fmName + "\n" + s.fmText;
    if (sig == sig_) return false;
    sig_ = sig;
    return true;
  }

  void draw(Env& e, float w, float h) override {
    PlayerState s = g_player.state();
    // the background: warm at the top, into the dark sections' color (a three-stop gradient)
    e.c.layer(0);
    e.c.fillGradient({0, 0, w, h / 2}, rgb(0x3a2a12), rgb(0x18140e));
    e.c.fillGradient({0, h / 2, w, h - h / 2}, rgb(0x18140e), e.p.dark);
    // the header
    int headH = e.dp(24);
    e.c.layer(2);
    Label head = label(e, "FM RADIO", 11, true);
    drawLabel(e, head, float(e.dp(12)), float((headH - head.h) / 2), whiteA(200));
    statusIcons(e, w - e.dp(10), headH, whiteA(200), true);
    // the bottom bar, the dial above it, the center between
    int dialH = e.dp(40), bottomH = e.dp(26);
    float dialY = h - bottomH - dialH;
    drawCenter(e, w, float(headH), dialY, s);
    drawDial(e, w, dialY, float(dialH));
    drawBottom(e, w, h - bottomH, float(bottomH), s);
    e.c.layer(0);
  }

 private:
  void drawCenter(Env& e, float w, float top, float bottom, const PlayerState& s) {
    int f = g_config.radio.frequency;
    int side = e.dp(14);
    float iw = w - 2 * side;
    Label freq = label(e, fmName(f), 44, true), unit = label(e, "MHz", 14, true);
    int rowW = freq.w + e.dp(6) + unit.w;
    int rowH = std::max(freq.h, unit.h + e.dp(9));
    float y = top + e.dp(6);
    float x = float(side) + float((int(iw) - rowW) / 2);
    e.c.layer(2);
    e.c.pushAlpha(s.fmOn ? 1 : 0.45f);
    drawLabel(e, freq, x, y + rowH - freq.h, white);
    e.c.popAlpha();
    drawLabel(e, unit, x + freq.w + e.dp(6), y + rowH - e.dp(9) - unit.h, whiteA(190));
    y += rowH;
    // what is on: an error, the state, or the station's name and radio text
    const Preset* saved = presetAt(f);
    std::string title, sub;
    if (s.fmError == 1) title = "Plug in wired headphones", sub = "Their cable is the radio's antenna.";
    else if (s.fmError == 2) title = "The radio didn't start", sub = "Press Play/Pause to try again.";
    else if (s.fmStarting) title = "Turning on\xe2\x80\xa6";
    else if (!s.fmOn) title = "Radio off", sub = "Press Play/Pause to turn it on.";
    else if (s.fmScanning) title = "Scanning\xe2\x80\xa6", sub = "Finding every station. This takes a little while.";
    else if (s.fmSeeking) title = "Searching\xe2\x80\xa6";
    else {
      title = s.fmName[0] ? s.fmName : saved && !saved->name.empty() ? saved->name : heardName(f);
      sub = s.fmText;
    }
    if (!title.empty()) {
      Label t = label(e, title, 17, true, iw);
      y += e.dp(2);
      drawLabel(e, t, centerText(float(side), iw, t.w), y, white);
      y += t.h;
    }
    if (!sub.empty()) {
      y += e.dp(3);
      for (const Label& l : wrap(e, sub, 12, false, iw, 2)) drawLabel(e, l, centerText(float(side), iw, l.w), y, whiteA(190)), y += l.h;
    }
  }

  // FmDial: the band with a tick every 0.5 MHz, labels every 4 MHz (2 in Japan), saved stations as dots
  // and a needle at the station; brighter while the wheel tunes
  void drawDial(Env& e, float w, float top, float h) {
    float pad = e.dpf(8), dw = w - 2 * pad;
    int mn = bandMin(), mx = bandMax();
    auto x = [&](int f) { return pad + dw * float(f - mn) / float(mx - mn); };
    float base = top + h * 0.62f;
    int alpha = tuning_ ? 230 : 140;
    Color c = whiteA(alpha);
    float line = e.dpf(1);
    e.c.layer(1);
    for (int f = (mn + 4) / 5 * 5; f <= mx; f += 5) {
      bool major = japan() ? f % 20 == 0 : f % 40 == 0;
      bool half = f % 10 != 0;
      float len = e.dpf(major ? 9 : half ? 3 : 5);
      e.c.fill({x(f) - line / 2, base - len, line, len}, c);
      if (major) {
        Label l = label(e, std::to_string(f / 10), 9, false);
        e.c.layer(2);
        drawLabel(e, l, x(f) - float(l.w) / 2, base + e.dpf(11) - e.text.ascentInt(l.font), c);
        e.c.layer(1);
      }
    }
    e.c.fill({pad, base - line / 2, dw, line}, c);
    float dot = e.dpf(2);
    for (const Preset& p : g_config.radio.presets)
      e.c.shape({x(p.frequency) - dot, base - e.dpf(13) - dot, 2 * dot, 2 * dot}, dot, 0xc8ffb04d);
    float nx = x(g_config.radio.frequency), nw = e.dpf(1.5f);
    e.c.layer(2);
    e.c.shape({nx - nw, base - e.dpf(18), 2 * nw, e.dpf(20)}, nw, tuning_ ? rgb(0xffb04d) : e.p.red);
  }

  // the signal bars, stereo or mono and the output, the tuning hint
  void drawBottom(Env& e, float w, float top, float h, const PlayerState& s) {
    int side = e.dp(14);
    float content = h - e.dp(6);
    float x = float(side);
    e.c.layer(2);
    if (s.fmOn) {
      int bw = e.dp(18), bh = e.dp(12);
      float by = top + float((int(content) - bh) / 2);
      float gap = bw / 12.f, barW = (bw - 3 * gap) / 4;
      for (int i = 0; i < 4; i++) {
        float bar = bh * (i + 1) / 4.f;
        e.c.shape({x + i * (barW + gap), by + bh - bar, barW, bar}, gap / 2, i < s.fmSignal ? whiteA(220) : whiteA(60));
      }
      x += bw + e.dp(8);
    }
    std::string info = !s.fmOn ? "" : s.fmMuted ? "Muted"
                                                : std::string(s.fmStereo ? "Stereo" : "Mono") + " \xc2\xb7 " +
                                                      (g_config.radio.speaker ? "Speaker" : "Headphones");
    if (!info.empty()) {
      Label l = label(e, info, 11, false);
      drawLabel(e, l, x, top + float((int(content) - l.h) / 2), whiteA(200));
    }
    if (tuning_) {
      Label l = label(e, "turn to tune \xc2\xb7 center: done", 11, false);
      drawLabel(e, l, w - side - l.w, top + float((int(content) - l.h) / 2), whiteA(170));
    }
  }

  bool tuning_ = false;
  std::string sig_;
};

}  // namespace

std::string fmName(int f) { return std::to_string(f / 10) + "." + std::to_string(f % 10); }

std::unique_ptr<Screen> makeFm() { return std::unique_ptr<Screen>(new FmScreen()); }

// From the loop (FmRadio's remembering in 1.0.x): a seek's station, a scan's stations and the RDS names
// heard go into config.json, and the radio's volume into the prefs.
void fmTick() {
  static uint32_t scanSerial = 0;
  PlayerState s = g_player.state();
  if (g_seekAsked && s.fmSeekSerial != g_seekFrom) {  // the seek has finished: its station
    g_seekAsked = false;
    remember(s.fmFrequency);
  }
  if (s.fmScanSerial != scanSerial) {
    if (scanSerial != 0 || s.fmFoundCount) {
      g_config.radio.found.assign(s.fmFound, s.fmFound + s.fmFoundCount);
      saveRadio();
    }
    scanSerial = s.fmScanSerial;
  }
  if (s.fmOn && s.fmName[0] && s.fmFrequency == g_config.radio.frequency && heardName(s.fmFrequency) != s.fmName) {
    g_config.radio.names[s.fmFrequency] = s.fmName;
    saveRadio();
  }
  if (s.fmVolume >= 0 && s.fmVolume != g_prefs.fmVolume) {
    g_prefs.fmVolume = s.fmVolume;
    savePrefsNow();
  }
  if (g_radioDirtyAt && g_app->now() - g_radioDirtyAt > 1000 * kMs) saveRadio();
}

}  // namespace rondo::shell
