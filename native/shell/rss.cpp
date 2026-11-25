// rss.h: the RSS shows subscribed to, their new episodes, episodes as spoken word, downloads.
#include "rss.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

#include <algorithm>
#include <map>
#include <set>

#include "../core/organize.h"
#include "../core/rss_ids.h"
#include "app.h"
#include "books.h"
#include "downloads.h"
#include "file_ops.h"
#include "podcasts.h"
#include "screens.h"

namespace rondo::shell {

const char* const kRssCountries[] = {
    "ae", "ag", "ai", "al", "am", "ao", "ar", "at", "au", "az", "ba", "bb", "be", "bf", "bg", "bh", "bj", "bm", "bn", "bo", "br", "bs",
    "bt", "bw", "by", "bz", "ca", "cd", "cg", "ch", "ci", "cl", "cm", "cn", "co", "cr", "cv", "cy", "cz", "de", "dk", "dm", "do", "dz",
    "ec", "ee", "eg", "es", "fi", "fj", "fm", "fr", "ga", "gb", "gd", "ge", "gh", "gm", "gr", "gt", "gw", "gy", "hk", "hn", "hr", "hu",
    "id", "ie", "il", "in", "iq", "is", "it", "jm", "jo", "jp", "ke", "kg", "kh", "kn", "kr", "kw", "ky", "kz", "la", "lb", "lc", "lk",
    "lr", "lt", "lu", "lv", "ly", "ma", "md", "me", "mg", "mk", "ml", "mm", "mn", "mo", "mr", "ms", "mt", "mu", "mv", "mw", "mx", "my",
    "mz", "na", "ne", "ng", "ni", "nl", "no", "np", "nr", "nz", "om", "pa", "pe", "pg", "ph", "pk", "pl", "pt", "pw", "py", "qa", "ro",
    "rs", "ru", "rw", "sa", "sb", "sc", "se", "sg", "si", "sk", "sl", "sn", "sr", "st", "sv", "sz", "tc", "td", "th", "tj", "tm", "tn",
    "to", "tr", "tt", "tw", "tz", "ua", "ug", "us", "uy", "uz", "vc", "ve", "vg", "vn", "vu", "xk", "ye", "za", "zm", "zw"};
const int kRssCountryCount = int(sizeof(kRssCountries) / sizeof(kRssCountries[0]));

// Apple's podcast genres (its top-level ones)
const RssGenre kRssGenres[] = {{0, "rss.genre.all"},         {1301, "rss.genre.arts"},      {1321, "rss.genre.business"},
                               {1303, "rss.genre.comedy"},    {1304, "rss.genre.education"}, {1483, "rss.genre.fiction"},
                               {1511, "rss.genre.government"}, {1512, "rss.genre.health"},   {1487, "rss.genre.history"},
                               {1305, "rss.genre.kids"},      {1502, "rss.genre.leisure"},   {1310, "rss.genre.music"},
                               {1489, "rss.genre.news"},      {1314, "rss.genre.religion"},  {1533, "rss.genre.science"},
                               {1324, "rss.genre.society"},   {1545, "rss.genre.sports"},    {1318, "rss.genre.technology"},
                               {1488, "rss.genre.trueCrime"}, {1309, "rss.genre.tv"}};
const int kRssGenreCount = int(sizeof(kRssGenres) / sizeof(kRssGenres[0]));

namespace {

constexpr const char* kFilePrefix = "http://127.0.0.1:47811/rss/file?";
constexpr int64_t kNewForMs = 30LL * 24 * 3600 * 1000;  // New episodes: a month at most
constexpr int64_t kCheckEveryMs = 4LL * 3600 * 1000;    // every 4 hours while the Y1 is awake
constexpr int64_t kOpenCheckMs = 15LL * 60 * 1000;      // Podcasts opening: at most every 15 min
constexpr size_t kLatest = 30;                          // a show's newest episodes kept with it

std::string dataDir() { return g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1); }
std::string storePath() { return dataDir() + "rss.json"; }

struct ShowRec {
  RssShow s;
  bool subscribed = false;
  int64_t subscribedAt = 0;  // ms since 1970
  int64_t newSince = 0;      // unix s: episodes after this are new
  int64_t checkedAt = 0;     // ms since 1970
  RssAuto autoDl;
  std::vector<RssEpisode> latest;  // its newest, newest first
};
struct EpRec {
  RssEpisode e;
  std::string file;  // the card's copy, "" none
  std::string chapters;  // encodeChapters(): what was found (its chapter file's, its MP3's); "" none
  bool chaptersAsked = false;
  bool autoDl = false;  // downloaded by "Download new episodes"
  bool noAuto = false;  // its download deleted by hand: not downloaded again by itself
  int64_t downloadedAt = 0, used = 0, laterAt = 0;  // ms since 1970
};
std::map<std::string, ShowRec> g_shows;
std::map<std::string, EpRec> g_eps;
std::vector<std::string> g_upNext;
std::string g_country = "us";
bool g_read = false, g_dirty = false;
int64_t g_writtenAt = 0;
uint32_t g_serial = 1;
bool g_afterSong = false;  // Play next / Add to queue while music plays: after this song
uint32_t g_afterSerial = 0;
std::set<std::string> g_deleteWhenDone;
std::map<std::string, RssShow> g_seen;  // shows looked at this time (a page, a search), not kept  // played, its show deletes played ones: once it isn't playing
// the check for new episodes
bool g_checking = false;
std::vector<std::string> g_toCheck;
std::string g_checkingNow;
int64_t g_lastCheck = 0, g_retryAt = 0;  // ms since 1970; now_ns()
int g_retries = 0;

void changed() {
  g_dirty = true;
  g_serial++;
}

std::string sval(yyjson_val* o, const char* k) {
  yyjson_val* v = yyjson_obj_get(o, k);
  return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string();
}
int64_t ival(yyjson_val* o, const char* k, int64_t otherwise = 0) {
  yyjson_val* v = yyjson_obj_get(o, k);
  return yyjson_is_num(v) ? int64_t(yyjson_get_num(v)) : otherwise;
}
bool bval(yyjson_val* o, const char* k) { return yyjson_get_bool(yyjson_obj_get(o, k)); }
std::string jq(const std::string& v) {
  std::string o = "\"";
  for (unsigned char c : v) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += char(c);
    }
  }
  return o + "\"";
}
bool fileThere(const std::string& p) {
  struct stat st;
  return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool online() { return g_core.state().wifiState == 4; }

std::string episodeJsonKept(const RssEpisode& e) {
  std::string o = "{\"id\":" + jq(e.id) + ",\"show\":" + jq(e.show) + ",\"showTitle\":" + jq(e.showTitle) + ",\"guid\":" + jq(e.guid) +
                  ",\"title\":" + jq(e.title) + ",\"url\":" + jq(e.url) + ",\"type\":" + jq(e.type) + ",\"date\":" + std::to_string(e.date) +
                  ",\"duration\":" + std::to_string(e.duration) + ",\"size\":" + std::to_string(e.size);
  if (!e.image.empty()) o += ",\"image\":" + jq(e.image);
  if (!e.chapters.empty()) o += ",\"chapters\":" + jq(e.chapters);
  if (e.season) o += ",\"season\":" + std::to_string(e.season);
  if (e.number) o += ",\"episode\":" + std::to_string(e.number);
  if (!e.episodeType.empty()) o += ",\"episodeType\":" + jq(e.episodeType);
  return o;  // the caller ends it
}

RssEpisode episodeFrom(yyjson_val* v) {
  RssEpisode e;
  e.id = sval(v, "id"), e.show = sval(v, "show"), e.showTitle = sval(v, "showTitle"), e.guid = sval(v, "guid"), e.title = sval(v, "title");
  e.url = sval(v, "url"), e.type = sval(v, "type"), e.image = sval(v, "image"), e.chapters = sval(v, "chapters");
  e.date = ival(v, "date"), e.duration = int(ival(v, "duration")), e.size = ival(v, "size");
  e.season = int(ival(v, "season")), e.number = int(ival(v, "episode"));
  e.episodeType = sval(v, "episodeType");
  if (e.episodeType == "full") e.episodeType.clear();
  return e;
}

std::string showJsonKept(const RssShow& s) {
  return "\"feed\":" + jq(s.feed) + ",\"title\":" + jq(s.title) + ",\"author\":" + jq(s.author) + ",\"art\":" + jq(s.art) +
         ",\"description\":" + jq(s.description) + ",\"link\":" + jq(s.link) + ",\"genre\":" + jq(s.genre) + ",\"serial\":" +
         (s.serial ? "true" : "false") + ",\"private\":" + (s.priv ? "true" : "false") + ",\"count\":" + std::to_string(s.count) +
         ",\"updated\":" + std::to_string(s.updated) + ",\"apple\":" + std::to_string(s.apple);
}
RssShow showFrom(yyjson_val* v) {
  RssShow s;
  s.id = sval(v, "id"), s.feed = sval(v, "feed"), s.title = sval(v, "title"), s.author = sval(v, "author"), s.art = sval(v, "art");
  s.description = sval(v, "description"), s.link = sval(v, "link"), s.genre = sval(v, "genre");
  s.serial = bval(v, "serial"), s.priv = bval(v, "private");
  s.count = ival(v, "count"), s.updated = ival(v, "updated"), s.apple = ival(v, "apple");
  return s;
}

void readStore() {
  if (g_read) return;
  g_read = true;
  yyjson_doc* d = yyjson_read_file(storePath().c_str(), 0, nullptr, nullptr);
  if (!d) return;
  yyjson_val* r = yyjson_doc_get_root(d);
  std::string cc = sval(r, "country");
  if (cc.size() == 2) g_country = cc;
  g_lastCheck = ival(r, "lastCheck");
  size_t i, n;
  yyjson_val *k, *v;
  yyjson_obj_foreach(yyjson_obj_get(r, "shows"), i, n, k, v) {
    ShowRec rec;
    rec.s = showFrom(v);
    rec.s.id = yyjson_get_str(k);
    rec.subscribed = bval(v, "subscribed");
    rec.subscribedAt = ival(v, "subscribedAt"), rec.newSince = ival(v, "newSince"), rec.checkedAt = ival(v, "checkedAt");
    rec.autoDl.on = bval(v, "auto"), rec.autoDl.keep = int(ival(v, "keep", 3)), rec.autoDl.deletePlayed = bval(v, "deletePlayed");
    size_t j, m;
    yyjson_val* e;
    yyjson_arr_foreach(yyjson_obj_get(v, "latest"), j, m, e) {
      RssEpisode ep = episodeFrom(e);
      ep.show = rec.s.id, ep.showTitle = rec.s.title;
      if (!ep.id.empty()) rec.latest.push_back(ep);
    }
    if (!rec.s.id.empty()) g_shows[rec.s.id] = std::move(rec);
  }
  yyjson_obj_foreach(yyjson_obj_get(r, "episodes"), i, n, k, v) {
    EpRec p;
    p.e = episodeFrom(v);
    p.e.id = yyjson_get_str(k);
    p.file = sval(v, "file"), p.chapters = sval(v, "chapterList");
    p.chaptersAsked = bval(v, "chaptersAsked");
    p.autoDl = bval(v, "auto"), p.noAuto = bval(v, "noAuto");
    p.downloadedAt = ival(v, "downloadedAt"), p.used = ival(v, "used"), p.laterAt = ival(v, "laterAt");
    if (!p.e.id.empty()) g_eps[p.e.id] = std::move(p);
  }
  yyjson_arr_foreach(yyjson_obj_get(r, "upNext"), i, n, v) if (yyjson_is_str(v)) g_upNext.push_back(yyjson_get_str(v));
  yyjson_doc_free(d);
}

bool inProgress(const std::string& id) {
  const BookState* st = g_library.bookState(rssKey(id));
  return st && st->lastPlayed > 0 && !st->finished;
}

// what's kept: 1,000 episodes at most (the least used go first, never one on the card, queued, saved
// for later or started); shows not subscribed to only while an episode of theirs is kept
void trim() {
  if (g_eps.size() > 1000) {
    std::vector<std::pair<int64_t, std::string>> by;
    for (auto& [id, p] : g_eps)
      if (p.file.empty() && !p.laterAt && !inProgress(id) && std::find(g_upNext.begin(), g_upNext.end(), id) == g_upNext.end())
        by.push_back({p.used, id});
    std::sort(by.begin(), by.end());
    for (size_t k = 0; k < by.size() && g_eps.size() > 1000; k++) g_eps.erase(by[k].second);
  }
  std::set<std::string> used;
  for (auto& [id, p] : g_eps) used.insert(p.e.show);
  for (auto it = g_shows.begin(); it != g_shows.end();) it = it->second.subscribed || used.count(it->first) ? std::next(it) : g_shows.erase(it);
}

void writeStore() {
  trim();
  std::string j = "{\"country\":" + jq(g_country) + ",\"lastCheck\":" + std::to_string(g_lastCheck) + ",\"shows\":{";
  int n = 0;
  for (auto& [id, rec] : g_shows) {
    j += std::string(n++ ? "," : "") + jq(id) + ":{" + showJsonKept(rec.s) + ",\"subscribed\":" + (rec.subscribed ? "true" : "false") +
         ",\"subscribedAt\":" + std::to_string(rec.subscribedAt) + ",\"newSince\":" + std::to_string(rec.newSince) +
         ",\"checkedAt\":" + std::to_string(rec.checkedAt) + ",\"auto\":" + (rec.autoDl.on ? "true" : "false") +
         ",\"keep\":" + std::to_string(rec.autoDl.keep) + ",\"deletePlayed\":" + (rec.autoDl.deletePlayed ? "true" : "false") + ",\"latest\":[";
    for (size_t k = 0; k < rec.latest.size(); k++) j += (k ? "," : "") + episodeJsonKept(rec.latest[k]) + "}";
    j += "]}";
  }
  j += "},\"episodes\":{";
  n = 0;
  for (auto& [id, p] : g_eps) {
    std::string e = episodeJsonKept(p.e);
    j += std::string(n++ ? "," : "") + jq(id) + ":" + e + ",\"file\":" + jq(p.file) + ",\"chapterList\":" + jq(p.chapters) +
         ",\"chaptersAsked\":" + (p.chaptersAsked ? "true" : "false") + ",\"auto\":" + (p.autoDl ? "true" : "false") +
         ",\"noAuto\":" + (p.noAuto ? "true" : "false") + ",\"downloadedAt\":" + std::to_string(p.downloadedAt) +
         ",\"used\":" + std::to_string(p.used) + ",\"laterAt\":" + std::to_string(p.laterAt) + "}";
  }
  j += "},\"upNext\":[";
  for (size_t k = 0; k < g_upNext.size(); k++) j += (k ? "," : "") + jq(g_upNext[k]);
  j += "]}\n";
  std::string tmp = storePath() + ".part";
  if (FILE* f = fopen(tmp.c_str(), "wb")) {
    bool ok = fwrite(j.data(), 1, j.size(), f) == j.size() && fflush(f) == 0;
    fclose(f);
    if (ok) rename(tmp.c_str(), storePath().c_str());
  }
  g_dirty = false;
  g_writtenAt = now_ns();
}

EpRec* kept(const std::string& id) {
  readStore();
  auto it = g_eps.find(id);
  return it == g_eps.end() ? nullptr : &it->second;
}
ShowRec* showRec(const std::string& id) {
  readStore();
  auto it = g_shows.find(id);
  return it == g_shows.end() ? nullptr : &it->second;
}

// a show's picture: Apple's (which comes at any size) when it was found there, else the feed's own
std::string showArt(const std::string& showId) {
  const ShowRec* r = showRec(showId);
  return r ? r->s.art : "";
}
std::string episodeArt(const RssEpisode& e) {
  std::string a = showArt(e.show);
  return rssArt(a.empty() ? e.image : a);
}

Item streamItemOf(const RssEpisode& e) {
  Item it;
  it.kind = "rss", it.id = e.id, it.title = e.title, it.subtitle = e.showTitle, it.thumb = episodeArt(e);
  it.durationSec = e.duration > 0 ? e.duration : -1;
  return it;
}

std::string showTitleOf(const std::string& showId) {
  const ShowRec* r = showRec(showId);
  return r ? r->s.title : "";
}

// the newest first; a show's latest list kept to kLatest
void mergeLatest(ShowRec& rec, const std::vector<RssEpisode>& eps) {
  std::map<std::string, RssEpisode> by;
  for (const RssEpisode& e : rec.latest) by[e.id] = e;
  for (const RssEpisode& e : eps) by[e.id] = e;
  std::vector<RssEpisode> all;
  for (auto& [id, e] : by) {
    all.push_back(e);
    all.back().show = rec.s.id, all.back().showTitle = rec.s.title;
  }
  std::stable_sort(all.begin(), all.end(), [](const RssEpisode& a, const RssEpisode& b) { return a.date > b.date; });
  if (all.size() > kLatest) all.resize(kLatest);
  rec.latest = std::move(all);
  if (!rec.latest.empty()) rec.s.updated = std::max(rec.s.updated, rec.latest.front().date);
}

// Up next: the next one starts (true)
bool startNext() {
  readStore();
  while (!g_upNext.empty()) {
    std::string id = g_upNext.front();
    g_upNext.erase(g_upNext.begin());
    changed();
    if (EpRec* p = kept(id)) {
      RssEpisode e = p->e;
      playRss(e);
      return true;
    }
  }
  return false;
}

// a show's "Download new episodes": its latest few not finished on the card (or on their way); what it
// downloaded before that isn't among them now, deleted (not one playing)
void autoDownload(const std::string& showId) {
  ShowRec* rec = showRec(showId);
  if (!rec || !rec->subscribed || !rec->autoDl.on) return;
  std::vector<RssEpisode> eps = rec->latest;
  std::set<std::string> keep;
  for (const RssEpisode& e : eps) {
    if (int(keep.size()) >= rec->autoDl.keep) break;
    const BookState* st = g_library.bookState(rssKey(e.id));
    if (st && st->finished) continue;
    keep.insert(e.id);
    EpRec* p = kept(e.id);
    if (p && (p->noAuto || fileThere(p->file))) continue;
    bool queued = false;
    for (const DownloadJob& j : downloadJobs())
      if (j.rss && j.id == e.id && j.state != DownloadJob::Failed && j.state != DownloadJob::Done) queued = true;
    if (!queued) downloadRss(e, true, true);
  }
  const BookPlay* now = bookNow();
  std::vector<std::string> drop;
  for (auto& [id, p] : g_eps)
    if (p.e.show == showId && p.autoDl && fileThere(p.file) && !keep.count(id) && !(now && now->key == rssKey(id))) drop.push_back(id);
  for (const std::string& id : drop) deleteRssDownload(id);
}

void checkNext();

void checked(const std::string& id, int status, const std::string& body) {
  if (status == 202 && g_retries < 30) {  // still being read: asked again in a moment
    g_retries++;
    g_retryAt = now_ns() + 3 * kSec;
    return;
  }
  g_retries = 0;
  if (status == 200) {
    RssShow s;
    std::vector<RssEpisode> eps;
    if (parseRssFeed(body, s, eps)) {
      if (ShowRec* rec = showRec(id)) {
        rec->checkedAt = wallNowMs();
        rssNoteFeed(s, eps);
        autoDownload(id);
      }
    }
  }
  checkNext();
}

void checkNext() {
  g_checkingNow.clear();
  while (!g_toCheck.empty()) {
    std::string id = g_toCheck.front();
    g_toCheck.erase(g_toCheck.begin());
    ShowRec* rec = showRec(id);
    if (!rec || !rec->subscribed) continue;
    int64_t newest = 0;
    for (const RssEpisode& e : rec->latest) newest = std::max(newest, e.date);
    g_checkingNow = id;
    std::string path = rssFeedPath(rec->s, true, newest > 0 ? newest - 1 : 0);
    g_core.get(path, 0, [id](int status, const std::string& body) { checked(id, status, body); });
    return;
  }
  g_checking = false;
  g_lastCheck = wallNowMs();
  changed();
}

}  // namespace

