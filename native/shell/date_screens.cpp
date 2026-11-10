// Settings › Date & time (2.0, Nov 10): the date and the time, and the time zone, each from the network
// (rondo-core, at every Wi-Fi connection) or set by hand; and the clock in 24 hours or AM/PM. The time
// set here goes into the RTC too (services/clock.h), so it holds while the Y1 is off.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/system_properties.h>

#include <algorithm>
#include <memory>

#include "../services/clock.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {
namespace {

Row entry(const std::string& title, const std::string& subtitle = "", const std::string& trailing = "") {
  Row r;
  r.title = title, r.subtitle = subtitle, r.trailing = trailing;
  return r;
}

const char* onOff(bool v) { return v ? "On" : "Off"; }

std::string currentZone() {
  char z[PROP_VALUE_MAX] = "";
  __system_property_get("persist.sys.timezone", z);
  return z[0] ? z : "Etc/UTC";
}

// "Asia/Tokyo" -> "Tokyo", "America/Argentina/Buenos_Aires" -> "Buenos Aires", "Etc/UTC" -> "UTC"
std::string cityOf(const std::string& zone) {
  std::string c = zone.substr(zone.rfind('/') + 1);
  for (char& ch : c)
    if (ch == '_') ch = ' ';
  return c;
}

// "GMT+4:00", "GMT-3:30", "GMT"
std::string gmt(long seconds) {
  if (seconds == 0) return "GMT";
  long a = labs(seconds);
  char b[24];
  snprintf(b, sizeof(b), "GMT%c%ld:%02ld", seconds < 0 ? '-' : '+', a / 3600, a / 60 % 60);
  return b;
}

// the zone's offset from UTC now. bionic's localtime reads TZ before persist.sys.timezone, on the UI
// thread only (the clock is drawn there too)
long offsetIn(const std::string& zone, time_t now) {
  setenv("TZ", zone.c_str(), 1);
  struct tm tm;
  localtime_r(&now, &tm);
  unsetenv("TZ");
  return tm.tm_gmtoff;
}

void tellCore() { g_core.clockConfigure(g_prefs.autoTime, g_prefs.autoZone); }

// The zones offered by hand: about a hundred cities the world over, as Android's Settings listed them;
// those the firmware's tzdata doesn't have are left out
const char* kZones[] = {
    "Pacific/Midway", "Pacific/Honolulu", "America/Anchorage", "America/Los_Angeles", "America/Tijuana", "America/Phoenix",
    "America/Denver", "America/Chihuahua", "America/Chicago", "America/Mexico_City", "America/Costa_Rica", "America/Regina",
    "America/Bogota", "America/Lima", "America/New_York", "America/Toronto", "America/Havana", "America/Caracas",
    "America/Halifax", "America/Santo_Domingo", "America/Manaus", "America/Santiago", "America/La_Paz", "America/St_Johns",
    "America/Sao_Paulo", "America/Argentina/Buenos_Aires", "America/Montevideo", "America/Nuuk", "Atlantic/South_Georgia",
    "Atlantic/Azores", "Atlantic/Cape_Verde", "Etc/UTC", "Europe/London", "Europe/Dublin", "Europe/Lisbon", "Africa/Casablanca",
    "Africa/Abidjan", "Europe/Amsterdam", "Europe/Berlin", "Europe/Brussels", "Europe/Madrid", "Europe/Paris", "Europe/Rome",
    "Europe/Stockholm", "Europe/Vienna", "Europe/Warsaw", "Europe/Prague", "Europe/Belgrade", "Europe/Budapest",
    "Africa/Lagos", "Africa/Algiers", "Africa/Tunis", "Europe/Athens", "Europe/Bucharest", "Europe/Helsinki", "Europe/Kyiv",
    "Europe/Sofia", "Europe/Riga", "Europe/Vilnius", "Europe/Tallinn", "Asia/Beirut", "Asia/Jerusalem", "Asia/Amman",
    "Asia/Damascus", "Africa/Cairo", "Africa/Johannesburg", "Africa/Harare", "Europe/Istanbul", "Europe/Minsk",
    "Europe/Moscow", "Asia/Baghdad", "Asia/Kuwait", "Asia/Riyadh", "Asia/Qatar", "Africa/Nairobi", "Africa/Addis_Ababa",
    "Asia/Tehran", "Asia/Baku", "Asia/Tbilisi", "Asia/Yerevan", "Asia/Dubai", "Europe/Samara", "Asia/Kabul", "Asia/Karachi",
    "Asia/Tashkent", "Asia/Yekaterinburg", "Asia/Kolkata", "Asia/Colombo", "Asia/Kathmandu", "Asia/Almaty", "Asia/Bishkek",
    "Asia/Dhaka", "Asia/Omsk", "Asia/Yangon", "Asia/Bangkok", "Asia/Jakarta", "Asia/Ho_Chi_Minh", "Asia/Krasnoyarsk",
    "Asia/Shanghai", "Asia/Hong_Kong", "Asia/Taipei", "Asia/Singapore", "Asia/Kuala_Lumpur", "Asia/Manila", "Australia/Perth",
    "Asia/Irkutsk", "Asia/Ulaanbaatar", "Asia/Seoul", "Asia/Tokyo", "Asia/Yakutsk", "Australia/Darwin", "Australia/Adelaide",
    "Australia/Brisbane", "Australia/Sydney", "Australia/Melbourne", "Australia/Hobart", "Pacific/Guam", "Asia/Vladivostok",
    "Asia/Magadan", "Pacific/Noumea", "Pacific/Auckland", "Pacific/Fiji", "Asia/Kamchatka", "Pacific/Tongatapu",
    "Pacific/Apia", "Pacific/Kiritimati"};

// The zone picker: by their offset from UTC now, then by name
class ZoneScreen : public ShellListScreen {
 public:
  ZoneScreen() : ShellListScreen("Time zone") {
    time_t now = time(nullptr);
    struct Z {
      std::string id;
      long offset;
    };
    std::vector<Z> zones;
    for (const char* id : kZones)
      if (knownTimeZone(id)) zones.push_back({id, offsetIn(id, now)});
    std::stable_sort(zones.begin(), zones.end(), [](const Z& a, const Z& b) {
      return a.offset != b.offset ? a.offset < b.offset : cityOf(a.id) < cityOf(b.id);
    });
    std::string current = currentZone();
    std::vector<Row> rows;
    int at = 0;
    for (const Z& z : zones) {
      if (z.id == current) at = int(rows.size());
      Row r = entry(cityOf(z.id), gmt(z.offset), z.id == current ? "Current" : "");
      std::string id = z.id;
      r.onSelect = [id] {
        setTimeZoneByHand(id);
        g_app->pop();
      };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows));
    list_.selectIndex(at);
  }
};

