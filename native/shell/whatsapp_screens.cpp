// WhatsApp's screens (whatsapp.h): the chats, a chat with its messages, writing one on the TV keyboard,
// a voice message from the Y1's microphone, a photo or a video from the card, the contacts, a profile.
// Dark, in WhatsApp's own greens, like the YouTube sections are in theirs.
//
// A chat: the messages scroll over a bar of three buttons (Message, Voice, Photo). The wheel walks the
// messages and then the buttons; center opens what a message holds (a photo, a video, a voice message
// played in place of the music, which comes back after) or its menu; held, the menu (Reply, Profile).
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <thread>

#include <GLES2/gl2.h>

#include <mutex>

#include "../ui/image.h"
#include "../ui/keyboard.h"
#include "../ui/list.h"
#include "app.h"
#include "main_queue.h"
#include "music_list.h"
#include "screens.h"
#include "whatsapp.h"

namespace rondo::shell {
namespace {

// WhatsApp's dark colors
constexpr Color kBg = 0xff0b141a, kBar = 0xff1f2c34, kIn = 0xff202c33, kOut = 0xff005c4b, kSel = 0xff2a3942;
constexpr Color kText = 0xffe9edef, kMuted = 0xff8696a0, kGreen = 0xff00a884, kBrand = 0xff25d366, kBlue = 0xff53bdeb;
constexpr Color kRed = 0xfff15c6d, kChip = 0xff182229, kQuote = 0x33000000;
constexpr Color kNames[] = {0xff53bdeb, 0xfff7a8b8, 0xffffd279, 0xff06cf9c, 0xffa5b337, 0xffff8a65, 0xffc4a3ff, 0xff7fd1d6};

uint32_t hashOf(const std::string& s) {
  uint32_t h = 2166136261u;
  for (char c : s) h = (h ^ uint8_t(c)) * 16777619u;
  return h;
}

int64_t nowMs() { return int64_t(time(nullptr)) * 1000; }

// a day's start (local time), for "Today", "Yesterday"
int64_t dayOf(int64_t ms) {
  time_t t = time_t(ms / 1000);
  struct tm tm;
  localtime_r(&t, &tm);
  tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
  return int64_t(mktime(&tm));
}

std::string timeOf(int64_t ms) { return clockTime(time_t(ms / 1000), g_prefs.clock24); }

// the chats list's time: today the time, yesterday, this week the day, else the date
std::string whenShort(int64_t ms) {
  if (ms <= 0) return "";
  int64_t d = (dayOf(nowMs()) - dayOf(ms)) / 86400;
  if (d <= 0) return timeOf(ms);
  if (d == 1) return "Yesterday";
  time_t t = time_t(ms / 1000);
  if (d < 7) {
    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    struct tm tm;
    localtime_r(&t, &tm);
    return kDays[tm.tm_wday];
  }
  return clockDate(t, false);
}

// a chat's day between its messages
std::string dayLabel(int64_t ms) {
  int64_t d = (dayOf(nowMs()) - dayOf(ms)) / 86400;
  if (d <= 0) return "Today";
  if (d == 1) return "Yesterday";
  time_t t = time_t(ms / 1000);
  struct tm tm, now;
  localtime_r(&t, &tm);
  time_t n = time(nullptr);
  localtime_r(&n, &now);
  if (d < 7) {
    static const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    return kDays[tm.tm_wday];
  }
  std::string s = clockDate(t, false);
  if (tm.tm_year != now.tm_year) s += ", " + std::to_string(tm.tm_year + 1900);
  return s;
}

// a round picture: the person's or group's, on a colored circle with a glyph until it comes (or when there's none)
void avatar(Env& e, const std::string& jid, bool group, Rect r) {
  static const Color kCircles[] = {0xff6b7c85, 0xff5f6f78, 0xff6a7480, 0xff56656e};
  float a = 0;
  // no picture (404) isn't asked again for 10 minutes
  const Texture* t = e.covers.get(WaClient::avatarPath(jid), int(r.w), int(r.h), 0, e.now, &a, 600 * kSec);
  if (!t || a < 1) {  // the placeholder until the picture has faded in over it
    e.c.layer(2);
    e.c.shape(r, r.w / 2, kCircles[hashOf(jid) % 4]);
    int g = int(r.w * 0.56f);
    e.c.layer(4);
    e.icons.drawInTile(e.c, group ? Icon::People : Icon::Person, r, float(g), 0xffcfd8dc);
  }
  if (t) {
    e.c.layer(5);  // over the placeholder's glyph while it fades in
    e.c.image(*t, r, {0, 0, 1, 1}, r.w / 2, withAlpha(0xffffffff, a));
  }
}

// the header: the bubble, a title (and a line under it), the status icons
void waHeader(Env& e, float w, const std::string& title, const std::string& sub = "", const std::string& jid = "", bool group = false) {
  int H = e.dp(31), pad = e.dp(10);
  e.c.layer(1);
  e.c.fill({0, 0, w, float(H)}, kBar);
  float x = float(pad);
  int s = e.dp(16);
  if (jid.empty()) {
    e.c.layer(4);
    e.icons.whatsappLogo(e.c, {x, float((H - s) / 2), float(s), float(s)});
    x += s + e.dp(7);
  } else {
    int a = e.dp(23);
    avatar(e, jid, group, {x, float((H - a) / 2), float(a), float(a)});
    x += a + e.dp(7);
  }
  float right = statusIcons(e, w - pad, H, kMuted, false);
  e.c.layer(4);
  Label t = label(e, title, sub.empty() ? 14 : 13, true, right - x - e.dp(6));
  if (sub.empty()) {
    drawLabel(e, t, x, float((H - t.h) / 2), kText);
  } else {
    Label u = label(e, sub, 9, false, right - x - e.dp(6));
    float top = float((H - t.h - u.h) / 2);
    drawLabel(e, t, x, top, kText);
    drawLabel(e, u, x, top + t.h, kMuted);
  }
}

// The picture loader hands over at most 40 000 pixels a picture (ui/image.cpp's slots): a bigger box asks
// for that much, in its shape, and draws it scaled (opened, a photo is shown whole)
void capped(int& w, int& h) {
  float k = sqrtf(40000.f / float(std::max(1, w * h)));
  if (k < 1) w = std::max(1, int(w * k)), h = std::max(1, int(h * k));
}

// a message of mine's state at x, centered on midY: a clock while it goes, one tick sent, two delivered,
// two blue read (or played); a red mark when it didn't go. Returns its width.
float drawTicks(Env& e, const std::string& status, bool sending, bool failed, float x, float midY, Color muted) {
  if (failed) {
    int s = e.dp(11);
    e.icons.draw(e.c, Icon::Info, {x, midY - s / 2.f, float(s), float(s)}, kRed);
    return float(s);
  }
  if (sending || status == "pending" || status.empty()) {
    int s = e.dp(10);
    e.icons.draw(e.c, Icon::Clock, {x, midY - s / 2.f, float(s), float(s)}, muted);
    return float(s);
  }
  int s = e.dp(13);
  bool seen = status == "read" || status == "played";
  e.icons.draw(e.c, status == "sent" ? Icon::Tick : Icon::Ticks, {x, midY - s / 2.f, float(s), float(s)}, seen ? kBlue : muted);
  return float(s);
}

// a message across the middle (the list is empty, or can't be shown)
void centered(Env& e, float w, float top, float h, const std::string& text, const std::string& sub = "") {
  std::vector<Label> lines = wrap(e, text, 12, false, w - e.dp(40), 4), subs;
  if (!sub.empty()) subs = wrap(e, sub, 10, false, w - e.dp(40), 4);
  int total = 0;
  for (auto& l : lines) total += l.h;
  if (!subs.empty()) total += e.dp(6);
  for (auto& l : subs) total += l.h;
  float y = top + float((int(h) - total) / 2);
  e.c.layer(4);
  for (auto& l : lines) drawLabel(e, l, centerText(0, w, l.w), y, kText), y += l.h;
  if (!subs.empty()) y += e.dp(6);
  for (auto& l : subs) drawLabel(e, l, centerText(0, w, l.w), y, kMuted), y += l.h;
}

// what the state of the server's WhatsApp says, when there's nothing to show ("" there's something)
std::pair<std::string, std::string> stateWords(const WaClient& c) {
  using S = WaClient::State;
  switch (c.state()) {
    case S::NoServer:
      return {"WhatsApp works through your Rondo server",
              "Connect one in Settings \xe2\x80\xba Server, then link WhatsApp on the server\xe2\x80\x99s admin page."};
    case S::Old:
      return {"Your server is too old for WhatsApp", "Update it to Rondo server 3.3.0 or newer on its admin page."};
    case S::Unlinked:
      return {"WhatsApp isn\xe2\x80\x99t linked yet",
              "Link it on your server\xe2\x80\x99s admin page (WhatsApp), with a code you type on your phone."};
    case S::Linking: return {"Linking\xe2\x80\xa6", "Type the code the server\xe2\x80\x99s admin page shows on your phone."};
    default: break;
  }
  return {"", ""};
}

// ---- a voice message played in place of the music, which comes back after ----

struct Voice {
  bool on = false;
  std::string jid, id, file;
  std::vector<std::string> queue;
  int index = -1, repeat = 0;
  bool playing = false, shuffle = false;
  int64_t ms = 0, startedAt = 0;
} g_voice;

void voiceStop() {
  if (!g_voice.on) return;
  g_voice.on = false;
  g_player.setRepeat(g_voice.repeat);
  g_player.setShuffle(g_voice.shuffle);
  if (!g_voice.queue.empty()) g_player.restoreQueue(g_voice.queue, g_voice.index, g_voice.ms, false);
  else g_player.playQueue({}, 0);
  g_queueLent = false;
}

void voicePlay(const std::string& jid, const std::string& id, const std::string& file) {
  voiceStop();
  PlayerState s = g_player.state();
  Voice v;
  if (g_player.active() && s.index >= 0 && !s.video && !s.fmOn) {
    v.queue = g_player.queue();
    v.index = s.index;
    v.ms = PlayerHost::positionAt(s, now_ns());
    v.playing = s.playing;
  }
  v.shuffle = s.shuffle, v.repeat = s.repeat;
  v.jid = jid, v.id = id, v.file = file;
  v.on = true;
  v.startedAt = now_ns();
  g_voice = v;
  g_queueLent = true;  // resume.cpp leaves the queue as it was
  g_player.setRepeat(0);
  g_player.setShuffle(false);
  g_player.playQueue({file}, 0);
}

// from the screens' ticks: the voice message ended (or something else plays): the music comes back
bool voiceTick() {
  if (!g_voice.on) return false;
  PlayerState s = g_player.state();
  if (now_ns() - g_voice.startedAt < kSec) return true;
  if (s.ended || s.path != g_voice.file || (!s.playing && !s.buffering && s.positionMs >= s.durationMs - 300 && s.durationMs > 0)) {
    voiceStop();
    return true;
  }
  return true;
}

bool voicePlaying(const std::string& id) { return g_voice.on && g_voice.id == id; }

float voiceProgress() {
  PlayerState s = g_player.state();
  if (!g_voice.on || s.durationMs <= 0) return 0;
  return clamp01(float(PlayerHost::positionAt(s, now_ns())) / float(s.durationMs));
}

std::map<std::string, std::string> g_drafts;  // what was typed and not sent, per chat

std::unique_ptr<Screen> makeChat(const std::string& jid, const std::string& name, bool group);
std::unique_ptr<Screen> makeSearch(bool contactsOnly);  // chats and contacts by name or number
std::unique_ptr<Screen> makeReact(const std::string& jid, const std::string& id, const std::string& mine);
std::unique_ptr<Screen> makeProfile(const std::string& jid, const std::string& name, bool group, bool offerChat);

// ===================================================================================================
// The chats

class ChatsScreen : public Screen {
 public:
  ChatsScreen() { waClient().hold(); }
  ~ChatsScreen() override { waClient().release(); }
  std::string title() const override { return "WhatsApp"; }
  bool statusBar() const override { return false; }
  bool waitsForContent() const override { return true; }
  bool onAction(Action a, int steps) override {
    switch (a) {
      case Action::Down: return list_.move(steps), true;
      case Action::Up: return list_.move(-steps), true;
      case Action::Select: return open(false), true;
      case Action::LongSelect: return open(true), true;
      default: return false;
    }
  }
  bool step(float dt) override { return list_.step(dt); }
  bool tick(int64_t now) override {
    voiceTick();
    if (waClient().serial() == seen_) return false;
    seen_ = waClient().serial();
    return true;
  }
  void draw(Env& e, float w, float h) override {
    WaClient& c = waClient();
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, kBg);
    waHeader(e, w, "WhatsApp");
    float top = float(e.dp(31));
    // a line under the header while the server's WhatsApp isn't there (what the card kept shows)
    std::string note;
    if (c.state() == WaClient::State::Connecting) note = "Connecting to WhatsApp\xe2\x80\xa6";
    else if (c.state() == WaClient::State::Offline) note = offlineOr("Can\xe2\x80\x99t reach your server");
    if (!c.problem().empty()) note = c.problem();
    auto words = stateWords(c);
    const std::vector<WaChat>& chats = c.chats();
    using S = WaClient::State;
    // no server, an old one, or nothing to show yet: what to do about it; what the card kept shows
    // under a line saying why it may not be new
    if (c.state() == S::NoServer || c.state() == S::Old || (!words.first.empty() && chats.empty()))
      return centered(e, w, top, h - top, words.first, words.second);
    if (!words.first.empty()) note = words.first;
    if (!c.loaded()) return centered(e, w, top, h - top, note.empty() ? "Loading\xe2\x80\xa6" : note);
    if (!note.empty()) {
      Label n = label(e, note, 9, false, w - e.dp(20));
      float nh = float(n.h + e.dp(6));
      e.c.layer(1);
      e.c.fill({0, top, w, nh}, kChip);
      e.c.layer(4);
      drawLabel(e, n, centerText(0, w, n.w), top + e.dp(3), kMuted);
      top += nh;
    }
    float rowH = float(e.dp(46)), viewH = h - top;
    int count = int(chats.size()) + 2;  // Search and New chat first
    if (count != count_ || rowH != rowH_ || viewH != viewH_) {
      int keep = list_.selected();
      list_.configure(count, rowH, viewH);
      if (count_ < 0) list_.select(0);
      else list_.select(std::min(keep, count - 1));
      count_ = count, rowH_ = rowH, viewH_ = viewH;
    }
    e.c.pushClip({0, top, w, viewH});
    float sc = list_.scroll();
    for (int i = list_.first(); i <= list_.last(); i++) {
      float y = top + i * rowH - sc;
      if (i == list_.selected()) {
        e.c.layer(1);
        e.c.shape({float(e.dp(4)), y + 1, w - e.dp(8), rowH - 2}, e.dpf(8), kSel);
      }
      if (i == 0) actionRow(e, w, y, rowH, Icon::Search, kSel, "Search", "Names and numbers");
      else if (i == 1) actionRow(e, w, y, rowH, Icon::Plus, kGreen, "New chat", "Your contacts");
      else chatRow(e, w, y, rowH, chats[size_t(i - 2)]);
    }
    e.c.popClip();
    if (chats.empty()) centered(e, w, top + 2 * rowH, viewH - 2 * rowH, "No chats yet", "Start one with New chat.");
    e.c.layer(0);
  }