// ---- keys ----

bool isRssKey(const std::string& key) { return key.compare(0, 4, "rss:") == 0 && key.size() == 20; }
std::string rssKey(const std::string& id) { return "rss:" + id; }
std::string rssIdOf(const std::string& key) { return isRssKey(key) ? key.substr(4) : ""; }
std::string rssShowSpeedKey(const std::string& showId) { return "rssshow:" + showId; }
std::string rssSpeedKey(const std::string& key) {
  const EpRec* p = kept(rssIdOf(key));
  return p && !p->e.show.empty() ? rssShowSpeedKey(p->e.show) : key;
}
namespace {
// a query's value with as little escaped as the local server's reading needs (a link stays about its
// own length)
std::string qv(const std::string& s) {
  static const char* hx = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (c <= 0x20 || c >= 0x7f || c == '%' || c == '&' || c == '#' || c == '+' || c == '"' || c == '<' || c == '>') o += '%', o += hx[c >> 4], o += hx[c & 15];
    else o += char(c);
  }
  return o;
}
}  // namespace

std::string rssStreamPath(const RssEpisode& e) {
  std::string p = std::string(kFilePrefix) + "s=" + e.show + "&e=" + e.id, u = qv(e.url);
  if (p.size() + 3 + u.size() < 500) p += "&u=" + u;
  return p;
}
bool isRssStream(const std::string& path) { return path.compare(0, strlen(kFilePrefix), kFilePrefix) == 0; }
bool rssUpNextWaiting() {
  readStore();
  return !g_upNext.empty();
}

