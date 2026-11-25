#include "video_places.h"

#include <stdio.h>
#include <yyjson.h>

#include <algorithm>
#include <map>

#include "../core/lang.h"
#include "../core/library.h"
#include "../core/video_place.h"
#include "app.h"
#include "books.h"
#include "downloads.h"

namespace rondo::shell {
namespace {

// What the screens show of a YouTube video the Y1 keeps a place for (its title, channel, picture and
// length), for YouTube's Continue watching: watching.json beside the prefs, the 200 last watched
struct Seen {
  std::string title, subtitle, thumb;
  int durationSec = -1;
  int64_t at = 0;  // ms since 1970
};
std::map<std::string, Seen> g_seen;  // by YouTube id
bool g_read = false;

std::string storePath() { return g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1) + "watching.json"; }

std::string jq(const std::string& v) {
  std::string o = "\"";
  for (unsigned char c : v) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c == '\n') o += "\\n";
    else if (c < 0x20) o += ' ';
    else o += char(c);
  }
  return o + "\"";
}

void readStore() {
  if (g_read) return;
  g_read = true;
  yyjson_doc* d = yyjson_read_file(storePath().c_str(), 0, nullptr, nullptr);
  if (!d) return;
  yyjson_val* root = yyjson_doc_get_root(d);
  size_t i, n;
  yyjson_val *k, *v;
  yyjson_obj_foreach(root, i, n, k, v) {
    Seen s;
    auto str = [&](const char* f) {
      yyjson_val* x = yyjson_obj_get(v, f);
      return yyjson_is_str(x) ? std::string(yyjson_get_str(x)) : std::string();
    };
    s.title = str("title"), s.subtitle = str("subtitle"), s.thumb = str("thumb");
    yyjson_val* dur = yyjson_obj_get(v, "durationSec");
    yyjson_val* at = yyjson_obj_get(v, "at");
    s.durationSec = yyjson_is_num(dur) ? int(yyjson_get_num(dur)) : -1;
    s.at = yyjson_is_num(at) ? int64_t(yyjson_get_num(at)) : 0;
    g_seen[yyjson_get_str(k)] = s;
  }
  yyjson_doc_free(d);
}

void writeStore() {
  while (g_seen.size() > 200) {
    auto oldest = g_seen.begin();
    for (auto it = g_seen.begin(); it != g_seen.end(); ++it)
      if (it->second.at < oldest->second.at) oldest = it;
    g_seen.erase(oldest);
  }
  std::string j = "{";
  int n = 0;
  for (auto& [id, s] : g_seen)
    j += std::string(n++ ? "," : "") + jq(id) + ":{\"title\":" + jq(s.title) + ",\"subtitle\":" + jq(s.subtitle) + ",\"thumb\":" +
         jq(s.thumb) + ",\"durationSec\":" + std::to_string(s.durationSec) + ",\"at\":" + std::to_string(s.at) + "}";
  j += "}\n";
  std::string tmp = storePath() + ".part";
  if (FILE* f = fopen(tmp.c_str(), "wb")) {
    bool ok = fwrite(j.data(), 1, j.size(), f) == j.size() && fflush(f) == 0;
    fclose(f);
    if (ok) rename(tmp.c_str(), storePath().c_str());
  }
}

bool isYouTubeKey(const std::string& key) { return key.compare(0, 4, "ytv:") == 0; }

const BookState* placeOf(const std::string& key) { return key.empty() ? nullptr : g_library.bookState(key); }

// the places of started videos, the last watched first
std::vector<BookState> started() {
  std::vector<BookState> out;
  for (BookState& s : g_library.bookStates())
    if (!s.finished && s.positionMs > 0 && (isYouTubeKey(s.key) || (!s.key.empty() && s.key[0] == '/'))) out.push_back(s);
  std::sort(out.begin(), out.end(), [](const BookState& a, const BookState& b) { return a.lastPlayed > b.lastPlayed; });
  return out;
}

// a card video as the player takes it (library_screens.cpp's cardVideo)
Item cardItem(const Track& t) {
  Item i;
  i.kind = "video";
  i.id = t.path, i.title = t.title, i.thumb = t.path;
  i.durationSec = t.durationSec;
  return i;
}

}  // namespace