 private:
  void open(bool hold) {
    int i = list_.selected();
    if (i == 0) return g_app->push(makeSearch(false));
    if (i == 1) return g_app->push(makeContacts());
    const std::vector<WaChat>& chats = waClient().chats();
    if (i - 2 >= int(chats.size())) return;
    const WaChat& ch = chats[size_t(i - 2)];
    g_app->push(hold ? makeProfile(ch.jid, ch.name, ch.group, false) : makeChat(ch.jid, ch.name, ch.group));
  }
  std::unique_ptr<Screen> makeContacts();
  void actionRow(Env& e, float w, float y, float rowH, Icon icon, Color circle, const char* title, const char* sub) {
    int a = e.dp(34);
    Rect r{float(e.dp(10)), y + float((int(rowH) - a) / 2), float(a), float(a)};
    e.c.layer(2);
    e.c.shape(r, r.w / 2, circle);
    int g = e.dp(18);
    e.c.layer(4);
    e.icons.drawInTile(e.c, icon, r, float(g), rgb(0xffffff));
    float x = r.x + a + e.dp(10);
    Label t = label(e, title, 13, true, w - x - e.dp(10));
    Label s = label(e, sub, 10, false, w - x - e.dp(10));
    float ty = y + float((int(rowH) - t.h - s.h - e.dp(1)) / 2);
    drawLabel(e, t, x, ty, kText);
    drawLabel(e, s, x, ty + t.h + e.dp(1), kMuted);
  }
  void chatRow(Env& e, float w, float y, float rowH, const WaChat& ch) {
    int a = e.dp(34);
    avatar(e, ch.jid, ch.group, {float(e.dp(10)), y + float((int(rowH) - a) / 2), float(a), float(a)});
    float x = float(e.dp(10) + a + e.dp(10)), right = w - e.dp(12);
    e.c.layer(4);
    Label when = label(e, whenShort(ch.at), 9, false);
    Label name = label(e, ch.name, 13, true, right - x - when.w - e.dp(8));
    // the preview: who wrote it (a group), the ticks for yours, the text
    std::string text = ch.hasPreview ? ch.preview.text : "";
    if (ch.hasPreview && !ch.preview.sender.empty()) text = ch.preview.sender + ": " + text;
    float lineY = y + float((int(rowH) - name.h - e.dp(15)) / 2);
    drawLabel(e, name, x, lineY, kText);
    drawLabel(e, when, right - when.w, lineY + float(name.h - when.h) / 2, ch.unread ? kBrand : kMuted);
    float py = lineY + name.h + e.dp(2);
    float px = x, pRight = right;
    if (ch.unread) {  // the green count
      Label n = label(e, ch.unread > 99 ? "99+" : std::to_string(ch.unread), 9, true);
      float bh = float(e.dp(15)), bw = std::max(bh, float(n.w + e.dp(8)));
      e.c.layer(2);
      e.c.shape({right - bw, py - e.dp(1), bw, bh}, bh / 2, kGreen);
      e.c.layer(4);
      drawLabel(e, n, centerText(right - bw, bw, n.w), py - e.dp(1) + float((int(bh) - n.h) / 2), rgb(0x0b141a));
      pRight = right - bw - e.dp(6);
    }
    Label line = label(e, " ", 11, false);
    float mid = py + line.h / 2.f;
    if (ch.hasPreview && ch.preview.fromMe) px += drawTicks(e, ch.preview.status, false, false, px, mid, kMuted) + e.dp(3);
    // a voice message as WhatsApp shows it: a mic and its length; a photo, a video, a document with their icon
    const WaPreview& pr = ch.preview;
    Icon kindIcon = Icon::Chat;
    bool icon = ch.hasPreview && (pr.kind == "voice" || pr.kind == "image" || pr.kind == "video" || pr.kind == "document" || pr.kind == "audio");
    if (icon) {
      if (!pr.sender.empty()) {
        Label who = label(e, pr.sender + ": ", 11, false, (pRight - px) / 2);
        drawLabel(e, who, px, py, kMuted);
        px += who.w;
      }
      kindIcon = pr.kind == "voice" ? Icon::Mic : pr.kind == "image" ? Icon::Photo : pr.kind == "video" ? Icon::Film
                 : pr.kind == "audio" ? Icon::Headphones : Icon::File;
      int s = e.dp(13);
      e.icons.draw(e.c, kindIcon, {px, mid - s / 2.f, float(s), float(s)}, pr.kind == "voice" ? kGreen : kMuted);
      px += s + e.dp(3);
      text = pr.kind == "voice" || pr.kind == "audio" ? (pr.seconds > 0 ? formatTime(pr.seconds) : pr.kind == "voice" ? "Voice message" : "Audio")
             : pr.kind == "image" ? (pr.caption.empty() ? "Photo" : pr.caption)
             : pr.kind == "video" ? (pr.caption.empty() ? "Video" : pr.caption)
                                  : pr.text.substr(pr.text.compare(0, 10, "Document: ") == 0 ? 10 : 0);
    }
    Label pv = label(e, text, 11, false, pRight - px);
    drawLabel(e, pv, px, py, kMuted);
  }
  WheelList list_;
  int count_ = -1;
  float rowH_ = 0, viewH_ = 0;
  uint32_t seen_ = 0;
};

// ===================================================================================================
// The contacts (New chat): everyone, the saved ones first; Next/Prev jump a letter

class ContactsScreen : public Screen {
 public:
  ContactsScreen() {
    waClient().hold();
    waClient().loadContacts([] {});
  }
  ~ContactsScreen() override { waClient().release(); }
  std::string title() const override { return "New chat"; }
  bool statusBar() const override { return false; }
  bool onAction(Action a, int steps) override {
    const auto& all = waClient().contacts();
    switch (a) {
      case Action::Down: return list_.move(steps), true;
      case Action::Up: return list_.move(-steps), true;
      case Action::Next:
      case Action::Prev: {  // the next or previous first letter (row 0 is Search)
        if (all.empty()) return true;
        int at = std::max(0, list_.selected() - 1);
        auto letter = [&](int i) { return all[size_t(i)].name.empty() ? ' ' : toupper((unsigned char)all[size_t(i)].name[0]); };
        int cur = letter(at), i = at;
        if (a == Action::Next) {
          while (i < int(all.size()) - 1 && letter(i) == cur) i++;
        } else {
          while (i > 0 && letter(i - 1) == cur) i--;
          if (i == at && i > 0) {
            int prev = letter(i - 1);
            while (i > 0 && letter(i - 1) == prev) i--;
          }
        }
        list_.select(i + 1);
        return true;
      }
      case Action::Select:
      case Action::LongSelect: {
        int i = list_.selected() - 1;
        if (i == -1) return g_app->push(makeSearch(true)), true;
        if (i < 0 || i >= int(all.size())) return true;
        const WaContact& c = all[size_t(i)];
        g_app->push(a == Action::Select ? makeChat(c.jid, c.name, false) : makeProfile(c.jid, c.name, false, true));
        return true;
      }
      default: return false;
    }
  }
  bool step(float dt) override { return list_.step(dt); }
  bool tick(int64_t) override {
    voiceTick();
    if (waClient().serial() == seen_) return false;
    seen_ = waClient().serial();
    return true;
  }
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, kBg);
    const auto& all = waClient().contacts();
    waHeader(e, w, "New chat", waClient().contactsLoaded() ? std::to_string(all.size()) + " contacts" : "");
    float top = float(e.dp(31));
    if (!waClient().contactsLoaded()) return centered(e, w, top, h - top, "Loading your contacts\xe2\x80\xa6");
    if (all.empty()) return centered(e, w, top, h - top, "No contacts yet");
    float rowH = float(e.dp(42)), viewH = h - top;
    if (int(all.size()) + 1 != count_ || viewH != viewH_) {
      list_.configure(int(all.size()) + 1, rowH, viewH);
      if (count_ < 0) list_.select(0);
      count_ = int(all.size()) + 1, viewH_ = viewH;
    }
    e.c.pushClip({0, top, w, viewH});
    for (int i = list_.first(); i <= list_.last(); i++) {
      float y = top + i * rowH - list_.scroll();
      if (i == list_.selected()) {
        e.c.layer(1);
        e.c.shape({float(e.dp(4)), y + 1, w - e.dp(8), rowH - 2}, e.dpf(8), kSel);
      }
      if (i == 0) {  // Search
        int a = e.dp(30);
        Rect r{float(e.dp(10)), y + float((int(rowH) - a) / 2), float(a), float(a)};
        e.c.layer(2);
        e.c.shape(r, r.w / 2, kSel);
        int g = e.dp(16);
        e.c.layer(4);
        e.icons.drawInTile(e.c, Icon::Search, r, float(g), rgb(0xffffff));
        Label t = label(e, "Search", 13, true);
        drawLabel(e, t, r.x + a + e.dp(10), y + float((int(rowH) - t.h) / 2), kText);
        continue;
      }
      const WaContact& c = all[size_t(i - 1)];
      int a = e.dp(30);
      avatar(e, c.jid, false, {float(e.dp(10)), y + float((int(rowH) - a) / 2), float(a), float(a)});
      float x = float(e.dp(10) + a + e.dp(10));
      e.c.layer(4);
      Label n = label(e, c.name, 13, true, w - x - e.dp(10));
      std::string sub = c.number + (!c.saved || c.pushName.empty() || c.pushName == c.name ? "" : " \xc2\xb7 ~" + c.pushName);
      Label s = label(e, sub, 10, false, w - x - e.dp(10));
      float ty = y + float((int(rowH) - n.h - s.h - e.dp(1)) / 2);
      drawLabel(e, n, x, ty, kText);
      drawLabel(e, s, x, ty + n.h + e.dp(1), kMuted);
    }
    e.c.popClip();
    e.c.layer(0);
  }

 private:
  WheelList list_;
  int count_ = -1;
  float viewH_ = 0;
  uint32_t seen_ = 0;
};

