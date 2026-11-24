// Songs the server streams, as the player's queue holds them: "/s/<id>/140" paths (1.0.x's
// audioMediaItem: AAC from YouTube Music through the server's range proxy), with what the screens show
// about each (title, subtitle, artwork), kept here since the library knows nothing of them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>

#include "../core/api.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {
namespace {
std::map<std::string, Item>& registry() {
  static std::map<std::string, Item> r;
  return r;
}
}  // namespace

std::string streamPath(const Item& it) { return "/s/" + it.id + "/140"; }

bool isStream(const std::string& path) { return path.compare(0, 3, "/s/") == 0; }

const Item* streamItem(const std::string& path) {
  auto it = registry().find(path);
  return it == registry().end() ? nullptr : &it->second;
}

void playStreams(const std::vector<Item>& items, int start) {
  std::vector<std::string> paths;
  for (const Item& it : items) {
    std::string p = streamPath(it);
    registry()[p] = it;
    paths.push_back(p);
  }
  g_player.playQueue(paths, start);
}

// largeThumb(): YouTube Music thumbnails are URLs like ...=w120-h120-l90-rj: a bigger one for the large
// art views (the server still resizes it to the width asked)
std::string largeThumb(const std::string& thumb, int size) {
  static const std::string prefix = "/thumb?u=";
  if (thumb.compare(0, prefix.size(), prefix) != 0) return thumb;
  std::string enc = thumb.substr(prefix.size());
  // the URL is percent-encoded: "%3Dw120-h120" is "=w120-h120"
  size_t at = enc.find("%3Dw");
  if (at == std::string::npos) return thumb;
  size_t h = enc.find("-h", at);
  if (h == std::string::npos) return thumb;
  size_t end = h + 2;
  while (end < enc.size() && isdigit((unsigned char)enc[end])) end++;
  std::string s = std::to_string(size);
  return prefix + enc.substr(0, at) + "%3Dw" + s + "-h" + s + enc.substr(end);
}

std::string artOf(const std::string& path, int size) {
  if (!isStream(path)) {
    if (const Book* b = g_library.bookOf(path)) return b->cover;  // an audiobook's file: the book's picture
    return path;
  }
  const Item* it = streamItem(path);
  return it ? (size > 0 ? largeThumb(it->thumb, size) : it->thumb) : "";
}

void rememberStream(const std::string& path, const Item& item) { registry()[path] = item; }

void playStreamsAppend(const Item& item, bool next) {
  std::string p = streamPath(item);
  registry()[p] = item;
  if (booksTakeQueueAdd(p, next)) return g_app->toast(tr("book.queuedForAfter"));  // the music waiting for the book
  if (next) g_player.insertNext(p);
  else g_player.append(p);
}

}  // namespace rondo::shell

namespace rondo::shell {

// 1.0.x's connectivity receiver and recover() (PlayerService.kt), for the songs the server streams
// (the card doesn't care about Wi-Fi): offline pauses what plays, or what failed for lack of a
// connection; online again resumes it. A stream that fails while online is tried again where it
// stopped, three times for the same place (1.0.x: three recoveries a minute, each a few seconds; here
// each try is the stream's own 20 s of retries).
void streamsNetwork(bool online) {
  static bool wasOnline = online, pausedForNetwork = false;
  static int64_t failKey = -1, triedAt = 0;
  static int tries = 0;
  static bool waiting = false;  // a try was asked for: the failure must clear before another
  PlayerState s = g_player.state();
  bool streaming = s.index >= 0 && strncmp(s.path, "/s/", 3) == 0;
  int64_t now = now_ns();
  if (online != wasOnline) {
    wasOnline = online;
    if (streaming && !online && (s.playing || s.failed)) {
      fprintf(stderr, "rondo: connectivity: offline, paused\n");
      pausedForNetwork = true;
      if (s.playing) g_player.pause();
    } else if (streaming && online && (pausedForNetwork || s.failed)) {
      fprintf(stderr, "rondo: connectivity: online, playing again\n");
      pausedForNetwork = false;
      g_player.play();
    }
    return;
  }
  if (!streaming) pausedForNetwork = false;
  if (!s.failed) waiting = false;
  // recover(): online, a stream that failed
  if (!streaming || !online || !s.failed || (waiting && now - triedAt < 5 * kSec)) return;
  int64_t key = int64_t(s.index) * 100000000LL + s.positionMs / 1000;  // the song and the second it stopped at
  if (key != failKey) failKey = key, tries = 0;
  if (tries == 3) {
    fprintf(stderr, "rondo: giving up on the stream after 3 tries\n");
    tries++;
  }
  if (tries > 3) return;
  tries++;
  waiting = true, triedAt = now;
  fprintf(stderr, "rondo: the stream failed: again where it stopped (try %d)\n", tries);
  g_player.play();
}

// ---- resolving ahead (screens.h) ----
namespace {
constexpr int64_t kRest = 500 * 1000000LL;      // the wheel still for half a second: the row is wanted
constexpr int64_t kAgain = 20 * 60 * kSec;       // a video asked for is not asked again for 20 minutes
std::map<std::string, int64_t> g_asked;           // id -> when
std::string g_resting;
int64_t g_restSince = 0;
int g_lastIndex = -2, g_lastNext = -2;
size_t g_lastCount = 0;

std::string idOfStream(const std::string& path) {  // "/s/<id>/140" -> id
  size_t a = 3, b = path.find('/', a);
  return isStream(path) && b != std::string::npos ? path.substr(a, b - a) : "";
}

const bool kLog = getenv("RONDO_AHEAD_LOG");

void ask(const std::string& id, int64_t now) {
  if (id.size() != 11) return;
  auto it = g_asked.find(id);
  if (kLog) fprintf(stderr, "ahead: %s%s\n", id.c_str(), it != g_asked.end() && now - it->second < kAgain ? " (asked already)" : "");
  if (it != g_asked.end() && now - it->second < kAgain) return;
  if (g_asked.size() > 300) g_asked.clear();
  g_asked[id] = now;
  // the answer doesn't matter: an older server resolves before it answers, a newer one at once
  g_core.get("/resolve/" + Api::escape(id) + "?ahead=1", 0, [](int, const std::string&) {});
}
}  // namespace

void restingOn(const std::string& id) {
  int64_t now = now_ns();
  // the row it leaves, if it rested there long enough and the main loop hadn't looked yet
  if (!g_resting.empty() && g_resting != id && now - g_restSince >= kRest) ask(g_resting, now);
  if (id == g_resting) return;
  g_resting = id;
  g_restSince = now;
}

int64_t aheadWakeAt() { return g_resting.empty() ? 0 : g_restSince + kRest; }

void aheadTick(int64_t now) {
  // the song after the one playing, once one starts (and when the queue or its order changes)
  PlayerState s = g_player.state();
  const std::vector<std::string>& queue = g_player.queue();
  if (s.index != g_lastIndex || s.nextIndex != g_lastNext || queue.size() != g_lastCount) {
    g_lastIndex = s.index, g_lastNext = s.nextIndex, g_lastCount = queue.size();
    if (s.index >= 0 && s.nextIndex >= 0 && s.nextIndex < int(queue.size())) ask(idOfStream(queue[size_t(s.nextIndex)]), now);
  }
  // the row the wheel rests on
  if (!g_resting.empty() && now - g_restSince >= kRest) {
    ask(g_resting, now);
    g_resting.clear();
  }
}

}  // namespace rondo::shell