// Setting a date or a time by hand: its parts side by side, the selected one in the accent's color.
// The wheel changes it, Center (or Next) goes to the next part and on the last one sets it; Prev goes
// back a part, Menu leaves without a change.
class PartsScreen : public Screen {
 public:
  struct Part {
    int value, lo, hi;
    std::function<std::string(int)> show;
  };
  PartsScreen(std::string title, std::vector<Part> parts, std::string separator, std::function<void(const std::vector<int>&)> set)
      : title_(std::move(title)), parts_(std::move(parts)), sep_(std::move(separator)), set_(std::move(set)) {}
  std::string title() const override { return title_; }
  bool onAction(Action a, int steps) override {
    Part& p = parts_[size_t(at_)];
    int span = p.hi - p.lo + 1;
    switch (a) {
      case Action::Down:
        p.value = p.lo + ((p.value - p.lo + steps) % span + span) % span;
        return true;
      case Action::Up:
        p.value = p.lo + ((p.value - p.lo - steps) % span + span) % span;
        return true;
      case Action::Select:
      case Action::Next:
        if (at_ + 1 < int(parts_.size())) {
          at_++;
          return true;
        }
        {
          std::vector<int> v;
          for (const Part& q : parts_) v.push_back(q.value);
          set_(v);
        }
        g_app->pop();
        return true;
      case Action::Prev:
        if (at_ > 0) at_--;
        return true;
      default:
        return false;
    }
  }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    // the parts' widths, then all of them centered
    std::vector<Label> labels;
    Label sep = label(e, sep_, 22, true);
    float total = 0;
    int pad = e.dp(8);
    for (const Part& q : parts_) {
      labels.push_back(label(e, q.show(q.value), 22, true));
      total += labels.back().w + 2 * pad;
    }
    total += float(sep.w + e.dp(4)) * float(parts_.size() - 1);
    float x = floorf((w - total) / 2), top = floorf(h * 0.36f);
    int boxH = labels.empty() ? 0 : labels[0].h + e.dp(10);
    for (size_t i = 0; i < parts_.size(); i++) {
      float bw = float(labels[i].w + 2 * pad);
      bool sel = int(i) == at_;
      e.c.layer(1);
      e.c.shape({x, top, bw, float(boxH)}, e.dpf(6), sel ? p.accent : p.rowLine);
      e.c.layer(2);
      drawLabel(e, labels[i], x + float(pad), top + float((boxH - labels[i].h) / 2), sel ? p.white : p.text);
      x += bw;
      if (i + 1 < parts_.size()) {
        drawLabel(e, sep, x + float(e.dp(2)), top + float((boxH - sep.h) / 2), p.muted);
        x += float(sep.w + e.dp(4));
      }
    }
    Label hint = label(e, "Wheel: change \xc2\xb7 Center: next \xc2\xb7 Menu: cancel", 11, false, w - 2 * e.dp(10));
    drawLabel(e, hint, centerText(0, w, hint.w), top + float(boxH) + float(e.dp(18)), p.muted);
  }

 private:
  std::string title_;
  std::vector<Part> parts_;
  std::string sep_;
  std::function<void(const std::vector<int>&)> set_;
  int at_ = 0;
};