bool rssDetails(const std::string& key, BookPlay& d) {
  EpRec* p = kept(rssIdOf(key));
  if (!p) return false;
  const RssEpisode& e = p->e;
  d = BookPlay();
  d.key = key, d.title = e.title, d.author = e.showTitle.empty() ? showTitleOf(e.show) : e.showTitle;
  d.speedKey = rssSpeedKey(key);
  bool onCard = fileThere(p->file);
  std::string path = onCard ? p->file : rssStreamPath(e);
  if (!onCard) rememberStream(path, streamItemOf(e));
  int64_t len = e.duration > 0 ? int64_t(e.duration) * 1000 : 0;
  std::vector<FileChapter> chapters = decodeChapters(p->chapters);
  if (onCard) {
    if (const BookFileInfo* info = g_library.bookFileInfo(path)) {
      if (info->durationMs > 0) len = info->durationMs;
      if (chapters.empty() && info->chapters.size() >= 2) chapters = info->chapters;
    } else if (const Track* t = g_library.find(path)) {
      if (t->durationSec > 0) len = int64_t(t->durationSec) * 1000;
    }
  }
  for (FileChapter& c : chapters)
    if (c.endMs <= c.startMs && len > 0) c.endMs = len;
  d.paths = {path};
  d.fileMs = {len};
  d.lengthMs = len;
  d.chapters = buildChapters(d.fileMs, {chapters.size() >= 2 ? chapters : std::vector<FileChapter>()}, {e.title});
  d.skips = skipsTime(d.chapters);
  if (!d.chapters.empty() && d.skips) d.chapters[0].title = e.title;
  return true;
}

