// Bluetooth headphones and speakers from the wheel (BluetoothScreens.kt in 1.0.x), ported 1:1 over
// rondo-core's Bluetooth: on and off, My devices (connect, disconnect; hold center for a device's
// screen: Forget), and the devices found while the screen is shown (pair: connected once paired).
// Searching costs battery and disturbs Bluetooth audio, so it runs only while this screen is in front,
// again 20 s after the last one ended.
#include <algorithm>

#include "app.h"
#include "screens.h"

namespace rondo::shell {

namespace {

// BluetoothClass's major and device classes (Bt.kind in 1.0.x)
bool isAudio(uint32_t cod) { return (cod & 0x1f00) == 0x0400; }
std::string kind(uint32_t cod) {
  switch (cod & 0x1ffc) {
    case 0x0418:  // headphones
    case 0x0404: return "Headphones";  // a wearable headset
    case 0x0408: return "Headset";     // hands-free
    case 0x0414:                       // loudspeaker
    case 0x041c: return "Speaker";     // portable audio
    case 0x0420: return "Car audio";
    default: return isAudio(cod) ? "Audio" : "Other device";
  }
}
std::string nameOf(const BtDevice& d) {
  std::string n = d.name;
  bool blank = n.find_first_not_of(" \t") == std::string::npos;
  return blank ? std::string(d.address) : n;
}
std::string lower(std::string s) {
  for (char& c : s) c = char(tolower((unsigned char)c));
  return s;
}
const BtDevice* deviceAt(const CoreState& s, const std::string& address) {
  for (int i = 0; i < s.btCount; i++)
    if (address == s.bt[i].address) return &s.bt[i];
  return nullptr;
}

// a list that follows rondo-core's Bluetooth state: drawn again whenever it changes
class BluetoothList : public ShellListScreen {
 public:
  using ShellListScreen::ShellListScreen;
  bool tick(int64_t now) override {
    CoreState s = g_core.state();
    bool changed = s.serial != serial_;
    if (changed) serial_ = s.serial, render(s);
    return poll(s, now) || changed;
  }

 protected:
  virtual void render(const CoreState& s) = 0;
  virtual bool poll(const CoreState& s, int64_t now) { return false; }
  uint32_t serial_ = 0;
};

// A paired device: connect / disconnect, forget.
class BluetoothDeviceScreen : public BluetoothList {
 public:
  BluetoothDeviceScreen(const std::string& address, const std::string& name) : BluetoothList(name), address_(address) {
    render(g_core.state());
  }

 protected:
  void render(const CoreState& s) override {
    const BtDevice* d = deviceAt(s, address_);
    bool connected = d && d->a2dp == 2;
    std::string a = address_;
    std::vector<Row> rows;
    Row info;
    info.title = kind(d ? d->cod : 0);
    info.trailing = address_;
    info.enabled = false;
    rows.push_back(info);
    Row c;
    c.title = connected ? "Disconnect" : "Connect";
    c.onSelect = [a, connected] {
      if (connected) g_core.btDisconnect(a);
      else g_core.btConnect(a);
      g_app->pop();
    };
    rows.push_back(c);
    Row f;
    f.title = "Forget this device";
    f.subtitle = "Pair it again to use it";
    f.onSelect = [a] {
      g_core.btForget(a);
      g_app->pop();
    };
    rows.push_back(f);
    list_.setRows(std::move(rows), true);
  }

 private:
  std::string address_;
};

class BluetoothScreen : public BluetoothList {
 public:
  BluetoothScreen() : BluetoothList("Bluetooth") { render(g_core.state()); }
  void onShow() override {
    g_core.btSearch(true);  // BtSession.start(): searching while the screen is in front
    lastSearch_ = g_app->now();
    render(g_core.state());
  }
  void onHide() override { g_core.btSearch(false); }