std::unique_ptr<Screen> ChatsScreen::makeContacts() { return std::unique_ptr<Screen>(new ContactsScreen()); }

// ===================================================================================================
// A profile: the picture, the name, the number, "about"

class ProfileScreen : public Screen {
 public:
  ProfileScreen(std::string jid, std::string name, bool group, bool offerChat)
      : jid_(std::move(jid)), name_(std::move(name)), group_(group), offerChat_(offerChat) {
    waClient().hold();
    waClient().profile(jid_, [this](const WaContact& c, bool ok) {
      if (ok) contact_ = c;
      loaded_ = true;
      failed_ = !ok;
      dirty_ = true;
    });
  }
  ~ProfileScreen() override { waClient().release(); }
  std::string title() const override { return name_; }
  bool statusBar() const override { return false; }
  bool onAction(Action a, int) override {
    if (a == Action::Select && offerChat_) {
      g_app->push(makeChat(jid_, contact_.name.empty() ? name_ : contact_.name, group_));
      return true;
    }
    return a == Action::Down || a == Action::Up;
  }
  bool tick(int64_t) override {
    bool d = dirty_;
    dirty_ = false;
    return d;
  }
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, kBg);
    waHeader(e, w, group_ ? "Group" : "Contact");
    float y = float(e.dp(31) + e.dp(12));
    int a = e.dp(84);
    avatar(e, jid_, group_, {float((int(w) - a) / 2), y, float(a), float(a)});
    y += a + e.dp(8);
    e.c.layer(4);
    std::string name = contact_.name.empty() ? name_ : contact_.name;
    Label n = label(e, name, 16, true, w - e.dp(20));
    drawLabel(e, n, centerText(0, w, n.w), y, kText);
    y += n.h + e.dp(2);
    std::string sub = group_ ? "Group" : contact_.number;
    if (!group_ && !contact_.pushName.empty() && contact_.pushName != name) sub += " \xc2\xb7 ~" + contact_.pushName;
    if (!sub.empty()) {
      Label s = label(e, sub, 11, false, w - e.dp(20));
      drawLabel(e, s, centerText(0, w, s.w), y, kMuted);
      y += s.h;
    }
    y += e.dp(10);
    if (!group_) {
      std::string about = !loaded_ ? "Loading\xe2\x80\xa6" : failed_ ? "Couldn\xe2\x80\x99t load the profile" : contact_.about.empty() ? "No about" : contact_.about;
      Label head = label(e, "About", 10, true);
      drawLabel(e, head, float(e.dp(16)), y, kGreen);
      y += head.h + e.dp(2);
      for (auto& l : wrap(e, about, 12, false, w - e.dp(32), 3)) drawLabel(e, l, float(e.dp(16)), y, kText), y += l.h;
    }
    if (offerChat_) {
      Label b = label(e, "Message", 12, true);
      float bw = float(b.w + e.dp(36)), bh = float(e.dp(26)), bx = float((int(w) - int(bw)) / 2), by = h - bh - e.dp(10);
      e.c.layer(2);
      e.c.shape({bx, by, bw, bh}, bh / 2, kGreen);
      e.c.layer(4);
      int g = e.dp(13);
      e.icons.draw(e.c, Icon::Chat, {bx + e.dp(10), by + float((int(bh) - g) / 2), float(g), float(g)}, rgb(0x0b141a));
      drawLabel(e, b, bx + e.dp(10) + g + e.dp(5), by + float((int(bh) - b.h) / 2), rgb(0x0b141a));
    }
    e.c.layer(0);
  }

 private:
  std::string jid_, name_;
  bool group_, offerChat_;
  WaContact contact_;
  bool loaded_ = false, failed_ = false, dirty_ = false;
};

// ===================================================================================================
// Writing a message: the TV keyboard, as YouTube Music's search has it

class ComposeScreen : public Screen {
 public:
  ComposeScreen(std::string jid, std::string name, WaQuote quote)
      : jid_(std::move(jid)), name_(std::move(name)), quote_(std::move(quote)), kb_("Send", false, true) {
    kb_.setTyped(g_drafts[jid_]);
  }
  ~ComposeScreen() override { g_drafts[jid_] = kb_.typed(); }
  std::string title() const override { return name_; }
  bool statusBar() const override { return false; }
  bool onAction(Action a, int) override {
    switch (a) {  // TvKeyboard.handle(): it takes every control; Menu on its top row leaves
      case Action::Down: kb_.nextKey(); return true;
      case Action::Up: kb_.prevKey(); return true;
      case Action::Next:
      case Action::LongNext: kb_.right(); return true;
      case Action::Prev:
      case Action::LongPrev: kb_.left(); return true;
      case Action::PlayPause: kb_.down(); return true;
      case Action::Back: return kb_.up();
      case Action::Select: {
        Keyboard::Result r = kb_.press();
        if (r == Keyboard::Result::Typed) kb_.keepAtMost(4000);
        if (r == Keyboard::Result::Action) send();
        return true;
      }
      default: return false;
    }
  }
  bool step(float dt) override { return kb_.step(dt); }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, kBg);
    waHeader(e, w, name_, quote_.id.empty() ? "New message" : "Reply");
    float y = float(e.dp(31) + e.dp(4)), bx = float(e.dp(8)), bw = w - 2 * bx;
    // the message being answered
    if (!quote_.id.empty()) {
      Label qn = label(e, quote_.name.empty() ? "Message" : quote_.name, 10, true, bw - e.dp(14));
      Label qt = label(e, quote_.text.empty() ? quote_.kind : quote_.text, 10, false, bw - e.dp(14));
      float qh = float(qn.h + qt.h + e.dp(6));
      e.c.layer(2);
      e.c.shape({bx, y, bw, qh}, e.dpf(6), kIn);
      e.c.fill({bx, y, float(e.dp(3)), qh}, kGreen);
      e.c.layer(4);
      drawLabel(e, qn, bx + e.dp(9), y + e.dp(3), kGreen);
      drawLabel(e, qt, bx + e.dp(9), y + e.dp(3) + qn.h, kMuted);
      y += qh + e.dp(3);
    }
    // the field: the last two lines typed, with the cursor
    std::string typed = kb_.typed() + "|";
    std::vector<WrappedLine> all = wrapText(e, typed, 13, false, bw - e.dp(20), 200);
    size_t from = all.size() > 2 ? all.size() - 2 : 0;
    Label one = label(e, " ", 13, false);
    float fh = float(2 * one.h + e.dp(8));
    e.c.layer(2);
    e.c.shape({bx, y, bw, fh}, e.dpf(10), p.raised);
    e.c.layer(4);
    float ly = y + e.dp(4);
    if (kb_.typed().empty()) {
      Label hint = label(e, "|  Message", 13, false, bw - e.dp(20));
      drawLabel(e, hint, bx + e.dp(10), ly, kMuted);
    } else {
      for (size_t i = from; i < all.size(); i++) {
        Label l = label(e, all[i].text, 13, false, bw - e.dp(20));
        drawLabel(e, l, bx + e.dp(10), ly, kText);
        ly += l.h;
      }
    }
    float area = y + fh + e.dp(4);
    Label hint = label(e, "Menu up \xc2\xb7 Play down \xc2\xb7 Prev left \xc2\xb7 Next right \xc2\xb7 wheel: next key \xc2\xb7 center: type", 9,
                       false, w);
    float kh = h - area - (e.dp(3) + hint.h + e.dp(4));
    e.c.layer(1);
    kb_.draw(e.c, e.text, p, {float(e.dp(5)), area, w - 2 * e.dp(5), kh}, e.dp(15), e.dp(12));
    e.c.layer(4);
    drawLabel(e, hint, centerText(0, w, hint.w), area + kh + e.dp(3), kMuted);
    e.c.layer(0);
  }

 private:
  void send() {
    std::string t = kb_.typed();
    size_t a = t.find_first_not_of(' '), b = t.find_last_not_of(' ');
    if (a == std::string::npos) return;
    waClient().sendText(jid_, t.substr(a, b - a + 1), quote_.id);
    kb_.clear();
    g_app->pop();
  }
  std::string jid_, name_;
  WaQuote quote_;
  Keyboard kb_;
};

// ===================================================================================================
// A voice message: the microphone while this screen is open; center sends it, Menu throws it away