std::string two(int v) {
  char b[8];
  snprintf(b, sizeof(b), "%02d", v);
  return b;
}

std::unique_ptr<Screen> makeDateEntry() {
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  std::vector<PartsScreen::Part> parts = {
      {tm.tm_mon, 0, 11, [](int v) { return std::string(kMonths[v]); }},
      {tm.tm_mday, 1, 31, [](int v) { return std::to_string(v); }},
      {std::max(2025, std::min(2099, tm.tm_year + 1900)), 2025, 2099, [](int v) { return std::to_string(v); }}};
  return std::unique_ptr<Screen>(new PartsScreen("Date", parts, " ", [](const std::vector<int>& v) {
    time_t t = time(nullptr);
    struct tm d;
    localtime_r(&t, &d);  // the time of day stays
    d.tm_mon = v[0], d.tm_year = v[2] - 1900;
    // a day past the month's end is its last day (Feb 31 -> Feb 28 or 29)
    static const int kDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int y = v[2];
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    int last = v[0] == 1 && !leap ? 28 : kDays[v[0]];
    d.tm_mday = std::min(v[1], last);
    d.tm_isdst = -1;
    time_t set = mktime(&d);
    if (set > 0) setWallClock(set);
  }));
}

std::unique_ptr<Screen> makeTimeEntry() {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  std::vector<PartsScreen::Part> parts;
  bool h24 = g_prefs.clock24;
  if (h24) {
    parts.push_back({tm.tm_hour, 0, 23, [](int v) { return two(v); }});
    parts.push_back({tm.tm_min, 0, 59, [](int v) { return two(v); }});
  } else {
    int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    parts.push_back({h12, 1, 12, [](int v) { return std::to_string(v); }});
    parts.push_back({tm.tm_min, 0, 59, [](int v) { return two(v); }});
    parts.push_back({tm.tm_hour < 12 ? 0 : 1, 0, 1, [](int v) { return std::string(v ? "PM" : "AM"); }});
  }
  return std::unique_ptr<Screen>(new PartsScreen("Time", parts, ":", [h24](const std::vector<int>& v) {
    time_t t = time(nullptr);
    struct tm d;
    localtime_r(&t, &d);  // the date stays
    d.tm_hour = h24 ? v[0] : v[0] % 12 + (v[2] ? 12 : 0);
    d.tm_min = v[1], d.tm_sec = 0, d.tm_isdst = -1;
    time_t set = mktime(&d);
    if (set > 0) setWallClock(set);
  }));
}

class DateTimeScreen : public ShellListScreen {
 public:
  DateTimeScreen() : ShellListScreen("Date & time") { render(); }
  void onShow() override { render(); }
  // the minute turning shows in the Date and Time rows
  bool tick(int64_t) override {
    time_t now = time(nullptr);
    if (now / 60 == shown_ / 60) return false;
    render();
    return true;
  }

 private:
  void render() {
    time_t now = shown_ = time(nullptr);
    std::vector<Row> rows;
    bool autoTime = g_prefs.autoTime, autoZone = g_prefs.autoZone;
    Row a = entry("Set automatically", "From the network when Wi-Fi is on", onOff(autoTime));
    a.onSelect = [this] {
      g_prefs.autoTime = !g_prefs.autoTime;
      savePrefsNow();
      tellCore();
      render();
    };
    rows.push_back(a);
    Row date = entry("Date", "", clockDate(now, true));
    Row timeRow = entry("Time", "", clockTime(now, g_prefs.clock24));
    if (autoTime) {
      date.enabled = timeRow.enabled = false;
    } else {
      date.chevron = timeRow.chevron = true;
      date.onSelect = [] { g_app->push(makeDateEntry()); };
      timeRow.onSelect = [] { g_app->push(makeTimeEntry()); };
    }
    rows.push_back(date);
    rows.push_back(timeRow);
    Row z = entry("Time zone automatically", "Where the Y1 is online", onOff(autoZone));
    z.onSelect = [this] {
      g_prefs.autoZone = !g_prefs.autoZone;
      savePrefsNow();
      tellCore();
      render();
    };
    rows.push_back(z);
    std::string zone = currentZone();
    Row zr = entry("Time zone", gmt(offsetIn(zone, now)), cityOf(zone));
    if (autoZone) {
      zr.enabled = false;
    } else {
      zr.chevron = true;
      zr.onSelect = [] { g_app->push(std::unique_ptr<Screen>(new ZoneScreen())); };
    }
    rows.push_back(zr);
    Row h = entry("24-hour time", "", onOff(g_prefs.clock24));
    h.onSelect = [this] {
      g_prefs.clock24 = !g_prefs.clock24;
      savePrefsNow();
      render();
    };
    rows.push_back(h);
    list_.setRows(std::move(rows), true);
  }
  time_t shown_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeDateTime() { return std::unique_ptr<Screen>(new DateTimeScreen()); }

}  // namespace rondo::shell