 protected:
  void render(const CoreState& s) override {
    if (s.btPower < 0) return list_.setMessage("This Y1 has no Bluetooth");
    bool on = s.btPower == 2;
    std::vector<Row> rows;
    Row power;
    power.title = "Bluetooth";
    power.tile = &tiles::bluetooth;
    power.trailing = s.btPower == 1 ? "Turning on\xe2\x80\xa6" : s.btPower == 3 ? "Turning off\xe2\x80\xa6" : on ? "On" : "Off";
    int state = s.btPower;
    power.onSelect = [state, on] {
      // a press while it is still turning is ignored
      if (state != 0 && state != 2) return;
      g_core.btPower(!on);
      g_prefs.bluetoothOn = !on;
      savePrefsNow();
    };
    rows.push_back(power);
    if (on) {
      std::vector<const BtDevice*> bonded, others;
      for (int i = 0; i < s.btCount; i++) (s.bt[i].paired ? bonded : others).push_back(&s.bt[i]);
      // connected first (BluetoothProfile's state, highest first)
      std::stable_sort(bonded.begin(), bonded.end(), [](const BtDevice* a, const BtDevice* b) { return a->a2dp > b->a2dp; });
      std::stable_sort(others.begin(), others.end(), [](const BtDevice* a, const BtDevice* b) {
        if (isAudio(a->cod) != isAudio(b->cod)) return isAudio(a->cod);
        return lower(nameOf(*a)) < lower(nameOf(*b));
      });
      if (!bonded.empty()) rows.push_back(header("My devices"));
      for (const BtDevice* d : bonded) {
        Row r;
        r.title = nameOf(*d);
        r.subtitle = d->a2dp == 2 ? "Connected \xc2\xb7 music plays here"
                     : d->a2dp == 1 ? "Connecting\xe2\x80\xa6"
                     : isAudio(d->cod) ? kind(d->cod) + " \xc2\xb7 not connected"
                                       : kind(d->cod);
        std::string a = d->address, name = nameOf(*d);
        bool connected = d->a2dp == 2;
        r.onSelect = [a, connected] {
          if (connected) g_core.btDisconnect(a);
          else g_core.btConnect(a);
        };
        r.onLongSelect = [a, name] { g_app->push(std::unique_ptr<Screen>(new BluetoothDeviceScreen(a, name))); };
        rows.push_back(std::move(r));
      }
      rows.push_back(header(s.btSearching ? "Other devices \xc2\xb7 searching\xe2\x80\xa6" : "Other devices"));
      if (others.empty()) {
        Row r;
        r.title = s.btSearching ? "Looking for devices\xe2\x80\xa6" : "Search again";
        r.subtitle = "Put your headphones in pairing mode";
        r.enabled = !s.btSearching;
        r.onSelect = [this] {
          g_core.btSearch(true);
          lastSearch_ = g_app->now();
        };
        rows.push_back(std::move(r));
      }
      for (const BtDevice* d : others) {
        Row r;
        r.title = nameOf(*d);
        r.subtitle = d->pairing ? "Pairing\xe2\x80\xa6" : kind(d->cod);
        std::string a = d->address;
        r.onSelect = [a] { g_core.btPair(a); };
        rows.push_back(std::move(r));
      }
    }
    list_.setRows(std::move(rows), true);
  }
  // a fresh search every 20 s while nothing is being paired
  bool poll(const CoreState& s, int64_t now) override {
    bool pairing = false;
    for (int i = 0; i < s.btCount; i++) pairing |= s.bt[i].pairing != 0;
    if (s.btPower == 2 && !s.btSearching && !pairing && now - lastSearch_ > 20000 * kMs) {
      g_core.btSearch(true);
      lastSearch_ = now;
    }
    return false;
  }

 private:
  static Row header(const std::string& title) {
    Row h;
    h.header = true;
    h.title = title;
    return h;
  }
  int64_t lastSearch_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeBluetooth() { return std::unique_ptr<Screen>(new BluetoothScreen()); }

// for the dashboard: Bt.name, Bt.kind and Bt.isAudio
std::string btName(const BtDevice& d) { return nameOf(d); }
std::string btKind(uint32_t cod) { return kind(cod); }
bool btIsAudio(uint32_t cod) { return isAudio(cod); }

// Settings' row: the headphones music plays on, else on or off
std::string bluetoothState() {
  CoreState s = g_core.state();
  if (s.btPower < 0) return "\xe2\x80\x94";
  if (s.btPower != 2) return "Off";
  for (int i = 0; i < s.btCount; i++)
    if (s.bt[i].a2dp == 2) return nameOf(s.bt[i]);
  return "On";
}

// From the loop: a headset's buttons (AVRCP, as 1.0.x's media session took them) and, as Android's
// "becoming noisy", a pause when the headphones music played on go away.
void bluetoothTick() {
  static uint32_t keySerial = 0;
  static bool wasConnected = false;
  CoreState s = g_core.state();
  if (s.btKeySerial != keySerial) {
    keySerial = s.btKeySerial;
    PlayerState p = g_player.state();
    switch (s.btKey) {
      case 0x44: if (!p.playing) g_player.togglePlay(); break;  // play
      case 0x46:                                                 // pause
      case 0x45: if (p.playing) g_player.pause(); break;         // stop
      case 0x4b: g_player.next(); break;
      case 0x4c: g_player.previous(); break;
      default: break;
    }
  }
  bool connected = false;
  for (int i = 0; i < s.btCount; i++) connected |= s.bt[i].a2dp == 2;
  if (connected != wasConnected && getenv("RONDO_BT_LOG"))
    printf("bt: the UI sees the headphones %s (playing %d)\n", connected ? "connected" : "gone", g_player.state().playing);
  if (wasConnected && !connected && g_player.state().playing) g_player.pause();
  wasConnected = connected;
}

}  // namespace rondo::shell