std::string videoPlaceKey(const Item& item) { return videoKeyFor(item.id); }

int64_t videoOpensAt(const std::string& key, int64_t lengthMs, int watchedPct) {
  const BookState* st = placeOf(key);
  bool has = st && (st->positionMs > 0 || st->finished);
  int64_t paused = st && st->lastPlayed ? wallNowMs() - st->lastPlayed : 0;
  return videoStartMs(has, st ? st->positionMs : 0, st && st->finished, paused, lengthMs, watchedPct);
}

void videoPlaceSave(const std::string& key, const Item& item, int64_t ms, int64_t lengthMs, bool ended) {
  if (key.empty() || !videoKeepsPlace(lengthMs)) return;
  BookState st;
  if (const BookState* old = placeOf(key)) st = *old;
  st.key = key;
  st.file = item.id;
  st.positionMs = std::max<int64_t>(0, ms);
  st.finished = ended || finishedAt(ms, lengthMs);
  st.lastPlayed = wallNowMs();
  g_library.setBookState(st);
  if (!isYouTubeKey(key) || item.id.empty() || item.id[0] == '/') return;  // a card file: the library knows it
  readStore();
  Seen& s = g_seen[item.id];
  bool news = s.title != item.title || s.thumb != item.thumb || s.subtitle != item.subtitle || s.at == 0;
  s.title = item.title, s.subtitle = item.subtitle, s.thumb = item.thumb;
  s.durationSec = int(lengthMs / 1000);
  // the time it was last watched decides which 200 stay: written when it's new, else at most each hour
  bool stale = st.lastPlayed - s.at > 3600000;
  s.at = st.lastPlayed;
  if (news || stale) writeStore();
}

void videoStartAgain(const std::string& key) {
  const BookState* old = placeOf(key);
  if (!old) return;
  BookState st = *old;
  st.positionMs = 0;
  st.finished = false;
  g_library.setBookState(st);
}

bool videoHasPlace(const std::string& key) {
  const BookState* st = placeOf(key);
  return st && !st->finished && st->positionMs > 0;
}

float videoProgress(const std::string& key, int lengthSec, int watchedPct) {
  if (const BookState* st = placeOf(key)) {
    if (st->finished) return 1;
    if (st->positionMs > 0 && lengthSec > 0) return std::min(1.f, float(st->positionMs) / (lengthSec * 1000.f));
  }
  return watchedPct > 0 ? std::min(1.f, watchedPct / 100.f) : -1;
}

std::string videoLeftText(const std::string& key, int lengthSec) {
  const BookState* st = placeOf(key);
  if (!st || st->finished || st->positionMs <= 0 || lengthSec <= 0) return "";
  return tr("book.left", {{"time", listenTime(std::max<int64_t>(0, int64_t(lengthSec) * 1000 - st->positionMs), 100)}});
}

std::vector<Item> continueWatchingCard() {
  std::vector<Item> out;
  for (const BookState& s : started()) {
    std::string path = isYouTubeKey(s.key) ? downloadedFile(s.key.substr(4)) : s.key;
    const Track* t = path.empty() ? nullptr : g_library.find(path);
    if (t && t->video && !t->gone) out.push_back(cardItem(*t));
  }
  return out;
}

std::vector<Item> continueWatchingYouTube() {
  readStore();
  std::vector<Item> out;
  for (const BookState& s : started()) {
    if (!isYouTubeKey(s.key)) continue;
    std::string id = s.key.substr(4), file = downloadedFile(id);
    const Track* t = file.empty() ? nullptr : g_library.find(file);
    auto seen = g_seen.find(id);
    if (t && t->video && !t->gone) {  // on the card: its file, so it plays with no connection
      Item i = cardItem(*t);
      if (seen != g_seen.end()) i.title = seen->second.title, i.subtitle = seen->second.subtitle;
      out.push_back(i);
    } else if (seen != g_seen.end()) {
      Item i;
      i.kind = "video";
      i.id = id, i.title = seen->second.title, i.subtitle = seen->second.subtitle, i.thumb = seen->second.thumb;
      i.durationSec = seen->second.durationSec;
      out.push_back(i);
    }
  }
  return out;
}

}  // namespace rondo::shell