class RecordScreen : public Screen {
 public:
  RecordScreen(std::string jid, std::string name) : jid_(std::move(jid)), name_(std::move(name)) {
    voiceStop();
    std::string dir = "/storage/sdcard0/.rondo/whatsapp";
    if (!(g_library.presentRoots() & 1)) dir = g_prefsPath.substr(0, g_prefsPath.rfind('/'));
    mkdir("/storage/sdcard0/.rondo", 0777);
    mkdir(dir.c_str(), 0777);
    file_ = dir + "/voice-" + std::to_string(time(nullptr)) + ".wav";
    g_player.recordStart(file_);
    started_ = now_ns();
  }
  ~RecordScreen() override {
    if (!done_) {
      g_player.recordStop(false);
      unlink(file_.c_str());
    }
  }
  std::string title() const override { return "Voice message"; }
  bool statusBar() const override { return false; }
  bool keepScreenOn() const override { return true; }
  bool onAction(Action a, int) override {
    if (a == Action::Select && !stopping_) {
      PlayerState s = g_player.state();
      if (!s.recording || s.recordMs < 800) return true;  // nothing yet
      seconds_ = int((s.recordMs + 500) / 1000);
      stopping_ = true;
      stopAt_ = now_ns();
      g_player.recordStop(true);
      return true;
    }
    if (a == Action::Back) {
      g_player.recordStop(false);
      done_ = true;
      unlink(file_.c_str());
      g_app->pop();
      return true;
    }
    return a == Action::Down || a == Action::Up || a == Action::PlayPause;
  }
  bool tick(int64_t now) override {
    PlayerState s = g_player.state();
    if (s.recording) everRecorded_ = true;
    // the longest it records ended it: sent as it is
    if (!stopping_ && everRecorded_ && !s.recording && s.recordMs > 800) {
      seconds_ = int((s.recordMs + 500) / 1000);
      stopping_ = true;
      stopAt_ = now;
      g_player.recordStop(true);
    }
    // stopped (the file's header written): sent
    if (stopping_ && !done_ && !s.recording && now - stopAt_ > 300 * kMs) {
      done_ = true;
      waClient().sendVoice(jid_, file_, seconds_);
      g_app->pop();
      return true;
    }
    return true;  // the time and the level move
  }
  int64_t wakeAt() const override { return now_ns() + 100 * kMs; }
  void draw(Env& e, float w, float h) override {
    PlayerState s = g_player.state();
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, kBg);
    waHeader(e, w, name_, "Voice message");
    float top = float(e.dp(31));
    if (s.recordError && !s.recording) {
      centered(e, w, top, h - top, "The microphone couldn\xe2\x80\x99t be opened",
               "Error " + std::to_string(s.recordError) + ". Menu goes back.");
      return;
    }
    float cx = w / 2, cy = top + (h - top) * 0.42f;
    // the microphone in a circle that breathes with the sound
    float base = float(e.dp(64)), grow = base * 0.45f * clamp01(s.recordLevel / 60.f);
    e.c.layer(1);
    e.c.shape({cx - (base + grow) / 2, cy - (base + grow) / 2, base + grow, base + grow}, (base + grow) / 2, 0x3300a884);
    e.c.layer(2);
    e.c.shape({cx - base / 2, cy - base / 2, base, base}, base / 2, stopping_ ? kSel : kGreen);
    int g = e.dp(30);
    e.c.layer(4);
    e.icons.draw(e.c, Icon::Mic, {cx - g / 2.f, cy - g / 2.f, float(g), float(g)}, rgb(0xffffff));
    std::string t = formatTime(int(s.recordMs / 1000));
    Label tl = label(e, stopping_ ? "Sending\xe2\x80\xa6" : t, 18, true);
    float ty = cy + base / 2 + grow / 2 + e.dp(10);
    drawLabel(e, tl, centerText(0, w, tl.w), ty, kText);
    bool on = s.recording && (now_ns() - started_) / 500000000 % 2 == 0;
    if (s.recording) {
      int d = e.dp(8);
      e.c.layer(2);
      e.c.shape({centerText(0, w, tl.w) - d - e.dp(6), ty + float((tl.h - d) / 2), float(d), float(d)}, d / 2.f, on ? kRed : 0x55f15c6d);
    }
    e.c.layer(4);
    Label hint = label(e, s.recording ? "center: send \xc2\xb7 Menu: throw away" : "Starting the microphone\xe2\x80\xa6", 10, false, w - e.dp(20));
    drawLabel(e, hint, centerText(0, w, hint.w), h - hint.h - e.dp(10), kMuted);
  }

 private:
  std::string jid_, name_, file_;
  int64_t started_ = 0, stopAt_ = 0;
  int seconds_ = 0;
  bool stopping_ = false, done_ = false, everRecorded_ = false;
};

// ===================================================================================================
// A photo or a video from the card: the newest first; center shows it, center again sends it

struct Pick {
  std::string path;
  int64_t modified = 0;  // s
  bool video = false;
  int seconds = -1;
};

class SendPreviewScreen : public Screen {
 public:
  SendPreviewScreen(std::string jid, std::string name, Pick p) : jid_(std::move(jid)), name_(std::move(name)), pick_(std::move(p)) {
    if (pick_.video) return;
    std::string path = pick_.path;
    auto big = big_;
    std::thread([big, path] {
      auto px = std::make_shared<ui::Pixels>();
      std::vector<uint8_t> bytes;
      if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (48 << 20)) {
          bytes.resize(size_t(n));
          if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        }
        fclose(f);
      }
      int pw = 0, ph = 0;
      if (!bytes.empty() && ui::photoSize(bytes.data(), bytes.size(), pw, ph) && pw > 0 && ph > 0) {
        float k = std::min(1.f, std::min(480.f / pw, 300.f / ph));
        if (!ui::decodePhoto(bytes.data(), bytes.size(), std::max(1, int(pw * k + 0.5f)), std::max(1, int(ph * k + 0.5f)), *px)) px->w = 0;
      }
      std::lock_guard<std::mutex> l(big->mu);
      big->px = px;
      big->done = true;
    }).detach();
  }
  ~SendPreviewScreen() override {
    if (tex_.id) ui::destroyTexture(tex_);
  }
  bool tick(int64_t) override {
    std::lock_guard<std::mutex> l(big_->mu);
    return big_->done && !shown_;
  }
  std::string title() const override { return "Send"; }
  bool statusBar() const override { return false; }
  bool onAction(Action a, int) override {
    if (a == Action::Select) {
      waClient().sendMedia(jid_, pick_.path, pick_.video);
      g_app->popTo(name_);
      return true;
    }
    return a == Action::Down || a == Action::Up;
  }
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, rgb(0x000000));
    waHeader(e, w, "Send to " + name_, pick_.video ? "Video" : "Photo");
    float top = float(e.dp(31)), bottom = float(e.dp(28));
    Rect area{0, top, w, h - top - bottom};
    if (!pick_.video) {
      if (!tex_.id) {
        std::lock_guard<std::mutex> l(big_->mu);
        if (big_->done && big_->px && big_->px->w > 0)
          tex_ = ui::createTexture(big_->px->w, big_->px->h, GL_RGBA, GL_UNSIGNED_BYTE, big_->px->rgba.data(), true);
        if (big_->done) shown_ = true;
      }
      if (tex_.id) {  // the whole photo, fitted
        float k = std::min(area.w / tex_.w, area.h / tex_.h);
        float pw = tex_.w * k, ph = tex_.h * k;
        e.c.layer(3);
        e.c.image(tex_, {area.x + (area.w - pw) / 2, area.y + (area.h - ph) / 2, pw, ph}, {0, 0, 1, 1});
      } else if (shown_) {
        centered(e, w, area.y, area.h, "This photo can\xe2\x80\x99t be shown", "It can still be sent.");
      }
    } else {
      float a = 0;
      int tw = int(area.w), th = int(area.h);
      capped(tw, th);
      if (const Texture* t = e.covers.get(pick_.path, tw, th, 0, e.now, &a)) {
        e.c.layer(3);
        e.c.image(*t, area, {0, 0, 1, 1}, 0, withAlpha(0xffffffff, a));
      }
    }
    if (pick_.video) {
      int g = e.dp(40);
      e.c.layer(4);
      e.c.shape({w / 2 - g / 2.f - e.dp(6), area.y + area.h / 2 - g / 2.f - e.dp(6), float(g + e.dp(12)), float(g + e.dp(12))}, (g + e.dp(12)) / 2.f,
                0x88000000);
      e.c.layer(5);
      e.icons.draw(e.c, Icon::Play, {w / 2 - g / 2.f, area.y + area.h / 2 - g / 2.f, float(g), float(g)}, rgb(0xffffff));
    }
    e.c.layer(4);
    std::string size;
    struct stat st;
    if (stat(pick_.path.c_str(), &st) == 0) size = " \xc2\xb7 " + bytes(uint64_t(st.st_size));
    Label hint = label(e, "center: send" + size + " \xc2\xb7 Menu: back", 10, false, w - e.dp(20));
    drawLabel(e, hint, centerText(0, w, hint.w), h - bottom + float((int(bottom) - hint.h) / 2), kMuted);
    e.c.layer(0);
  }

 private:
  struct Big {
    std::mutex mu;
    std::shared_ptr<ui::Pixels> px;
    bool done = false;
  };
  std::string jid_, name_;
  Pick pick_;
  std::shared_ptr<Big> big_ = std::make_shared<Big>();
  ui::Texture tex_{};
  bool shown_ = false;
};

