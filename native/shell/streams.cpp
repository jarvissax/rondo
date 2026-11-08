// Songs the server streams, as the player's queue holds them: "/s/<id>/140" paths (1.0.x's
// audioMediaItem: AAC from YouTube Music through the server's range proxy), with what the screens show
// about each (title, subtitle, artwork), kept here since the library knows nothing of them.
#include <stdio.h>
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
  if (!isStream(path)) return path;
  const Item* it = streamItem(path);
  return it ? (size > 0 ? largeThumb(it->thumb, size) : it->thumb) : "";
}

void playStreamsAppend(const Item& item, bool next) {
  std::string p = streamPath(item);
  registry()[p] = item;
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

}  // namespace rondo::shell