void rssFinishedChanged(const std::string& key, bool finished) {
  std::string id = rssIdOf(key);
  EpRec* p = kept(id);
  if (!p) return;
  changed();
  if (!finished) return;
  const ShowRec* rec = showRec(p->e.show);
  if (rec && rec->autoDl.deletePlayed && fileThere(p->file)) g_deleteWhenDone.insert(id);
}

bool rssEnded(const std::string& key) {
  (void)key;
  return startNext();
}

void rssRestored(const std::string& key, const std::string& path) {
  if (EpRec* p = kept(rssIdOf(key)))
    if (isStream(path)) rememberStream(path, streamItemOf(p->e));
}

// ---- reading ----

bool parseRssFeed(const std::string& json, RssShow& s, std::vector<RssEpisode>& eps) {
  yyjson_doc* d = yyjson_read(json.data(), json.size(), 0);
  if (!d) return false;
  yyjson_val* r = yyjson_doc_get_root(d);
  s = showFrom(r);
  if (s.id.empty() && !s.feed.empty()) s.id = rssid::showId(s.feed);
  size_t i, n;
  yyjson_val* v;
  eps.reserve(yyjson_arr_size(yyjson_obj_get(r, "episodes")));
  yyjson_arr_foreach(yyjson_obj_get(r, "episodes"), i, n, v) {
    RssEpisode e = episodeFrom(v);
    if (e.id.empty()) continue;
    e.show = s.id, e.showTitle = s.title;
    eps.push_back(std::move(e));
  }
  yyjson_doc_free(d);
  return !s.id.empty();
}

std::vector<RssShow> parseRssShows(const std::string& json) {
  std::vector<RssShow> out;
  yyjson_doc* d = yyjson_read(json.data(), json.size(), 0);
  if (!d) return out;
  size_t i, n;
  yyjson_val* v;
  yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(d), "items"), i, n, v) {
    RssShow s = showFrom(v);
    if (s.id.empty() && !s.feed.empty()) s.id = rssid::showId(s.feed);
    if (!s.feed.empty()) out.push_back(std::move(s));
  }
  yyjson_doc_free(d);
  return out;
}

void sortRssEpisodes(std::vector<RssEpisode>& eps, bool serial) {
  if (!serial) {
    std::stable_sort(eps.begin(), eps.end(), [](const RssEpisode& a, const RssEpisode& b) { return a.date > b.date; });
    return;
  }
  // a story: season and episode numbers when it has them, else the oldest first
  std::stable_sort(eps.begin(), eps.end(), [](const RssEpisode& a, const RssEpisode& b) {
    if (a.number && b.number && (a.season != b.season || a.number != b.number))
      return a.season != b.season ? a.season < b.season : a.number < b.number;
    return a.date < b.date;
  });
}

std::string rssFeedPath(const RssShow& s, bool fresh, int64_t since, bool urls) {
  std::string p = std::string(Api::kLocal) + "rss/feed?url=" + Api::escape(s.feed);
  if (!urls) p += "&urls=0";
  if (s.priv) p += "&private=1";
  if (fresh) p += "&fresh=1";
  if (since > 0) p += "&since=" + std::to_string(since);
  return p;
}

std::string rssArt(const std::string& url) {
  std::string u = qv(url);
  // (the picture loader keeps 511 bytes of a path, its size added: a longer link has no picture)
  return url.empty() || u.size() > 440 ? "" : std::string(Api::kLocal) + "rss/art?u=" + u;
}

// ---- subscriptions ----

std::vector<RssShow> rssShows() {
  readStore();
  std::vector<RssShow> v;
  for (auto& [id, rec] : g_shows)
    if (rec.subscribed) v.push_back(rec.s);
  std::stable_sort(v.begin(), v.end(), [](const RssShow& a, const RssShow& b) { return a.updated > b.updated; });
  return v;
}

bool rssSubscribed(const std::string& showId) {
  const ShowRec* r = showRec(showId);
  return r && r->subscribed;
}

const RssShow* rssShow(const std::string& showId) {
  const ShowRec* r = showRec(showId);
  return r ? &r->s : nullptr;
}

void rssNoteFeed(const RssShow& in, const std::vector<RssEpisode>& eps) {
  readStore();
  ShowRec* rec = showRec(in.id);
  if (!rec) {
    // not subscribed: kept only while an episode of it is (its name and picture for those rows)
    bool any = false;
    for (auto& [id, p] : g_eps) any = any || p.e.show == in.id;
    if (!any) {
      RssShow& seen = g_seen[in.id];
      std::string art = seen.art;
      seen = in;
      if (in.art.empty() || (art.find("mzstatic.com/") != std::string::npos && in.art.find("mzstatic.com/") == std::string::npos)) seen.art = art;
      if (g_seen.size() > 200) g_seen.clear();
      return;
    }
    rec = &g_shows[in.id];
    rec->s = in;
  }
  RssShow& s = rec->s;
  s.id = in.id;
  if (!in.feed.empty()) s.feed = in.feed;
  if (!in.title.empty()) s.title = in.title;
  if (!in.author.empty()) s.author = in.author;
  if (!in.description.empty()) s.description = in.description;
  if (!in.link.empty()) s.link = in.link;
  if (!in.genre.empty()) s.genre = in.genre;
  if (in.apple) s.apple = in.apple;
  // Apple's picture is kept once had (it's sized for the Y1); else the feed's
  if (!in.art.empty() && (s.art.empty() || s.art.find("mzstatic.com/") == std::string::npos || in.art.find("mzstatic.com/") != std::string::npos))
    s.art = in.art;
  s.serial = in.serial;
  if (in.count) s.count = in.count;
  mergeLatest(*rec, eps);
  for (const RssEpisode& e : eps)  // what's kept of these up to date (their link may have changed)
    if (EpRec* p = kept(e.id)) {
      std::string file = p->file;
      p->e.title = e.title, p->e.type = e.type, p->e.duration = e.duration ? e.duration : p->e.duration;
      if (!e.url.empty()) p->e.url = e.url;  // (a show's page comes without links)
      p->e.chapters = e.chapters, p->e.image = e.image, p->e.date = e.date, p->e.showTitle = s.title;
    }
  changed();
}

void rssSubscribe(const RssShow& s, const std::vector<RssEpisode>& eps) {
  readStore();
  ShowRec& rec = g_shows[s.id];
  bool was = rec.subscribed;
  if (!was) {
    rec.s = s;
    rec.subscribed = true;
    rec.subscribedAt = wallNowMs();
    rec.checkedAt = wallNowMs();
    // its newest counts as new; what came before doesn't
    int64_t newest = 0;
    for (const RssEpisode& e : eps) newest = std::max(newest, e.date);
    rec.newSince = newest > 0 ? newest - 1 : int64_t(time(nullptr));
  }
  rssNoteFeed(s, eps);
  changed();
  writeStore();
}

void rssUnsubscribe(const std::string& showId, bool deleteDownloads) {
  ShowRec* rec = showRec(showId);
  if (!rec) return;
  rec->subscribed = false;
  rec->autoDl = RssAuto();
  if (deleteDownloads) {
    std::vector<std::string> ids;
    for (auto& [id, p] : g_eps)
      if (p.e.show == showId && fileThere(p.file)) ids.push_back(id);
    for (const std::string& id : ids) deleteRssDownload(id);
  }
  for (const DownloadJob& j : std::vector<DownloadJob>(downloadJobs()))  // its downloads waiting go too
    if (j.rss && j.state != DownloadJob::Done && j.state != DownloadJob::Failed)
      if (const EpRec* p = kept(j.id))
        if (p->e.show == showId && p->autoDl) cancelDownload(j.id);
  changed();
  writeStore();
}

RssAuto rssAuto(const std::string& showId) {
  const ShowRec* r = showRec(showId);
  return r ? r->autoDl : RssAuto();
}

void setRssAuto(const std::string& showId, const RssAuto& a) {
  ShowRec* r = showRec(showId);
  if (!r) return;
  r->autoDl = a;
  r->autoDl.keep = std::max(1, std::min(10, a.keep));
  changed();
  writeStore();
  if (a.on && online()) autoDownload(showId);
}