class PickScreen : public Screen {
 public:
  PickScreen(std::string jid, std::string name) : jid_(std::move(jid)), name_(std::move(name)) {
    // the card's videos (the library knows them) and its photos (looked for on a thread)
    for (const Track* t : g_library.videos())
      if (t && !t->gone) all_.push_back({t->path, t->modified / 1000, true, t->durationSec});
    auto alive = alive_;
    std::thread([alive] {
      auto photos = std::make_shared<std::vector<std::pair<std::string, int64_t>>>();
      findPhotos(*photos);
      postToMain([alive, photos] {
        if (!*alive) return;
        PickScreen* self = alive->self;
        for (auto& p : *photos) self->all_.push_back({p.first, p.second, false, -1});
        std::stable_sort(self->all_.begin(), self->all_.end(), [](const Pick& a, const Pick& b) { return a.modified > b.modified; });
        self->found_ = true;
        self->count_ = -1;
      });
    }).detach();
    alive_->self = this;
  }
  ~PickScreen() override { *alive_ = false; }
  std::string title() const override { return "Photo or video"; }
  bool statusBar() const override { return false; }
  bool onAction(Action a, int steps) override {
    switch (a) {
      case Action::Down: return list_.move(steps), true;
      case Action::Up: return list_.move(-steps), true;
      case Action::Select: {
        int i = list_.selected();
        if (found_ && i >= 0 && i < int(all_.size())) g_app->push(std::unique_ptr<Screen>(new SendPreviewScreen(jid_, name_, all_[size_t(i)])));
        return true;
      }
      default: return false;
    }
  }
  bool step(float dt) override { return list_.step(dt); }
  bool tick(int64_t) override {
    bool c = count_ < 0 && found_;
    return c;
  }
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, kBg);
    waHeader(e, w, "Send to " + name_, "Photo or video from the card");
    float top = float(e.dp(31));
    if (!found_) return centered(e, w, top, h - top, "Looking on the card\xe2\x80\xa6");
    if (all_.empty()) return centered(e, w, top, h - top, "No photos or videos on the card");
    float rowH = float(e.dp(48)), viewH = h - top;
    if (count_ != int(all_.size())) {
      list_.configure(int(all_.size()), rowH, viewH);
      list_.select(0);
      count_ = int(all_.size());
    }
    e.c.pushClip({0, top, w, viewH});
    for (int i = list_.first(); i <= list_.last(); i++) {
      const Pick& p = all_[size_t(i)];
      float y = top + i * rowH - list_.scroll();
      if (i == list_.selected()) {
        e.c.layer(1);
        e.c.shape({float(e.dp(4)), y + 1, w - e.dp(8), rowH - 2}, e.dpf(8), kSel);
      }
      int a = e.dp(40);
      Rect r{float(e.dp(10)), y + float((int(rowH) - a) / 2), float(a), float(a)};
      e.c.layer(2);
      e.c.shape(r, e.dpf(4), kIn);
      float al = 0;
      if (const Texture* t = e.covers.get(p.path, a, a, 0, e.now, &al)) {
        e.c.layer(3);
        e.c.image(*t, r, {0, 0, 1, 1}, e.dpf(4), withAlpha(0xffffffff, al));
      }
      float x = r.x + a + e.dp(10);
      e.c.layer(4);
      std::string name = p.path.substr(p.path.rfind('/') + 1);
      Label n = label(e, name, 12, true, w - x - e.dp(10));
      std::string sub = std::string(p.video ? "Video" : "Photo") + (p.video && p.seconds > 0 ? " " + formatTime(p.seconds) : "") + " \xc2\xb7 " +
                        clockDate(time_t(p.modified), false);
      Label s = label(e, sub, 10, false, w - x - e.dp(10));
      float ty = y + float((int(rowH) - n.h - s.h - e.dp(1)) / 2);
      drawLabel(e, n, x, ty, kText);
      drawLabel(e, s, x, ty + n.h + e.dp(1), kMuted);
      if (p.video) {
        int g = e.dp(14);
        e.icons.drawInTile(e.c, Icon::Play, r, float(g), rgb(0xffffff));
      }
    }
    e.c.popClip();
    e.c.layer(0);
  }

 private:
  struct Alive {
    bool on = true;
    PickScreen* self = nullptr;
    explicit operator bool() const { return on; }
    Alive& operator=(bool b) {
      on = b;
      return *this;
    }
  };
  std::string jid_, name_;
  std::vector<Pick> all_;
  bool found_ = false;
  int count_ = -1;
  WheelList list_;
  std::shared_ptr<Alive> alive_ = std::make_shared<Alive>();
};

// ===================================================================================================
// A message's menu (held center, or center on a text): Reply, open what it holds, the sender's profile

class MessageMenu : public Screen {
 public:
  struct Entry {
    std::string label;
    Icon icon;
    std::function<void()> run;
  };
  MessageMenu(std::string title, std::vector<Entry> entries) : title_(std::move(title)), entries_(std::move(entries)) {}
  std::string title() const override { return title_; }
  bool statusBar() const override { return false; }
  bool onAction(Action a, int steps) override {
    switch (a) {
      case Action::Down: return list_.move(steps), true;
      case Action::Up: return list_.move(-steps), true;
      case Action::Select: {
        auto run = entries_[size_t(list_.selected())].run;
        g_app->pop();
        run();
        return true;
      }
      default: return false;
    }
  }
  bool step(float dt) override { return list_.step(dt); }
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, kBg);
    waHeader(e, w, title_);
    float top = float(e.dp(31) + e.dp(4)), rowH = float(e.dp(36));
    if (!placed_) list_.configure(int(entries_.size()), rowH, h - top), placed_ = true;
    for (int i = 0; i < int(entries_.size()); i++) {
      float y = top + i * rowH - list_.scroll();
      if (i == list_.selected()) {
        e.c.layer(1);
        e.c.shape({float(e.dp(4)), y + 1, w - e.dp(8), rowH - 2}, e.dpf(8), kSel);
      }
      int g = e.dp(16);
      e.c.layer(4);
      e.icons.draw(e.c, entries_[size_t(i)].icon, {float(e.dp(14)), y + float((int(rowH) - g) / 2), float(g), float(g)}, kGreen);
      Label l = label(e, entries_[size_t(i)].label, 13, false, w - e.dp(50));
      drawLabel(e, l, float(e.dp(40)), y + float((int(rowH) - l.h) / 2), kText);
    }
    e.c.layer(0);
  }

 private:
  std::string title_;
  std::vector<Entry> entries_;
  WheelList list_;
  bool placed_ = false;
};

// ===================================================================================================
// Reacting: any emoji, the usual ones first; mine outlined, and taking it back on top

class ReactScreen : public Screen {
 public:
  ReactScreen(std::string jid, std::string id, std::string mine) : jid_(std::move(jid)), id_(std::move(id)), mine_(std::move(mine)) {
    static const char* kQuick[] = {"\xf0\x9f\x91\x8d", "\xe2\x9d\xa4\xef\xb8\x8f", "\xf0\x9f\x98\x82", "\xf0\x9f\x98\xae",
                                   "\xf0\x9f\x98\xa2", "\xf0\x9f\x99\x8f", "\xf0\x9f\x94\xa5", "\xf0\x9f\x91\x8f"};
    for (const char* q : kQuick) emoji_.push_back(q);
    for (const std::string& e : Keyboard::emojiList())
      if (std::find(emoji_.begin(), emoji_.end(), e) == emoji_.end()) emoji_.push_back(e);
    // on mine to start with, else on the first
    if (!mine_.empty()) {
      auto at = std::find(emoji_.begin(), emoji_.end(), mine_);
      sel_ = at == emoji_.end() ? 0 : int(at - emoji_.begin()) + 1;
    }
  }
  std::string title() const override { return "React"; }
  bool statusBar() const override { return false; }
  bool onAction(Action a, int steps) override {
    int n = int(emoji_.size()) + (mine_.empty() ? 0 : 1);
    switch (a) {
      case Action::Down: sel_ = std::min(n - 1, sel_ + steps); return true;
      case Action::Up: sel_ = std::max(0, sel_ - steps); return true;
      case Action::Next:
      case Action::LongNext: sel_ = std::min(n - 1, sel_ + kCols); return true;
      case Action::Prev:
      case Action::LongPrev: sel_ = std::max(0, sel_ - kCols); return true;
      case Action::Select: {
        bool back = !mine_.empty() && sel_ == 0;
        std::string e = back ? "" : emoji_[size_t(sel_ - (mine_.empty() ? 0 : 1))];
        if (e == mine_) e = "";  // mine again: taken back
        waClient().react(jid_, id_, e);
        g_app->pop();
        return true;
      }
      default: return false;
    }
  }
  bool step(float dt) override { return scroll_.step(scrollTo_, dt, 30); }
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, kBg);
    waHeader(e, w, "React", mine_.empty() ? "Pick an emoji" : "Center on yours takes it back");
    float top = float(e.dp(31)), pad = float(e.dp(6));
    float cell = floorf((w - 2 * pad) / kCols), rowH = cell;
    float y0 = top + pad;
    float headH = mine_.empty() ? 0 : float(e.dp(30)) + pad;
    // the selected row whole in view
    int row = !mine_.empty() && sel_ == 0 ? -1 : (sel_ - (mine_.empty() ? 0 : 1)) / kCols;
    float viewH = h - y0 - pad;
    float selTop = row < 0 ? 0 : headH + row * rowH, selBottom = row < 0 ? headH : selTop + rowH;
    if (selTop < scrollTo_) scrollTo_ = selTop;
    if (selBottom > scrollTo_ + viewH) scrollTo_ = selBottom - viewH;
    if (!placed_) scroll_.snap(scrollTo_), placed_ = true;
    e.c.pushClip({0, y0, w, viewH});
    float base = y0 - scroll_.x;
    if (!mine_.empty()) {  // taking mine back
      Rect r{pad, base, w - 2 * pad, float(e.dp(30))};
      e.c.layer(1);
      e.c.shape(r, r.h / 2, sel_ == 0 ? kGreen : kSel);
      e.c.layer(4);
      Label l = label(e, "Take back " + mine_, 12, true);
      drawLabel(e, l, centerText(r.x, r.w, l.w), r.y + float((int(r.h) - l.h) / 2), sel_ == 0 ? rgb(0x0b141a) : kText);
    }
    for (int i = 0; i < int(emoji_.size()); i++) {
      float x = pad + (i % kCols) * cell, y = base + headH + (i / kCols) * rowH;
      if (y + rowH < y0 || y > y0 + viewH) continue;
      int at = i + (mine_.empty() ? 0 : 1);
      Rect r{x + 2, y + 2, cell - 4, rowH - 4};
      if (at == sel_) {
        e.c.layer(1);
        e.c.shape(r, e.dpf(8), 0xff3b4a54);
      }
      if (emoji_[size_t(i)] == mine_) {
        e.c.layer(2);
        e.c.shape({r.x + 3, r.y + 3, r.w - 6, r.h - 6}, e.dpf(7), 0x5525d366);
      }
      e.c.layer(4);
      Label l = label(e, emoji_[size_t(i)], 20, false);
      drawLabel(e, l, centerText(r.x, r.w, l.w), r.y + float((int(r.h) - l.h) / 2), kText);
    }
    e.c.popClip();
    e.c.layer(0);
  }

 private:
  static constexpr int kCols = 8;
  std::string jid_, id_, mine_;
  std::vector<std::string> emoji_;
  int sel_ = 0;
  Spring scroll_;
  float scrollTo_ = 0;
  bool placed_ = false;
};

std::unique_ptr<Screen> makeReact(const std::string& jid, const std::string& id, const std::string& mine) {
  return std::unique_ptr<Screen>(new ReactScreen(jid, id, mine));
}

// ===================================================================================================
// Search: chats and contacts by name (any case, any accents) or by the digits of a number; not
// messages. Type on the keyboard, Search shows what matches; Menu goes back to the keyboard.

