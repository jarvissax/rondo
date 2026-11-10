// The queue across a restart (2.0's crash budget, private/native-plan.md N6: "a crash means back in
// <= 1 s with playback resumed"; 1.0.x started empty): what plays and where is kept beside the prefs,
// and put back when Rondo starts. Songs from the card and streamed songs (with what the screens show
// about them); a video or the radio is not kept.
//
//   queue.txt        the queue, a line each: the path, then for a streamed song its kind, id, title,
//                    subtitle, thumbnail and length, tab-separated (written when the queue changes)
//   queue-state.txt  where it was: the index, the position, playing, shuffle, repeat, when (every 5 s
//                    while it changes)
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "app.h"
#include "screens.h"

namespace rondo::shell {
namespace {

std::string dir() { return g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1); }

std::string clean(std::string s) {
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> f;
  size_t at = 0;
  for (;;) {
    size_t t = line.find('\t', at);
    f.push_back(line.substr(at, t == std::string::npos ? std::string::npos : t - at));
    if (t == std::string::npos) break;
    at = t + 1;
  }
  return f;
}

bool writeFile(const std::string& path, const std::string& text) {
  std::string tmp = path + ".part";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0;
  fclose(f);
  return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

std::vector<std::string> g_savedQueue;  // what queue.txt holds
bool g_resumeWaiting = false;          // the queue is on a card not mounted yet
std::string g_savedState;
int64_t g_stateAt = 0;

}  // namespace

void resumeTick(int64_t now) {
  if (now - g_stateAt < 5 * kSec) return;
  PlayerState s = g_player.state();
  if (s.video || s.fmOn || !g_player.active() || s.index < 0) return;
  const std::vector<std::string>& q = g_player.queue();
  if (q != g_savedQueue) {
    std::string text;
    for (const std::string& p : q) {
      text += clean(p);
      if (const Item* it = isStream(p) ? streamItem(p) : nullptr)
        text += "\t" + clean(it->kind) + "\t" + clean(it->id) + "\t" + clean(it->title) + "\t" + clean(it->subtitle) + "\t" +
                clean(it->thumb) + "\t" + std::to_string(it->durationSec);
      text += "\n";
    }
    if (writeFile(dir() + "queue.txt", text)) g_savedQueue = q;
  }
  char state[160];
  snprintf(state, sizeof(state), "%d %lld %d %d %d %lld\n", s.index, (long long)(PlayerHost::positionAt(s, now) / 1000 * 1000),
           s.playing ? 1 : 0, s.shuffle ? 1 : 0, s.repeat, (long long)time(nullptr));
  // unchanged but for the time: not written again (the position moves only while playing)
  std::string st = state, key = st.substr(0, st.rfind(' '));
  if (key != g_savedState.substr(0, g_savedState.rfind(' ')) && writeFile(dir() + "queue-state.txt", st)) g_savedState = st;
  g_stateAt = now;
}

void resumeSaveNow() {
  g_stateAt = 0;
  g_savedState.clear();
  resumeTick(now_ns());
}

void resumeRestore() {
  FILE* f = fopen((dir() + "queue-state.txt").c_str(), "r");
  if (!f) return;
  int index = -1, playing = 0, shuffle = 0, repeat = 0;
  long long ms = 0, at = 0;
  bool ok = fscanf(f, "%d %lld %d %d %d %lld", &index, &ms, &playing, &shuffle, &repeat, &at) == 6;
  fclose(f);
  if (!ok || index < 0) return;
  std::vector<std::string> paths;
  if (FILE* q = fopen((dir() + "queue.txt").c_str(), "r")) {
    char buf[4096];
    while (fgets(buf, sizeof(buf), q)) {
      std::string line = buf;
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      if (line.empty()) continue;
      std::vector<std::string> f2 = split(line);
      if (f2.size() >= 7) {
        Item it;
        it.kind = f2[1], it.id = f2[2], it.title = f2[3], it.subtitle = f2[4], it.thumb = f2[5];
        it.durationSec = atoi(f2[6].c_str());
        rememberStream(f2[0], it);
      }
      paths.push_back(f2[0]);
    }
    fclose(q);
  }
  if (paths.empty() || index >= int(paths.size())) return;
  // a card song still there; a streamed one is asked for again. The card's mount at boot runs in the
  // background (firmware/rondo/start), so after a restart it may not be there yet: then this is asked
  // again once it is (main.cpp; Nov 10: the queue was lost at every restart).
  const std::string& cur = paths[size_t(index)];
  if (!isStream(cur) && access(cur.c_str(), R_OK) != 0) {
    int root = cur.compare(0, 17, "/storage/sdcard0/") == 0 ? 1 : cur.compare(0, 17, "/storage/sdcard1/") == 0 ? 2 : 0;
    g_resumeWaiting = root && !(g_library.presentRoots() & root);
    return;
  }
  g_resumeWaiting = false;
  g_savedQueue = paths;
  // playing again only after a crash or a quick restart (seconds ago); after a night off, paused
  bool resume = playing && time(nullptr) - at < 20;
  g_player.setShuffle(shuffle != 0);
  g_player.setRepeat(repeat);
  g_player.restoreQueue(paths, index, ms, resume);
  printf("rondo: the queue is back: %zu songs, at %d, %lld ms%s\n", paths.size(), index, ms, resume ? ", playing" : "");
}

}  // namespace rondo::shell

namespace rondo::shell {

bool resumeWaitingForCard() { return g_resumeWaiting; }

}  // namespace rondo::shell