void rssAddFeed(const std::string& url, bool priv, std::function<void(bool, const std::string&)> done) {
  RssShow s;
  s.feed = url;
  s.priv = priv;
  s.id = rssid::showId(url);
  if (rssSubscribed(s.id)) return done(true, rssShow(s.id)->title);
  struct Try {
    static void go(RssShow s, int left, std::function<void(bool, const std::string&)> done) {
      g_core.get(rssFeedPath(s, true), 0, [s, left, done](int status, const std::string& body) {
        if (status == 202 && left > 0) {
          // still being read: again in a moment (the core answers within its 30 s each time)
          return go(s, left - 1, done);
        }
        RssShow got;
        std::vector<RssEpisode> eps;
        if (status != 200 || !parseRssFeed(body, got, eps)) {
          std::string why = status == 422 ? tr("rss.add.notFeed") : status == 0 || status == 504 ? tr("rss.add.offline") : tr("rss.add.failed");
          return done(false, why);
        }
        got.priv = s.priv;
        got.id = s.id;  // the address it was added by is its name, even if it moved since
        rssSubscribe(got, eps);
        done(true, got.title);
      });
    }
  };
  Try::go(s, 6, done);
}

namespace {
std::string xmlAttr(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}
std::string xmlUnescape(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '&') {
      o += s[i];
      continue;
    }
    size_t semi = s.find(';', i);
    if (semi == std::string::npos || semi - i > 10) {
      o += s[i];
      continue;
    }
    std::string ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp") o += '&';
    else if (ent == "lt") o += '<';
    else if (ent == "gt") o += '>';
    else if (ent == "quot") o += '"';
    else if (ent == "apos") o += '\'';
    else if (!ent.empty() && ent[0] == '#') {
      long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtol(ent.c_str() + 2, nullptr, 16) : strtol(ent.c_str() + 1, nullptr, 10);
      if (cp < 0x80) o += char(cp);
      else if (cp < 0x800) o += char(0xc0 | (cp >> 6)), o += char(0x80 | (cp & 0x3f));
      else o += char(0xe0 | (cp >> 12)), o += char(0x80 | ((cp >> 6) & 0x3f)), o += char(0x80 | (cp & 0x3f));
    } else {
      o += s.substr(i, semi - i + 1);
    }
    i = semi;
  }
  return o;
}
// an address that looks like it has a secret in it (a private or paid feed): only ever read here
bool looksPrivate(const std::string& url) {
  std::string u = url;
  for (char& c : u) c = char(tolower((unsigned char)c));
  size_t host = u.find("://");
  std::string rest = host == std::string::npos ? u : u.substr(host + 3);
  size_t slash = rest.find('/');
  std::string h = rest.substr(0, slash);
  if (h.find('@') != std::string::npos || u.find('?') != std::string::npos) return true;
  for (const char* p : {"patreon", "supercast", "memberful", "glow.fm", "supporter", "private", "premium", "subscribe"})
    if (u.find(p) != std::string::npos) return true;
  return false;
}
// the OPML file's adds, one at a time
struct Import {
  std::vector<std::string> urls;
  size_t at = 0;
  int added = 0, already = 0, failed = 0;
  std::function<void(int, int, int)> done;
};
std::shared_ptr<Import> g_import;
int g_imported[3] = {-1, 0, 0};  // the last file's: added, already there, failed (-1: none yet)
void importNext() {
  auto im = g_import;
  if (!im) return;
  if (im->at >= im->urls.size()) {
    g_import.reset();
    if (im->done) im->done(im->added, im->already, im->failed);
    return;
  }
  std::string url = im->urls[im->at++];
  if (rssSubscribed(rssid::showId(url))) {
    im->already++;
    return importNext();
  }
  rssAddFeed(url, looksPrivate(url), [im](bool ok, const std::string&) {
    ok ? im->added++ : im->failed++;
    importNext();
  });
}
}  // namespace

std::string rssOpml() {
  std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<opml version=\"2.0\">\n  <head>\n    <title>Rondo podcasts</title>\n  </head>\n  <body>\n";
  for (const RssShow& s : rssShows())
    o += "    <outline type=\"rss\" text=\"" + xmlAttr(s.title) + "\" title=\"" + xmlAttr(s.title) + "\" xmlUrl=\"" + xmlAttr(s.feed) + "\"" +
         (s.link.empty() ? "" : " htmlUrl=\"" + xmlAttr(s.link) + "\"") + "/>\n";
  return o + "  </body>\n</opml>\n";
}

int rssImportOpml(const std::string& opml, std::function<void(int, int, int)> done) {
  std::vector<std::string> urls;
  std::set<std::string> seen;
  for (size_t at = opml.find("<outline"); at != std::string::npos; at = opml.find("<outline", at + 8)) {
    size_t end = opml.find('>', at);
    if (end == std::string::npos) break;
    std::string tag = opml.substr(at, end - at);
    // xmlUrl="…" (any case of the name, either quote)
    std::string lower = tag;
    for (char& c : lower) c = char(tolower((unsigned char)c));
    size_t k = lower.find("xmlurl");
    if (k == std::string::npos) continue;
    size_t eq = tag.find('=', k);
    if (eq == std::string::npos) continue;
    size_t q = tag.find_first_of("\"'", eq);
    if (q == std::string::npos) continue;
    size_t q2 = tag.find(tag[q], q + 1);
    if (q2 == std::string::npos) continue;
    std::string url = xmlUnescape(tag.substr(q + 1, q2 - q - 1));
    size_t a = url.find_first_not_of(" \t\r\n"), b = url.find_last_not_of(" \t\r\n");
    url = a == std::string::npos ? "" : url.substr(a, b - a + 1);
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) continue;
    if (seen.insert(rssid::showId(url)).second) urls.push_back(url);
  }
  if (urls.empty() || g_import) return urls.empty() ? 0 : -1;
  g_import = std::make_shared<Import>();
  g_import->urls = urls;
  g_import->done = [done](int added, int already, int failed) {
    g_imported[0] = added, g_imported[1] = already, g_imported[2] = failed;
    changed();
    if (done) done(added, already, failed);
  };
  importNext();
  return int(urls.size());
}

// ---- the lists ----

std::vector<RssEpisode> rssNewEpisodes() {
  readStore();
  std::vector<RssEpisode> v;
  int64_t since = (wallNowMs() - kNewForMs) / 1000;
  for (auto& [id, rec] : g_shows) {
    if (!rec.subscribed) continue;
    for (const RssEpisode& e : rec.latest) {
      if (e.date <= rec.newSince || e.date < since) continue;
      const BookState* st = g_library.bookState(rssKey(e.id));
      if (st && st->finished) continue;
      v.push_back(e);
    }
  }
  std::stable_sort(v.begin(), v.end(), [](const RssEpisode& a, const RssEpisode& b) { return a.date > b.date; });
  return v;
}

int rssNewCount(const std::string& showId) {
  int n = 0;
  for (const RssEpisode& e : rssNewEpisodes()) n += showId.empty() || e.show == showId;
  return n;
}

std::vector<RssEpisode> rssInProgress() {
  readStore();
  std::vector<std::pair<int64_t, RssEpisode>> v;
  for (auto& [id, p] : g_eps) {
    const BookState* st = g_library.bookState(rssKey(id));
    if (st && st->lastPlayed > 0 && !st->finished) v.push_back({st->lastPlayed, p.e});
  }
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<RssEpisode> out;
  for (auto& [t, e] : v) out.push_back(e);
  return out;
}

std::vector<RssEpisode> rssLater() {
  readStore();
  std::vector<std::pair<int64_t, RssEpisode>> v;
  for (auto& [id, p] : g_eps)
    if (p.laterAt) v.push_back({p.laterAt, p.e});
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<RssEpisode> out;
  for (auto& [t, e] : v) out.push_back(e);
  return out;
}