class SearchScreen : public Screen {
 public:
  explicit SearchScreen(bool contactsOnly) : contactsOnly_(contactsOnly), kb_("Search") {
    waClient().hold();
    waClient().loadContacts([] {});
  }
  ~SearchScreen() override { waClient().release(); }
  std::string title() const override { return contactsOnly_ ? "Search contacts" : "Search WhatsApp"; }
  bool statusBar() const override { return false; }
  bool onAction(Action a, int steps) override {
    if (showing_) {
      switch (a) {
        case Action::Back: showing_ = false; return true;
        case Action::Down: return list_.move(steps), true;
        case Action::Up: return list_.move(-steps), true;
        case Action::Select:
        case Action::LongSelect: {
          int i = list_.selected();
          if (i < 0 || i >= int(found_.size())) return true;
          const Found& f = found_[size_t(i)];
          g_app->push(a == Action::Select ? makeChat(f.jid, f.name, f.group) : makeProfile(f.jid, f.name, f.group, true));
          return true;
        }
        default: return false;
      }
    }
    switch (a) {  // the keyboard takes every control; Menu on its top row leaves
      case Action::Down: kb_.nextKey(); return true;
      case Action::Up: kb_.prevKey(); return true;
      case Action::Next:
      case Action::LongNext: kb_.right(); return true;
      case Action::Prev:
      case Action::LongPrev: kb_.left(); return true;
      case Action::PlayPause: kb_.down(); return true;
      case Action::Back: return kb_.up();
      case Action::Select: {
        Keyboard::Result r = kb_.press();
        if (r == Keyboard::Result::Typed) kb_.keepAtMost(40);
        if (r == Keyboard::Result::Action) search();
        return true;
      }
      default: return false;
    }
  }
  bool step(float dt) override { return showing_ ? list_.step(dt) : kb_.step(dt); }
  bool tick(int64_t) override {
    voiceTick();
    if (waClient().serial() == seen_) return false;
    seen_ = waClient().serial();
    if (showing_ && waiting_ && waClient().contactsLoaded()) search();  // the contacts came after Search
    return true;
  }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, kBg);
    waHeader(e, w, title());
    float bx = float(e.dp(8)), by = float(e.dp(31) + e.dp(4)), bw = w - 2 * bx, bh = float(e.dp(28));
    e.c.layer(2);
    e.c.shape({bx, by, bw, bh}, e.dpf(14), p.raised);
    e.c.layer(4);
    int icon = e.dp(12);
    float ix = bx + e.dp(10);
    e.icons.draw(e.c, Icon::Search, {ix, by + float((int(bh) - icon) / 2), float(icon), float(icon)}, kMuted);
    float fx = ix + icon + e.dp(8), fw = bx + bw - e.dp(10) - fx;
    bool empty = kb_.typed().empty() && !showing_;
    Label field = label(e, empty ? "|  A name or a number" : showing_ ? kb_.typed() : kb_.typed() + "|", 14, false, fw);
    drawLabel(e, field, fx, by + float((int(bh) - field.h) / 2), empty ? kMuted : kText);
    float area = by + bh + e.dp(5);
    if (showing_) {
      float rowH = float(e.dp(42)), viewH = h - area;
      if (found_.empty()) {
        centered(e, w, area, viewH, waiting_ ? "Looking\xe2\x80\xa6" : "Nothing found", waiting_ ? "" : "Names and numbers only, not messages.");
        return;
      }
      if (configured_ != int(found_.size())) list_.configure(int(found_.size()), rowH, viewH), list_.select(0), configured_ = int(found_.size());
      e.c.pushClip({0, area, w, viewH});
      for (int i = list_.first(); i <= list_.last(); i++) {
        const Found& f = found_[size_t(i)];
        float y = area + i * rowH - list_.scroll();
        if (i == list_.selected()) {
          e.c.layer(1);
          e.c.shape({float(e.dp(4)), y + 1, w - e.dp(8), rowH - 2}, e.dpf(8), kSel);
        }
        int a = e.dp(30);
        avatar(e, f.jid, f.group, {float(e.dp(10)), y + float((int(rowH) - a) / 2), float(a), float(a)});
        float x = float(e.dp(10) + a + e.dp(10));
        e.c.layer(4);
        Label n = label(e, f.name, 13, true, w - x - e.dp(10));
        Label s = label(e, f.sub, 10, false, w - x - e.dp(10));
        float ty = y + float((int(rowH) - n.h - s.h - e.dp(1)) / 2);
        drawLabel(e, n, x, ty, kText);
        drawLabel(e, s, x, ty + n.h + e.dp(1), kMuted);
      }
      e.c.popClip();
      e.c.layer(0);
      return;
    }
    Label hint = label(e, "Menu up \xc2\xb7 Play down \xc2\xb7 Prev left \xc2\xb7 Next right \xc2\xb7 wheel: next key \xc2\xb7 center: type", 9,
                       false, w);
    float kh = h - area - (e.dp(3) + hint.h + e.dp(4));
    e.c.layer(1);
    kb_.draw(e.c, e.text, p, {float(e.dp(5)), area, w - 2 * e.dp(5), kh}, e.dp(15), e.dp(12));
    e.c.layer(4);
    drawLabel(e, hint, centerText(0, w, hint.w), area + kh + e.dp(3), kMuted);
    e.c.layer(0);
  }

 private:
  struct Found {
    std::string jid, name, sub;
    bool group = false;
  };
  static std::string digitsOf(const std::string& s) {
    std::string d;
    for (char c : s)
      if (c >= '0' && c <= '9') d += c;
    return d;
  }
  // a name containing the words typed (any case or accents), or a number containing the digits typed
  static bool matches(const std::string& q, const std::string& qd, const std::string& name, const std::string& name2, const std::string& number) {
    if (!q.empty() && (fold(name).find(q) != std::string::npos || (!name2.empty() && fold(name2).find(q) != std::string::npos))) return true;
    return qd.size() >= 3 && digitsOf(number).find(qd) != std::string::npos;
  }
  void search() {
    std::string typed = kb_.typed();
    size_t a = typed.find_first_not_of(' '), b = typed.find_last_not_of(' ');
    if (a == std::string::npos) return;
    typed = typed.substr(a, b - a + 1);
    // digits alone (with + - ( ) and spaces): a number; anything else: a name
    bool number = typed.find_first_not_of("0123456789+-() ") == std::string::npos;
    std::string q = number ? "" : fold(typed), qd = number ? digitsOf(typed) : "";
    found_.clear();
    std::vector<std::string> seen;
    if (!contactsOnly_)
      for (const WaChat& c : waClient().chats()) {
        std::string num = c.group ? "" : c.jid.substr(0, c.jid.find('@'));
        if (!matches(q, qd, c.name, "", c.jid.find("@s.whatsapp.net") != std::string::npos ? num : "")) continue;
        found_.push_back({c.jid, c.name, c.group ? "Group" : c.hasPreview ? c.preview.text : "+" + num, c.group});
        seen.push_back(c.jid);
      }
    for (const WaContact& c : waClient().contacts()) {
      if (std::find(seen.begin(), seen.end(), c.jid) != seen.end()) continue;
      if (!matches(q, qd, c.name, c.pushName, c.number)) continue;
      found_.push_back({c.jid, c.name, c.number, false});
    }
    waiting_ = !waClient().contactsLoaded();
    showing_ = true;
    configured_ = -1;
  }
  bool contactsOnly_;
  Keyboard kb_;
  bool showing_ = false, waiting_ = false;
  std::vector<Found> found_;
  WheelList list_;
  int configured_ = -1;
  uint32_t seen_ = 0;
};

std::unique_ptr<Screen> makeSearch(bool contactsOnly) { return std::unique_ptr<Screen>(new SearchScreen(contactsOnly)); }

// ===================================================================================================
// A chat

class ChatScreen : public Screen {
 public:
  ChatScreen(std::string jid, std::string name, bool group) : jid_(std::move(jid)), name_(std::move(name)), group_(group) {
    waClient().hold();
    waClient().open(jid_);
  }
  ~ChatScreen() override {
    *alive_ = false;
    waClient().close(jid_);
    waClient().release();
  }
  std::string title() const override { return name_; }
  bool statusBar() const override { return false; }
  bool waitsForContent() const override { return true; }
  void onShow() override { seen_ = 0; }
  bool onAction(Action a, int steps) override {
    int n = int(stops_.size());
    switch (a) {
      case Action::Down:
      case Action::Up: {
        if (!n) return true;
        int at = std::max(0, std::min(n - 1, sel_ + (a == Action::Down ? steps : -steps)));
        if (at != sel_) {
          sel_ = at;
          follow_ = sel_ >= n - 3;  // on the buttons: new messages keep the view at the bottom
          // near the top: the earlier messages
          if (sel_ < 3) waClient().older(jid_);
        }
        return true;
      }
      case Action::Select: return activate(false), true;
      case Action::LongSelect: return activate(true), true;
      default: return false;
    }
  }
  bool step(float dt) override { return scroll_.step(scrollTo_, dt, 30); }
  bool tick(int64_t) override {
    bool v = voiceTick();
    if (waClient().serial() == seen_) return v || fetchingAny_;
    seen_ = waClient().serial();
    rebuild_ = true;
    return true;
  }
  int64_t wakeAt() const override { return g_voice.on || fetchingAny_ ? now_ns() + 200 * kMs : 0; }
  void draw(Env& e, float w, float h) override;

 private:
  // one line of the list: a day, a message, the earlier ones loading; then the buttons (not in it)
  struct Item {
    enum Kind { Day, Msg, Older } kind = Day;
    int msg = -1;  // in the chat's messages
    std::string text;
    float y = 0, h = 0;
  };
  struct Bubble {  // a message laid out at a width
    std::vector<std::string> lines;
    float w = 0, h = 0, mediaW = 0, mediaH = 0, nameH = 0, quoteH = 0, textH = 0, footerW = 0;
    bool footerInline = false;
  };
  void layout(Env& e, float w);
  Bubble lay(Env& e, const WaMessage& m, float maxW);
  void drawBubble(Env& e, const WaMessage& m, const Bubble& b, float w, float y, bool selected);
  void activate(bool menu);
  void openMedia(const WaMessage& m);
  const WaMessage* selectedMessage() const {
    const auto* all = waClient().messages(jid_);
    if (!all || sel_ < 0 || sel_ >= int(stops_.size()) || stops_[size_t(sel_)] < 0) return nullptr;
    const Item& it = items_[size_t(stops_[size_t(sel_)])];
    return it.msg >= 0 && it.msg < int(all->size()) ? &(*all)[size_t(it.msg)] : nullptr;
  }
  int button() const { return sel_ >= 0 && sel_ < int(stops_.size()) && stops_[size_t(sel_)] < 0 ? -stops_[size_t(sel_)] - 1 : -1; }