bool rssIsLater(const std::string& id) {
  const EpRec* p = kept(id);
  return p && p->laterAt;
}

void setRssLater(const RssEpisode& e, bool later) {
  rssKeep(e);
  if (EpRec* p = kept(e.id)) p->laterAt = later ? wallNowMs() : 0;
  changed();
}

std::vector<RssEpisode> rssDownloaded() {
  readStore();
  std::vector<std::pair<int64_t, RssEpisode>> v;
  for (auto& [id, p] : g_eps)
    if (fileThere(p.file)) v.push_back({p.e.date, p.e});
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<RssEpisode> out;
  for (auto& [t, e] : v) out.push_back(e);
  return out;
}

std::vector<RssEpisode> rssUpNext() {
  readStore();
  std::vector<RssEpisode> v;
  for (const std::string& id : g_upNext)
    if (const EpRec* p = kept(id)) v.push_back(p->e);
  return v;
}

void rssRemoveFromUpNext(const std::string& id) {
  g_upNext.erase(std::remove(g_upNext.begin(), g_upNext.end(), id), g_upNext.end());
  if (g_upNext.empty()) g_afterSong = false;
  changed();
}

std::string rssCountry() {
  readStore();
  return g_country;
}

void setRssCountry(const std::string& code) {
  readStore();
  if (code.size() != 2 || code == g_country) return;
  g_country = code;
  changed();
  writeStore();
}

std::string rssCountryName(const std::string& code) { return tr("rss.country." + code); }

// ---- playing ----

namespace {
// its chapters (its chapter file's, else the MP3's own), once: the one playing takes them at once
void askChapters(const std::string& id) {
  EpRec* p = kept(id);
  if (!p || p->chaptersAsked) return;
  bool mp3 = p->e.type == "audio/mpeg" || p->e.url.find(".mp3") != std::string::npos;
  if (p->e.chapters.empty() && !mp3) return;  // an .m4a's: the file's own, read once on the card
  p->chaptersAsked = true;
  std::string path = std::string(Api::kLocal) + (!p->e.chapters.empty() ? "rss/chapters?u=" + Api::escape(p->e.chapters)
                                                  : "rss/id3?s=" + p->e.show + "&e=" + id + (p->e.url.empty() ? "" : "&u=" + Api::escape(p->e.url)));
  g_core.get(path, 600000, [id](int status, const std::string& body) {
    EpRec* p = kept(id);
    if (!p) return;
    if (status != 200) {
      if (status == 0) p->chaptersAsked = false;  // no connection: asked again next time
      return;
    }
    std::vector<FileChapter> cs;
    if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
      size_t i, n;
      yyjson_val* c;
      std::vector<std::pair<int64_t, std::string>> starts;
      yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(d), "chapters"), i, n, c) starts.push_back({ival(c, "ms"), sval(c, "title")});
      int64_t len = p->e.duration > 0 ? int64_t(p->e.duration) * 1000 : 0;
      for (size_t k = 0; k < starts.size(); k++) {
        FileChapter f;
        f.startMs = starts[k].first;
        f.endMs = k + 1 < starts.size() ? starts[k + 1].first : len;
        f.title = starts[k].second;
        cs.push_back(f);
      }
      yyjson_doc_free(d);
    }
    if (cs.size() < 2) return;
    p->chapters = encodeChapters(cs);
    changed();
    BookPlay d;
    if (rssDetails(rssKey(id), d)) spokenDetailsCame(d);
  });
}
}  // namespace

void rssKeep(const RssEpisode& e) {
  readStore();
  EpRec& p = g_eps[e.id];
  RssEpisode& k = p.e;
  k.id = e.id;
  if (!e.show.empty()) k.show = e.show;
  if (!e.showTitle.empty()) k.showTitle = e.showTitle;
  if (!e.guid.empty()) k.guid = e.guid;
  if (!e.title.empty()) k.title = e.title;
  if (!e.url.empty()) k.url = e.url;
  if (!e.type.empty()) k.type = e.type;
  if (!e.image.empty()) k.image = e.image;
  if (!e.chapters.empty()) k.chapters = e.chapters;
  if (e.date) k.date = e.date;
  if (e.duration > 0) k.duration = e.duration;
  if (e.size > 0) k.size = e.size;
  if (e.season) k.season = e.season;
  if (e.number) k.number = e.number;
  k.episodeType = e.episodeType;
  p.used = wallNowMs();
  if (k.showTitle.empty()) k.showTitle = showTitleOf(k.show);
  // its show, for its name, picture and notes (one not subscribed to: as its page or a search had it)
  if (!k.show.empty() && !showRec(k.show)) {
    auto seen = g_seen.find(k.show);
    if (seen != g_seen.end()) g_shows[k.show].s = seen->second;
  }
  changed();
}

const RssEpisode* rssKept(const std::string& id) {
  const EpRec* p = kept(id);
  return p ? &p->e : nullptr;
}

void playRss(const RssEpisode& e) {
  rssKeep(e);
  if (e.video()) {
    EpRec* p = kept(e.id);
    Item it = streamItemOf(e);
    it.id = p && fileThere(p->file) ? p->file : rssStreamPath(p ? p->e : e);
    openVideoEpisode(it, rssKey(e.id));
    return;
  }
  BookPlay d;
  if (!rssDetails(rssKey(e.id), d)) return;
  playSpokenDetails(d);
  askChapters(e.id);
}

void listenRss(const RssEpisode& e) {
  rssKeep(e);
  BookPlay d;
  if (!rssDetails(rssKey(e.id), d)) return;
  playSpokenDetails(d);
}

void playRssFrom(const RssEpisode& e, int64_t ms) {
  rssKeep(e);
  if (e.video()) {
    BookState st;
    if (const BookState* old = g_library.bookState(rssKey(e.id))) st = *old;
    st.key = rssKey(e.id), st.positionMs = ms, st.finished = false, st.lastPlayed = ms ? wallNowMs() : 0;
    g_library.setBookState(st);
    return playRss(e);
  }
  playSpokenFrom(rssKey(e.id), {0, ms});
  askChapters(e.id);
}

void queueRss(const RssEpisode& e, bool next) {
  rssKeep(e);
  g_upNext.erase(std::remove(g_upNext.begin(), g_upNext.end(), e.id), g_upNext.end());
  const BookPlay* now = bookNow();
  PlayerState s = g_player.state();
  bool music = !now && g_player.active() && s.index >= 0 && s.playing && !s.video && !s.fmOn;
  if (!now && !music) {  // nothing plays: at once
    changed();
    playRss(e);
    if (!e.video()) openNowPlaying();
    return;
  }
  if (next) g_upNext.insert(g_upNext.begin(), e.id);
  else g_upNext.push_back(e.id);
  changed();
  if (music && !g_afterSong) g_afterSong = true, g_afterSerial = s.trackSerial;
  g_app->toast(now ? (isEpisodeKey(now->key) || isRssKey(now->key) ? tr("podcasts.afterThisEpisode") : tr("podcasts.afterThisBook"))
                   : tr("podcasts.afterThisSong"));
}

void markRssPlayed(const RssEpisode& e, bool played) {
  rssKeep(e);
  markSpokenFinished(rssKey(e.id), played);
  changed();
}

bool rssStarted(const RssEpisode& e, bool* finished) {
  const BookState* st = g_library.bookState(rssKey(e.id));
  if (finished) *finished = st && st->finished;
  return st && st->lastPlayed > 0 && !st->finished;
}

bool rssNew(const RssEpisode& e) {
  const BookState* st = g_library.bookState(rssKey(e.id));
  return !(st && (st->lastPlayed > 0 || st->finished));
}

std::string rssSubtitle(const RssEpisode& e, bool withShow) {
  std::string date;
  if (e.date > 0) {
    time_t t = time_t(e.date), now = time(nullptr);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    date = a.tm_year == b.tm_year ? clockDate(t, false)
                                  : tr("rss.date.withYear", {{"date", clockDate(t, false)}, {"year", std::to_string(a.tm_year + 1900)}});
  }
  std::string state;
  const BookState* st = g_library.bookState(rssKey(e.id));
  int64_t len = e.duration > 0 ? int64_t(e.duration) * 1000 : 0;
  if (st && st->finished) state = tr("podcasts.played");
  else if (st && st->lastPlayed > 0 && len > 0)
    state = tr("book.left", {{"time", listenTime(std::max<int64_t>(0, len - st->positionMs), spokenSpeed(rssKey(e.id)))}});
  else if (st && st->lastPlayed > 0) state = tr("podcasts.started");
  else if (len > 0) state = listenTime(len, 100);
  std::string kind = e.video() ? tr("rss.video") : e.episodeType == "trailer" ? tr("rss.trailer") : e.episodeType == "bonus" ? tr("rss.bonus") : "";
  std::string out;
  for (const std::string& part : {withShow ? (e.showTitle.empty() ? showTitleOf(e.show) : e.showTitle) : std::string(), date, kind, state})
    if (!part.empty()) out += (out.empty() ? "" : " \xc2\xb7 ") + part;
  return out;
}

void rssNotes(const RssEpisode& e, std::function<void(const std::string&)> done) {
  const RssShow* s = rssShow(e.show);
  if (!s) return done("");
  g_core.get(std::string(Api::kLocal) + "rss/notes?url=" + Api::escape(s->feed) + "&guid=" + Api::escape(e.guid), 0,
             [done](int status, const std::string& body) {
               std::string notes;
               if (status == 200)
                 if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
                   notes = sval(yyjson_doc_get_root(d), "notes");
                   yyjson_doc_free(d);
                 }
               done(notes);
             });
}

void rssVideoPlace(const std::string& key, int64_t ms, int64_t durationMs, bool ended) {
  if (!isRssKey(key)) return;
  BookState st;
  const BookState* old = g_library.bookState(key);
  if (old) st = *old;
  bool was = old && old->finished;
  st.key = key;
  st.positionMs = ms;
  st.finished = ended || finishedAt(ms, durationMs);
  st.lastPlayed = wallNowMs();
  if (EpRec* p = kept(rssIdOf(key))) {
    if (durationMs > 0 && p->e.duration <= 0) p->e.duration = int(durationMs / 1000), changed();
    st.file = fileThere(p->file) ? p->file : rssStreamPath(p->e);
  }
  g_library.setBookState(st);
  if (st.finished != was) rssFinishedChanged(key, st.finished);
}

int64_t rssVideoStart(const std::string& key) {
  const BookState* st = g_library.bookState(key);
  if (!st || st->finished || st->positionMs <= 0) return 0;
  int64_t rw = st->lastPlayed ? rewindAfterPause(wallNowMs() - st->lastPlayed) : 0;
  return std::max<int64_t>(0, st->positionMs - rw);
}

// ---- downloads ----

bool downloadRss(const RssEpisode& e, bool quiet, bool automatic) {
  rssKeep(e);
  EpRec* p = kept(e.id);
  if (!p) return false;
  if (!fileThere(p->file)) p->autoDl = automatic;
  if (!automatic) p->noAuto = false;
  writeStore();
  Item it = streamItemOf(e);
  return downloadRssJob(it, rssStreamPath(p->e), quiet);
}

std::string downloadedRss(const std::string& id) {
  const EpRec* p = kept(id);
  return p && fileThere(p->file) ? p->file : "";
}

bool deleteRssDownload(const std::string& id) {
  EpRec* p = kept(id);
  if (!p || p->file.empty()) return false;
  std::string file = p->file;
  // playing from it now: on from the stream, where it is
  if (const BookPlay* b = bookNow())
    if (b->key == rssKey(id) && !b->paths.empty() && b->paths[0] == file) {
      PlayerState s = g_player.state();
      int64_t at = PlayerHost::positionAt(s, now_ns());
      bool playing = s.playing && !s.ended;
      p->file.clear();
      playSpokenFrom(rssKey(id), {0, at}, playing);
    }
  bool ok = unlink(file.c_str()) == 0 || !fileThere(file);
  // one "Download new episodes" fetched, deleted by hand: not fetched again by itself
  const ShowRec* rec = showRec(p->e.show);
  if (rec && rec->autoDl.on) p->noAuto = true;
  p->file.clear();
  p->autoDl = false;
  changed();
  writeStore();
  rmdir(file.substr(0, file.rfind('/')).c_str());  // its show's folder goes when it's empty
  g_library.scanAsync();
  return ok;
}

void rssDownloaded(const std::string& id, const std::string& file) {
  readStore();
  EpRec& p = g_eps[id];
  p.e.id = id;
  p.file = file;
  p.downloadedAt = wallNowMs();
  p.used = p.downloadedAt;
  if (const BookPlay* b = bookNow())  // the one playing, streamed: on from the card, where it is
    if (b->key == rssKey(id) && !b->paths.empty() && isStream(b->paths[0])) {
      PlayerState s = g_player.state();
      playSpokenFrom(rssKey(id), {0, PlayerHost::positionAt(s, now_ns())}, s.playing && !s.ended);
    }
  changed();
  writeStore();
}

std::string rssDownloadFile(const std::string& id) {
  const EpRec* p = kept(id);
  if (!p) return "";
  const RssEpisode& e = p->e;
  std::string show = cardName(e.showTitle.empty() ? showTitleOf(e.show) : e.showTitle);
  if (show.empty()) show = "Podcast";
  if (show.size() > 80) show.resize(80);
  std::string title = cardName(e.title);
  if (title.size() > 110) title.resize(110);
  if (title.empty()) title = "Episode";
  // its extension: from its type, else its address
  std::string ext;
  const std::string& t = e.type;
  if (t == "audio/mpeg" || t == "audio/mp3") ext = "mp3";
  else if (t == "audio/mp4" || t == "audio/x-m4a" || t == "audio/m4a" || t == "audio/aac" || t == "audio/x-m4b") ext = t == "audio/aac" ? "aac" : "m4a";
  else if (t == "video/mp4" || t == "video/x-m4v") ext = "mp4";
  else if (t == "video/quicktime") ext = "mov";
  else if (t == "audio/ogg" || t == "audio/opus") ext = "ogg";
  if (ext.empty()) {
    std::string path = e.url.substr(0, e.url.find('?'));
    size_t dot = path.rfind('.');
    if (dot != std::string::npos && path.size() - dot <= 5 && path.find('/', dot) == std::string::npos) ext = path.substr(dot + 1);
    for (char& c : ext) c = char(tolower((unsigned char)c));
    if (ext != "mp3" && ext != "m4a" && ext != "mp4" && ext != "m4v" && ext != "aac" && ext != "ogg" && ext != "opus" && ext != "mov") ext = "mp3";
  }
  std::string date;
  if (e.date > 0) {
    time_t tt = time_t(e.date);
    struct tm tm;
    gmtime_r(&tt, &tm);
    char b[16];
    strftime(b, sizeof(b), "%Y-%m-%d ", &tm);
    date = b;
  }
  std::string dir = podcastDownloadDir() + "/" + show, base = dir + "/" + date + title;
  std::string file = base + "." + ext;
  for (int k = 2; fileThere(file); k++) file = base + " (" + std::to_string(k) + ")." + ext;
  return file;
}

bool rssDownloadInfo(const std::string& id, std::string& url, std::string& title, std::string& show, std::string& thumb) {
  const EpRec* p = kept(id);
  if (!p) return false;
  url = p->e.url, title = p->e.title, show = p->e.showTitle, thumb = episodeArt(p->e);
  return true;
}

// ---- new episodes ----