  std::string jid_, name_;
  bool group_;
  std::vector<Item> items_;
  std::vector<int> stops_;  // what the wheel stops at: an item, or -1, -2, -3 for the buttons
  std::map<std::string, Bubble> bubbles_;  // by id, for the width laid out at
  float laidW_ = 0, contentH_ = 0;
  int sel_ = -1;
  bool follow_ = true, rebuild_ = true, fetchingAny_ = false, placed_ = false;
  std::string selId_;  // the message selected, kept across a rebuild
  Spring scroll_;
  float scrollTo_ = 0;
  uint32_t seen_ = 0;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

ChatScreen::Bubble ChatScreen::lay(Env& e, const WaMessage& m, float maxW) {
  Bubble b;
  float padX = float(e.dp(7)), inner = maxW - 2 * padX;
  float minW = float(e.dp(60));
  float textW = 0;
  if (group_ && !m.fromMe) b.nameH = float(e.text.lineHeight(e.font(11, true)) + e.dp(1));
  if (m.quoted) b.quoteH = float(e.text.lineHeight(e.font(10, true)) + e.text.lineHeight(e.font(10, false)) + e.dp(8));
  if (m.kind == "image" || m.kind == "video" || m.kind == "sticker") {
    float mw = std::min(inner, float(e.dp(m.kind == "sticker" ? 110 : 180)));
    float ratio = m.width > 0 && m.height > 0 ? float(m.height) / float(m.width) : 0.75f;
    b.mediaW = mw;
    b.mediaH = std::max(float(e.dp(60)), std::min(float(e.dp(m.kind == "sticker" ? 110 : 200)), roundf(mw * ratio)));
    textW = mw;
  } else if (m.kind == "voice" || m.kind == "audio" || m.kind == "document") {
    b.mediaW = std::min(inner, float(e.dp(190)));
    b.mediaH = float(e.dp(30));
    textW = b.mediaW;
  }
  // the text: a message's, a photo's caption, or what it is (a location, a poll…)
  std::string text = m.kind == "text" || m.kind == "image" || m.kind == "video" ? m.text
                     : m.kind == "voice" || m.kind == "audio" || m.kind == "document" || m.kind == "sticker" ? ""
                                                                                                              : WaClient::summary(m);
  int font = e.font(12, false);
  size_t at = 0;
  while (!text.empty() && at <= text.size()) {  // paragraph by paragraph
    size_t nl = text.find('\n', at);
    std::string para = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    if (para.empty()) b.lines.push_back("");
    for (auto& l : wrapText(e, para, 12, false, inner, 1000)) b.lines.push_back(l.text);
    if (nl == std::string::npos) break;
    at = nl + 1;
  }
  if (b.lines.size() > 60) b.lines.resize(60), b.lines.back() += "\xe2\x80\xa6";
  for (const std::string& l : b.lines) textW = std::max(textW, e.text.run(font, l).width);
  b.textH = float(b.lines.size() * e.text.lineHeight(font));
  // the footer: the time, "edited", the ticks
  std::string foot = (m.edited ? "edited " : "") + timeOf(m.at);
  b.footerW = e.text.run(e.font(9, false), foot).width + (m.fromMe ? e.dp(17) : 0);
  float lastW = b.lines.empty() ? 0 : e.text.run(font, b.lines.back()).width;
  b.footerInline = !b.lines.empty() && b.mediaH == 0 && lastW + e.dp(8) + b.footerW <= inner;
  float nameW = 0;
  if (b.nameH > 0) nameW = std::min(inner, e.text.run(e.font(11, true), m.name).width);
  float wq = b.quoteH > 0 ? std::min(inner, float(e.dp(150))) : 0;
  float contentW = std::max({textW + (b.footerInline ? e.dp(8) + b.footerW : 0), b.footerW, nameW, wq, b.mediaW});
  b.w = std::max(minW, std::min(maxW, contentW + 2 * padX));
  float footH = float(e.text.lineHeight(e.font(9, false)));
  b.h = e.dp(5) + b.nameH + b.quoteH + (b.mediaH > 0 ? b.mediaH + e.dp(3) : 0) + b.textH + (b.footerInline ? 0 : footH) + e.dp(4);
  return b;
}

void ChatScreen::layout(Env& e, float w) {
  const auto* all = waClient().messages(jid_);
  std::string keep = selId_;
  int keepButton = button();
  items_.clear();
  stops_.clear();
  fetchingAny_ = false;
  if (laidW_ != w) bubbles_.clear(), laidW_ = w;
  float y = float(e.dp(6));
  if (all) {
    if (waClient().olderToCome(jid_)) {
      Item it;
      it.kind = Item::Older;
      it.text = waClient().loadingOlder(jid_) ? "Loading earlier messages\xe2\x80\xa6" : "Earlier messages";
      it.y = y, it.h = float(e.dp(24));
      items_.push_back(it);
      y += it.h;
    }
    int64_t day = -1;
    std::string lastSender;
    for (int i = 0; i < int(all->size()); i++) {
      const WaMessage& m = (*all)[size_t(i)];
      int64_t d = dayOf(m.at);
      if (d != day) {
        Item it;
        it.kind = Item::Day;
        it.text = dayLabel(m.at);
        it.y = y + e.dp(4), it.h = float(e.dp(22));
        items_.push_back(it);
        y += it.h + e.dp(8);
        day = d;
        lastSender.clear();
      }
      std::string who = m.fromMe ? "me" : m.sender;
      if (!lastSender.empty() && who != lastSender) y += e.dp(4);
      lastSender = who;
      std::string key = m.id + (m.sending ? "s" : "") + (m.failed ? "f" : "") + m.status + std::to_string(m.reactions.size()) + m.text.substr(0, 8);
      auto b = bubbles_.find(key);
      if (b == bubbles_.end()) b = bubbles_.emplace(key, lay(e, m, roundf(w * 0.8f))).first;
      Item it;
      it.kind = Item::Msg;
      it.msg = i;
      it.y = y;
      it.h = b->second.h + (m.reactions.empty() ? 0 : e.dp(13)) + (m.failed ? e.dp(13) : 0);
      it.text = key;
      stops_.push_back(int(items_.size()));
      items_.push_back(it);
      y += it.h + e.dp(3);
      if (waClient().fetching(jid_, m.id)) fetchingAny_ = true;
    }
  }
  contentH_ = y + e.dp(4);
  for (int b = 0; b < 3; b++) stops_.push_back(-b - 1);
  // the selection kept: the same message, or the same button (the first time: Message)
  int n = int(stops_.size());
  sel_ = -1;
  if (!keep.empty() && all)
    for (int s = 0; s < n && sel_ < 0; s++)
      if (stops_[size_t(s)] >= 0 && (*all)[size_t(items_[size_t(stops_[size_t(s)])].msg)].id == keep) sel_ = s;
  if (sel_ < 0) sel_ = n - 3 + std::max(0, keepButton);
}

void ChatScreen::drawBubble(Env& e, const WaMessage& m, const Bubble& b, float w, float y, bool selected) {
  float margin = float(e.dp(8));
  float x = m.fromMe ? w - margin - b.w : margin;
  Rect r{x, y, b.w, b.h};
  Color fill = m.fromMe ? kOut : kIn;
  if (selected) {
    e.c.layer(1);
    float g = float(e.dp(2));
    e.c.shape({r.x - g, r.y - g, r.w + 2 * g, r.h + 2 * g}, e.dpf(8) + g, kBrand);
  }
  e.c.layer(2);
  e.c.shape(r, e.dpf(8), fill);
  float padX = float(e.dp(7)), ix = x + padX, iw = b.w - 2 * padX;
  float cy = y + e.dp(5);
  e.c.layer(4);
  if (b.nameH > 0) {
    Label n = label(e, m.name.empty() ? "Someone" : m.name, 11, true, iw);
    drawLabel(e, n, ix, cy, kNames[hashOf(m.sender) % 8]);
    cy += b.nameH;
  }
  if (b.quoteH > 0) {
    float qh = b.quoteH - e.dp(4);
    e.c.layer(3);
    e.c.shape({ix, cy, iw, qh}, e.dpf(5), kQuote);
    e.c.fill({ix, cy, float(e.dp(3)), qh}, kGreen);
    e.c.layer(4);
    Label qn = label(e, m.quote.name.empty() ? "Message" : m.quote.name, 10, true, iw - e.dp(12));
    std::string qt = m.quote.text.empty() ? (m.quote.kind == "image" ? "Photo" : m.quote.kind == "video" ? "Video"
                                             : m.quote.kind == "voice"  ? "Voice message"
                                                                        : m.quote.kind)
                                          : m.quote.text;
    Label qtl = label(e, qt, 10, false, iw - e.dp(12));
    drawLabel(e, qn, ix + e.dp(8), cy + e.dp(2), kGreen);
    drawLabel(e, qtl, ix + e.dp(8), cy + e.dp(2) + qn.h, kMuted);
    cy += b.quoteH;
  }
  if (b.mediaH > 0) {
    Rect mr{ix, cy, b.mediaW, b.mediaH};
    bool fetching = waClient().fetching(jid_, m.id);
    if (m.kind == "image" || m.kind == "video" || m.kind == "sticker") {
      e.c.layer(3);
      if (m.kind != "sticker") e.c.shape(mr, e.dpf(5), 0x33000000);
      std::string pic = !m.localFile.empty() ? m.localFile : WaClient::previewPath(jid_, m);
      float a = 0;
      int tw = int(mr.w), th = int(mr.h);
      capped(tw, th);
      if (!pic.empty())
        if (const Texture* t = e.covers.get(pic, tw, th, 0, e.now, &a)) e.c.image(*t, mr, {0, 0, 1, 1}, e.dpf(5), withAlpha(0xffffffff, a));
      if (m.kind == "video" || fetching) {
        int g = e.dp(m.kind == "video" ? 22 : 16);
        float cx = mr.x + mr.w / 2, cyy = mr.y + mr.h / 2, d = float(g + e.dp(14));
        e.c.layer(4);
        e.c.shape({cx - d / 2, cyy - d / 2, d, d}, d / 2, 0x99000000);
        e.c.layer(5);
        e.icons.draw(e.c, fetching ? Icon::Download : Icon::Play, {cx - g / 2.f, cyy - g / 2.f, float(g), float(g)}, rgb(0xffffff));
        if (m.kind == "video" && m.seconds > 0) {
          Label dl = label(e, formatTime(m.seconds), 9, false);
          drawLabel(e, dl, mr.x + e.dp(5), mr.y + mr.h - dl.h - e.dp(4), rgb(0xffffff));
        }
      }
    } else if (m.kind == "voice" || m.kind == "audio") {
      bool playing = voicePlaying(m.id);
      int g = e.dp(24);
      Rect pr{mr.x, mr.y + float((int(mr.h) - g) / 2), float(g), float(g)};
      e.c.layer(3);
      e.c.shape(pr, g / 2.f, m.kind == "voice" ? kGreen : 0xff6b7c85);
      e.c.layer(4);
      int ig = e.dp(12);
      e.icons.draw(e.c, fetching ? Icon::Download : playing ? Icon::Pause : m.kind == "voice" ? Icon::Play : Icon::Headphones,
                   {pr.x + float((g - ig) / 2), pr.y + float((g - ig) / 2), float(ig), float(ig)}, rgb(0xffffff));
      float bx = pr.x + g + e.dp(8), bw = mr.w - g - e.dp(8);
      Label len = label(e, playing ? formatTime(int(PlayerHost::positionAt(g_player.state(), now_ns()) / 1000)) : formatTime(std::max(0, m.seconds)), 9, false);
      float by = mr.y + mr.h / 2 - e.dp(4);
      e.c.layer(3);
      progressBar(e, {bx, by, bw, float(e.dp(6))}, 0x55ffffff, kBlue, playing ? voiceProgress() : 0, 0, 3);
      e.c.layer(4);
      drawLabel(e, len, bx, by + e.dp(7), 0xb3e9edef);
    } else {  // a document: its name and size
      int g = e.dp(20);
      e.c.layer(3);
      e.c.shape(mr, e.dpf(5), kQuote);
      e.c.layer(4);
      e.icons.draw(e.c, Icon::File, {mr.x + e.dp(5), mr.y + float((int(mr.h) - g) / 2), float(g), float(g)}, kMuted);
      Label fn = label(e, m.fileName.empty() ? "Document" : m.fileName, 11, false, mr.w - g - e.dp(14));
      Label fs = label(e, m.size > 0 ? bytes(uint64_t(m.size)) : "Document", 9, false);
      float tx = mr.x + g + e.dp(10);
      drawLabel(e, fn, tx, mr.y + e.dp(3), kText);
      drawLabel(e, fs, tx, mr.y + e.dp(3) + fn.h, kMuted);
    }
    cy += b.mediaH + e.dp(3);
  }
  e.c.layer(4);
  int font = e.font(12, false);
  Color tc = m.kind == "deleted" || m.kind == "unsupported" ? kMuted : kText;
  for (const std::string& l : b.lines) {
    const TextRun& run = e.text.run(font, l, iw);
    e.text.draw(e.c, run, ix, cy + e.text.ascentInt(font), tc);
    cy += e.text.lineHeight(font);
  }
  // the footer at the bottom right
  std::string foot = (m.edited ? "edited " : "") + timeOf(m.at);
  Label fl = label(e, foot, 9, false);
  float fy = y + b.h - fl.h - e.dp(3);
  float fx = x + b.w - padX - b.footerW;
  drawLabel(e, fl, fx, fy, 0x99e9edef);
  if (m.fromMe) drawTicks(e, m.status, m.sending, m.failed, fx + fl.w + e.dp(3), fy + fl.h / 2.f, 0x99e9edef);
  float below = y + b.h;
  if (!m.reactions.empty()) {  // the reactions, hanging under the bubble
    std::string s;
    int total = 0;
    for (const WaReaction& rc : m.reactions) s += rc.emoji, total += rc.count;
    if (total > 1) s += " " + std::to_string(total);
    Label rl = label(e, s, 10, false);
    float rh = float(rl.h + e.dp(4)), rw = float(rl.w + e.dp(10));
    float rx = m.fromMe ? x + b.w - rw - e.dp(6) : x + e.dp(6);
    e.c.layer(3);
    e.c.shape({rx - 1, below - e.dp(4) - 1, rw + 2, rh + 2}, rh / 2 + 1, kBg);
    e.c.shape({rx, below - e.dp(4), rw, rh}, rh / 2, kIn);
    e.c.layer(4);
    drawLabel(e, rl, rx + e.dp(5), below - e.dp(4) + e.dp(2), kText);
    below += e.dp(13);
  }
  if (m.failed) {
    Label fl2 = label(e, "Not sent \xc2\xb7 center sends it again", 9, false);
    drawLabel(e, fl2, m.fromMe ? w - margin - fl2.w : margin, below + e.dp(1), kRed);
  }
}

void ChatScreen::draw(Env& e, float w, float h) {
  const WaClient& c = waClient();
  if (rebuild_ || laidW_ != w) {
    if (const WaMessage* m = selectedMessage()) selId_ = m->id;
    else selId_.clear();
    layout(e, w);
    rebuild_ = false;
  }
  if (const WaMessage* m = selectedMessage()) selId_ = m->id;
  else selId_.clear();
  e.c.layer(0);
  e.c.fill({0, 0, w, h}, kBg);
  const WaChat* ch = c.chat(jid_);
  std::string sub = group_ ? "Group" : "";
  if (c.state() == WaClient::State::Connecting) sub = "Connecting\xe2\x80\xa6";
  if (c.state() == WaClient::State::Offline) sub = "Offline";
  waHeader(e, w, ch && !ch->name.empty() ? ch->name : name_, sub, jid_, group_);
  float top = float(e.dp(31)), barH = float(e.dp(34)), viewH = h - top - barH;
  // the scroll: the selected message whole in view (its top when it's taller than the view), the
  // bottom on the buttons; a short chat sits at the bottom
  float maxScroll = std::max(0.f, contentH_ - viewH);
  float target = maxScroll;
  if (sel_ >= 0 && sel_ < int(stops_.size()) && stops_[size_t(sel_)] >= 0) {
    const Item& it = items_[size_t(stops_[size_t(sel_)])];
    float cur = scrollTo_;
    if (it.y - e.dp(6) < cur) cur = it.y - e.dp(6);
    if (it.y + it.h + e.dp(6) > cur + viewH) cur = std::min(it.y - e.dp(6), it.y + it.h + e.dp(6) - viewH);
    target = std::max(0.f, std::min(maxScroll, cur));
  }
  scrollTo_ = target;
  if (!placed_) scroll_.snap(target), placed_ = true;
  float offset = contentH_ < viewH ? viewH - contentH_ : 0;  // a short chat at the bottom
  const auto* all = c.messages(jid_);
  e.c.pushClip({0, top, w, viewH});
  if (!all) {
    centered(e, w, top, viewH, "Loading\xe2\x80\xa6");
  } else if (all->empty()) {
    centered(e, w, top, viewH, "No messages yet", "Say hello with Message, Voice or Photo below.");
  }
  float base = top + offset - scroll_.x;
  const WaMessage* selected = selectedMessage();
  for (const Item& it : items_) {
    float y = base + it.y;
    if (y + it.h < top || y > top + viewH) continue;
    if (it.kind == Item::Day || it.kind == Item::Older) {
      Label l = label(e, it.text, 9, it.kind == Item::Day);
      float cw = float(l.w + e.dp(16)), chh = float(l.h + e.dp(6));
      e.c.layer(2);
      e.c.shape({float((int(w) - int(cw)) / 2), y, cw, chh}, e.dpf(6), kChip);
      e.c.layer(4);
      drawLabel(e, l, centerText(0, w, l.w), y + e.dp(3), kMuted);
      continue;
    }
    const WaMessage& m = (*all)[size_t(it.msg)];
    auto b = bubbles_.find(it.text);
    if (b == bubbles_.end()) continue;
    drawBubble(e, m, b->second, w, y, selected == &m);
  }
  e.c.popClip();
  // the buttons
  e.c.layer(1);
  e.c.fill({0, h - barH, w, barH}, kBar);
  static const char* kLabels[] = {"Message", "Voice", "Photo"};
  static const Icon kIcons[] = {Icon::Chat, Icon::Mic, Icon::Photo};
  float gap = float(e.dp(6)), bw = (w - 4 * gap) / 3, bh = float(e.dp(24)), by = h - barH + float((int(barH) - int(bh)) / 2);
  for (int i = 0; i < 3; i++) {
    float bx = gap + i * (bw + gap);
    bool on = button() == i;
    e.c.layer(2);
    e.c.shape({bx, by, bw, bh}, bh / 2, on ? kGreen : kSel);
    Label l = label(e, kLabels[i], 11, true);
    int g = e.dp(13);
    float tx = bx + float((int(bw) - (g + e.dp(5) + l.w)) / 2);
    Color col = on ? rgb(0x0b141a) : kText;
    e.c.layer(4);
    e.icons.draw(e.c, kIcons[i], {tx, by + float((int(bh) - g) / 2), float(g), float(g)}, col);
    drawLabel(e, l, tx + g + e.dp(5), by + float((int(bh) - l.h) / 2), col);
  }
  e.c.layer(0);
}

void ChatScreen::activate(bool menu) {
  int b = button();
  std::string name = name_;  // the screens after it come back here by this title (popTo)
  if (b == 0) return g_app->push(std::unique_ptr<Screen>(new ComposeScreen(jid_, name, {})));
  if (b == 1) return g_app->push(std::unique_ptr<Screen>(new RecordScreen(jid_, name)));
  if (b == 2) return g_app->push(std::unique_ptr<Screen>(new PickScreen(jid_, name)));
  const WaMessage* mp = selectedMessage();
  if (!mp) return;
  WaMessage m = *mp;
  std::string jid = jid_;
  if (m.failed && !menu) return waClient().retry(jid_, m.id);
  bool media = m.kind == "image" || m.kind == "video" || m.kind == "voice" || m.kind == "audio" || m.kind == "sticker";
  if (!menu && media) return openMedia(m);
  // the menu
  std::vector<MessageMenu::Entry> entries;
  if (!m.sending && !m.failed && m.kind != "deleted") {
    std::string id = m.id, mine = m.mine;
    entries.push_back({mine.empty() ? "React" : "React (yours: " + mine + ")", Icon::Smile,
                       [jid, id, mine] { g_app->push(makeReact(jid, id, mine)); }});
  }
  if (!m.sending && !m.failed) {
    WaQuote q{m.id, m.sender, m.fromMe ? "You" : m.name.empty() ? name : m.name, m.kind, m.kind == "text" ? m.text : WaClient::summary(m)};
    entries.push_back({"Reply", Icon::Chat, [jid, name, q] { g_app->push(std::unique_ptr<Screen>(new ComposeScreen(jid, name, q))); }});
  }
  if (media) {
    std::string verb = m.kind == "voice" || m.kind == "audio" ? (voicePlaying(m.id) ? "Stop" : "Play") : "Open";
    entries.push_back({verb, m.kind == "image" || m.kind == "sticker" ? Icon::Photo : m.kind == "video" ? Icon::Film : Icon::Play,
                       [this, m] { openMedia(m); }});
  }
  if (m.failed) entries.push_back({"Send again", Icon::Refresh, [jid, m] { waClient().retry(jid, m.id); }});
  if (group_ && !m.fromMe && !m.sender.empty()) {
    std::string who = m.sender, wn = m.name;
    entries.push_back({"Profile of " + (wn.empty() ? std::string("the sender") : wn), Icon::Person,
                       [who, wn] { g_app->push(makeProfile(who, wn, false, true)); }});
  }
  bool group = group_;
  entries.push_back({group_ ? "Group info" : "Profile", group_ ? Icon::People : Icon::Person,
                     [jid, name, group] { g_app->push(makeProfile(jid, name, group, false)); }});
  g_app->push(std::unique_ptr<Screen>(new MessageMenu(WaClient::summary(m).substr(0, 60), std::move(entries))));
}

void ChatScreen::openMedia(const WaMessage& m) {
  if ((m.kind == "voice" || m.kind == "audio") && voicePlaying(m.id)) return voiceStop();
  std::string jid = jid_, id = m.id, kind = m.kind, title = WaClient::summary(m);
  auto alive = alive_;
  Screen* self = this;
  waClient().media(jid_, m, [jid, id, kind, title, alive, self](const std::string& file, const std::string& problem) {
    if (file.empty()) return g_app->toast(problem);
    // it came while its chat is still in front: opened (another screen looked at meanwhile isn't interrupted)
    if (!*alive || g_app->top() != self) return;
    if (kind == "image" || kind == "sticker") {
      g_app->push(makePhotoViewer({file}, 0));
    } else if (kind == "video") {
      voiceStop();
      rondo::Item it;
      it.kind = "video";
      it.id = file;
      it.title = title;
      openVideo(it);
    } else {
      voicePlay(jid, id, file);
    }
  });
  rebuild_ = true;
}

std::unique_ptr<Screen> makeChat(const std::string& jid, const std::string& name, bool group) {
  return std::unique_ptr<Screen>(new ChatScreen(jid, name, group));
}

std::unique_ptr<Screen> makeProfile(const std::string& jid, const std::string& name, bool group, bool offerChat) {
  return std::unique_ptr<Screen>(new ProfileScreen(jid, name, group, offerChat));
}

}  // namespace

std::unique_ptr<Screen> makeWhatsApp() { return std::unique_ptr<Screen>(new ChatsScreen()); }

}  // namespace rondo::shell