void rssCheck(bool opening) {
  readStore();
  if (g_checking || !online()) return;
  int64_t now = wallNowMs();
  if (now - g_lastCheck < (opening ? kOpenCheckMs : kCheckEveryMs) && now >= g_lastCheck) return;
  g_toCheck.clear();
  for (auto& [id, rec] : g_shows)
    if (rec.subscribed) g_toCheck.push_back(id);
  if (g_toCheck.empty()) return;
  g_checking = true;
  g_retries = 0;
  changed();
  checkNext();
}

void rssCheckNow() {
  readStore();
  if (g_checking) return;
  g_lastCheck = 0;
  rssCheck(false);
}

bool rssChecking() { return g_checking; }
int64_t rssCheckedAt() {
  readStore();
  return g_lastCheck;
}

// ---- the loop ----

bool rssTick(int64_t now) {
  readStore();
  bool shown = false;
  PlayerState s = g_player.state();
  // the one playing, its length not known yet: the player's, once it says
  if (const BookPlay* b = bookNow())
    if (isRssKey(b->key) && b->lengthMs <= 0 && s.durationMs > 0)
      if (EpRec* p = kept(rssIdOf(b->key))) {
        p->e.duration = int(s.durationMs / 1000);
        changed();
        BookPlay d;
        if (rssDetails(b->key, d)) spokenDetailsCame(d), shown = true;
      }
  // Play next / Add to queue while music played: the song is over
  if (g_afterSong) {
    if (bookNow() || g_upNext.empty()) g_afterSong = false;
    else if (s.trackSerial != g_afterSerial || s.ended) {
      g_afterSong = false;
      if (startNext()) shown = true;
    }
  }
  // played, and its show deletes played ones: once it isn't the one playing
  if (!g_deleteWhenDone.empty()) {
    const BookPlay* b = bookNow();
    for (auto it = g_deleteWhenDone.begin(); it != g_deleteWhenDone.end();) {
      if (b && b->key == rssKey(*it)) {
        ++it;
        continue;
      }
      const BookState* st = g_library.bookState(rssKey(*it));
      if (st && st->finished) deleteRssDownload(*it);
      it = g_deleteWhenDone.erase(it);
      shown = true;
    }
  }
  // a feed still being read: asked again
  if (g_retryAt && now >= g_retryAt) {
    g_retryAt = 0;
    std::string id = g_checkingNow;
    if (const ShowRec* rec = showRec(id)) {
      int64_t newest = 0;
      for (const RssEpisode& e : rec->latest) newest = std::max(newest, e.date);
      g_core.get(rssFeedPath(rec->s, false, newest > 0 ? newest - 1 : 0), 0, [id](int status, const std::string& body) { checked(id, status, body); });
    } else {
      checkNext();
    }
  }
  // every 4 hours while awake (the loop only runs then), on Wi-Fi
  static int64_t lookedAt = 0;
  if (now - lookedAt > 60 * kSec) {
    lookedAt = now;
    rssCheck(false);
  }
  if (g_dirty && now - g_writtenAt > 2 * kSec) writeStore();
  return shown;
}

void rssSaveNow() {
  if (g_dirty) writeStore();
}

uint32_t rssSerial() { return g_serial; }

// ---- the dashboard's ----

std::string rssEpisodeJson(const RssEpisode& e) {
  const BookState* st = g_library.bookState(rssKey(e.id));
  const EpRec* p = kept(e.id);
  const BookPlay* b = bookNow();
  std::string o = episodeJsonKept(e);
  o += ",\"art\":" + jq(episodeArt(e)) + ",\"video\":" + (e.video() ? "true" : "false") + ",\"subtitle\":" + jq(rssSubtitle(e, false)) +
       ",\"new\":" + (rssNew(e) ? "true" : "false") + ",\"played\":" + (st && st->finished ? "true" : "false") +
       ",\"positionMs\":" + std::to_string(st && st->lastPlayed > 0 && !st->finished ? st->positionMs : 0) +
       ",\"onCard\":" + (p && fileThere(p->file) ? "true" : "false") + ",\"later\":" + (p && p->laterAt ? "true" : "false") +
       ",\"playing\":" + (b && b->key == rssKey(e.id) ? "true" : "false");
  for (const DownloadJob& j : downloadJobs())
    if (j.rss && j.id == e.id && (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running || j.state == DownloadJob::Paused))
      o += ",\"downloading\":" + std::to_string(j.percent);
  return o + "}";
}

std::string rssShowJson(const RssShow& s) {
  const ShowRec* r = showRec(s.id);
  RssAuto a = r ? r->autoDl : RssAuto();
  return "{\"id\":" + jq(s.id) + "," + showJsonKept(s) + ",\"thumb\":" + jq(rssArt(s.art)) + ",\"subscribed\":" + (r && r->subscribed ? "true" : "false") +
         ",\"newCount\":" + std::to_string(r && r->subscribed ? rssNewCount(s.id) : 0) + ",\"auto\":{\"on\":" + (a.on ? "true" : "false") +
         ",\"keep\":" + std::to_string(a.keep) + ",\"deletePlayed\":" + (a.deletePlayed ? "true" : "false") +
         "},\"speed\":" + std::to_string(spokenSpeed(rssShowSpeedKey(s.id))) + "}";
}

std::string rssStateJson() {
  auto list = [](const std::vector<RssEpisode>& v) {
    std::string s = "[";
    for (size_t k = 0; k < v.size(); k++) s += (k ? "," : "") + rssEpisodeJson(v[k]);
    return s + "]";
  };
  std::string shows = "[";
  std::vector<RssShow> subs = rssShows();
  for (size_t k = 0; k < subs.size(); k++) shows += (k ? "," : "") + rssShowJson(subs[k]);
  shows += "]";
  return "{\"shows\":" + shows + ",\"newEpisodes\":" + list(rssNewEpisodes()) + ",\"inProgress\":" + list(rssInProgress()) +
         ",\"later\":" + list(rssLater()) + ",\"upNext\":" + list(rssUpNext()) + ",\"downloaded\":" + list(rssDownloaded()) +
         ",\"country\":" + jq(rssCountry()) + ",\"checking\":" + (rssChecking() ? "true" : "false") +
         ",\"checkedAt\":" + std::to_string(rssCheckedAt()) + ",\"importing\":" + (g_import ? "true" : "false") +
         (g_imported[0] >= 0 ? ",\"imported\":{\"added\":" + std::to_string(g_imported[0]) + ",\"already\":" + std::to_string(g_imported[1]) +
                                   ",\"failed\":" + std::to_string(g_imported[2]) + "}"
                             : "") +
         (g_import ? ",\"importProgress\":{\"done\":" + std::to_string(g_import->at) + ",\"total\":" + std::to_string(g_import->urls.size()) + "}" : "") + "}";
}

RssEpisode rssEpisodeFromJson(const std::string& json) {
  RssEpisode e;
  if (yyjson_doc* d = yyjson_read(json.data(), json.size(), 0)) {
    e = episodeFrom(yyjson_doc_get_root(d));
    yyjson_doc_free(d);
  }
  if (const EpRec* p = kept(e.id)) {  // what the Y1 keeps of it wins (its show, its link now)
    RssEpisode k = p->e;
    if (k.url.empty()) k.url = e.url;
    return k;
  }
  if (e.showTitle.empty()) e.showTitle = showTitleOf(e.show);
  return e;
}

RssShow rssShowFromJson(const std::string& json) {
  RssShow s;
  if (yyjson_doc* d = yyjson_read(json.data(), json.size(), 0)) {
    s = showFrom(yyjson_doc_get_root(d));
    yyjson_doc_free(d);
  }
  if (s.id.empty() && !s.feed.empty()) s.id = rssid::showId(s.feed);
  if (const RssShow* k = rssShow(s.id)) {
    RssShow mine = *k;
    if (mine.feed.empty()) mine.feed = s.feed;
    return mine;
  }
  return s;
}

}  // namespace rondo::shell
