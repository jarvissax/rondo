// podcasts.h: the episodes kept (podcasts.json), their played marks agreed with YouTube, Episodes up
// next, and episodes as spoken word (books.h's session).
#include "podcasts.h"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

#include <algorithm>
#include <map>

#include "app.h"
#include "books.h"
#include "downloads.h"
#include "file_ops.h"
#include "screens.h"

namespace rondo::shell {
namespace {

std::string dataDir() { return g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1); }
std::string storePath() { return dataDir() + "podcasts.json"; }

// what the Y1 keeps of an episode
struct Kept {
  Episode e;
  std::string file;   // the card's copy (YouTube Music › Download), "" none
  std::string cover;  // the picture its download was asked with (its show's, else its own)
  int agreed = -1;    // the Played mark the Y1 and YouTube last agreed on (-1: never compared)
  int pending = -1;   // a mark made here that YouTube hasn't been told yet (-1: none)
  int64_t used = 0;   // ms since 1970: played, queued or downloaded then
  int64_t downloadedAt = 0;
  int64_t toldAt = 0;  // when YouTube took a mark from here (ms since 1970): it shows it some seconds late
};
std::map<std::string, Kept> g_eps;
std::vector<std::string> g_upNext;  // Episodes up next (ids), in order
bool g_read = false, g_dirty = false;
int64_t g_writtenAt = 0;
int64_t g_changedAt = 0;  // the account changed (now_ns): lists asked fresh for a while
// Play next / Add to queue while music plays: the queue starts when this song is over
bool g_afterSong = false;
uint32_t g_afterSerial = 0;
int64_t g_pushAt = 0;  // the next try of marks not yet taken by YouTube (now_ns)

std::string sval(yyjson_val* o, const char* k) {
  yyjson_val* v = yyjson_obj_get(o, k);
  return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string();
}
int64_t ival(yyjson_val* o, const char* k, int64_t otherwise = 0) {
  yyjson_val* v = yyjson_obj_get(o, k);
  return yyjson_is_num(v) ? int64_t(yyjson_get_num(v)) : otherwise;
}
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

void readStore() {
  if (g_read) return;
  g_read = true;
  yyjson_doc* d = yyjson_read_file(storePath().c_str(), 0, nullptr, nullptr);
  if (!d) return;
  yyjson_val* r = yyjson_doc_get_root(d);
  size_t i, n;
  yyjson_val *k, *v;
  yyjson_obj_foreach(yyjson_obj_get(r, "episodes"), i, n, k, v) {
    Kept p;
    Episode& e = p.e;
    e.id = yyjson_get_str(k);
    e.title = sval(v, "title"), e.show = sval(v, "show"), e.showId = sval(v, "showId"), e.thumb = sval(v, "thumb");
    e.date = sval(v, "date"), e.status = sval(v, "status");
    e.lengthMs = ival(v, "lengthMs");
    e.progress = int(ival(v, "progress", -1));
    e.played = yyjson_get_bool(yyjson_obj_get(v, "played"));
    e.chapters = decodeChapters(sval(v, "chapters"));
    e.detailed = yyjson_get_bool(yyjson_obj_get(v, "detailed"));
    p.file = sval(v, "file"), p.cover = sval(v, "cover");
    p.agreed = int(ival(v, "agreed", -1)), p.pending = int(ival(v, "pending", -1));
    p.used = ival(v, "used"), p.downloadedAt = ival(v, "downloadedAt");
    if (!e.id.empty()) g_eps[e.id] = std::move(p);
  }
  yyjson_arr_foreach(yyjson_obj_get(r, "upNext"), i, n, v) if (yyjson_is_str(v)) g_upNext.push_back(yyjson_get_str(v));
  yyjson_doc_free(d);
}

// at most 400 kept: the least used go first, never one downloaded, queued or with a mark to send
void trim() {
  if (g_eps.size() <= 400) return;
  std::vector<std::pair<int64_t, std::string>> by;
  for (auto& [id, p] : g_eps)
    if (p.file.empty() && p.pending < 0 && std::find(g_upNext.begin(), g_upNext.end(), id) == g_upNext.end()) by.push_back({p.used, id});
  std::sort(by.begin(), by.end());
  for (size_t k = 0; k < by.size() && g_eps.size() > 400; k++) g_eps.erase(by[k].second);
}

void writeStore() {
  trim();
  std::string j = "{\"episodes\":{";
  int n = 0;
  for (auto& [id, p] : g_eps) {
    const Episode& e = p.e;
    j += std::string(n++ ? "," : "") + jq(id) + ":{\"title\":" + jq(e.title) + ",\"show\":" + jq(e.show) + ",\"showId\":" + jq(e.showId) +
         ",\"thumb\":" + jq(e.thumb) + ",\"date\":" + jq(e.date) + ",\"status\":" + jq(e.status) + ",\"lengthMs\":" + std::to_string(e.lengthMs) +
         ",\"progress\":" + std::to_string(e.progress) + ",\"played\":" + (e.played ? "true" : "false") +
         ",\"chapters\":" + jq(encodeChapters(e.chapters)) + ",\"detailed\":" + (e.detailed ? "true" : "false") + ",\"file\":" + jq(p.file) +
         ",\"cover\":" + jq(p.cover) + ",\"agreed\":" + std::to_string(p.agreed) + ",\"pending\":" + std::to_string(p.pending) +
         ",\"used\":" + std::to_string(p.used) + ",\"downloadedAt\":" + std::to_string(p.downloadedAt) + "}";
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

void changed() { g_dirty = true; }

// what a list or a page says of an episode, into what's kept (its marks are noteEpisodes')
Kept& keep(const Episode& e) {
  readStore();
  Kept& p = g_eps[e.id];
  Episode& k = p.e;
  k.id = e.id;
  if (!e.title.empty()) k.title = e.title;
  if (!e.show.empty()) k.show = e.show;
  if (!e.showId.empty()) k.showId = e.showId;
  if (!e.thumb.empty()) k.thumb = e.thumb;
  if (!e.date.empty()) k.date = e.date;
  if (!e.status.empty()) k.status = e.status;
  if (e.lengthMs > 0) k.lengthMs = e.lengthMs;
  if (!e.description.empty()) k.description = e.description;
  if (e.detailed) k.chapters = e.chapters, k.detailed = true;
  if (e.later >= 0) k.later = e.later;
  p.used = wallNowMs();
  changed();
  return p;
}

Kept* kept(const std::string& id) {
  readStore();
  auto it = g_eps.find(id);
  return it == g_eps.end() ? nullptr : &it->second;
}

Item streamItemOf(const Episode& e) {
  Item it;
  it.kind = "episode", it.id = e.id, it.title = e.title, it.subtitle = e.show, it.thumb = e.thumb;
  it.durationSec = e.lengthMs > 0 ? int(e.lengthMs / 1000) : -1;
  return it;
}

bool signedIn() { return g_prefs.accountMusic; }

// tell YouTube a mark made here (Mark as played / unplayed): kept as pending until it's taken
void push(const std::string& id) {
  Kept* p = kept(id);
  if (!p || p->pending < 0 || !signedIn()) return;
  int mark = p->pending;
  g_core.post("/music/episode/" + Api::escape(id) + "/played", mark ? "{\"played\":true}" : "{\"played\":false}", [id, mark](int status, const std::string&) {
    Kept* p = kept(id);
    if (!p) return;
    if (status == 200) {
      if (p->pending == mark) p->pending = -1;
      p->agreed = mark;
      p->toldAt = wallNowMs();
      p->e.played = mark != 0;
      if (mark) p->e.progress = 100;
      g_changedAt = now_ns();
    } else if (status == 503 || status == 404 || status == 400) {
      p->pending = -1;  // no account, or a server before podcasts: kept on the Y1 only
    } else {
      g_pushAt = now_ns() + 60 * kSec;  // tried again in a minute
    }
    changed();
  });
}

// the queue of episodes: the next one starts (true), the music waits as it did
bool startNext() {
  readStore();
  while (!g_upNext.empty()) {
    std::string id = g_upNext.front();
    g_upNext.erase(g_upNext.begin());
    changed();
    if (Kept* p = kept(id)) {
      playEpisode(p->e);
      return true;
    }
  }
  return false;
}

}  // namespace

bool isEpisodeKey(const std::string& key) { return key.compare(0, 3, "yt:") == 0 && key.size() == 14; }
std::string episodeKey(const std::string& id) { return "yt:" + id; }
std::string episodeIdOf(const std::string& key) { return isEpisodeKey(key) ? key.substr(3) : ""; }
std::string showSpeedKey(const std::string& showId) { return "ytshow:" + showId; }

std::string episodeSpeedKey(const std::string& key) {
  const Kept* p = kept(episodeIdOf(key));
  return p && !p->e.showId.empty() ? showSpeedKey(p->e.showId) : key;
}

int podcastCacheMs() { return g_changedAt && now_ns() - g_changedAt < 20 * kSec ? 0 : 120000; }

bool episodeDetails(const std::string& key, BookPlay& d) {
  Kept* p = kept(episodeIdOf(key));
  if (!p) return false;
  const Episode& e = p->e;
  d = BookPlay();
  d.key = key, d.title = e.title, d.author = e.show;
  d.speedKey = episodeSpeedKey(key);
  std::string path = fileThere(p->file) ? p->file : "/s/" + e.id + "/140";
  if (isStream(path)) rememberStream(path, streamItemOf(e));
  d.paths = {path};
  int64_t len = e.lengthMs;
  if (len <= 0)
    if (const Track* t = g_library.find(path)) len = t->durationSec > 0 ? int64_t(t->durationSec) * 1000 : 0;
  d.fileMs = {len};
  d.lengthMs = len;
  d.chapters = buildChapters(d.fileMs, {e.chapters}, {e.title});
  d.skips = skipsTime(d.chapters);
  if (!d.chapters.empty() && d.skips) d.chapters[0].title = e.title;
  return true;
}

void episodeFinishedChanged(const std::string& key, bool finished) {
  Kept* p = kept(episodeIdOf(key));
  if (!p) return;
  if (p->agreed == int(finished) && p->pending < 0) return;  // YouTube says so already
  p->pending = finished ? 1 : 0;
  changed();
  push(p->e.id);
}

bool episodeEnded(const std::string& key) {
  (void)key;
  return startNext();
}

void episodeRestored(const std::string& key, const std::string& path) {
  if (Kept* p = kept(episodeIdOf(key)))
    if (isStream(path)) rememberStream(path, streamItemOf(p->e));
}

// ---- reading ----

namespace {
Episode episodeFrom(yyjson_val* v) {
  Episode e;
  e.id = sval(v, "id"), e.title = sval(v, "title"), e.show = sval(v, "show"), e.showId = sval(v, "showId"), e.thumb = sval(v, "thumb");
  e.date = sval(v, "date");
  // the row's words: how long, how much is left, Played (YouTube's), else its length's
  e.status = sval(v, "status");
  if (e.status.empty()) e.status = sval(v, "length");
  e.description = sval(v, "description");
  int64_t sec = ival(v, "durationSec");
  if (sec > 0) e.lengthMs = sec * 1000;
  yyjson_val* pr = yyjson_obj_get(v, "progress");
  if (yyjson_is_num(pr)) e.progress = std::max(0, std::min(100, int(yyjson_get_num(pr))));
  e.played = yyjson_get_bool(yyjson_obj_get(v, "played"));
  yyjson_val* l = yyjson_obj_get(v, "later");
  if (yyjson_is_bool(l)) e.later = yyjson_get_bool(l) ? 1 : 0;
  return e;
}
}  // namespace

bool parseEpisode(const std::string& json, Episode& out) {
  yyjson_doc* d = yyjson_read(json.data(), json.size(), 0);
  if (!d) return false;
  yyjson_val* r = yyjson_doc_get_root(d);
  out = episodeFrom(r);
  size_t i, n;
  yyjson_val* c;
  std::vector<std::pair<int64_t, std::string>> starts;
  yyjson_arr_foreach(yyjson_obj_get(r, "chapters"), i, n, c) starts.push_back({ival(c, "ms"), sval(c, "title")});
  for (size_t k = 0; k < starts.size(); k++) {
    FileChapter f;
    f.startMs = starts[k].first;
    f.endMs = k + 1 < starts.size() ? starts[k + 1].first : out.lengthMs;
    f.title = starts[k].second;
    out.chapters.push_back(f);
  }
  out.detailed = true;
  yyjson_doc_free(d);
  return !out.id.empty();
}

void parseEpisodes(const std::string& json, std::vector<Episode>& out, std::vector<Item>* shows, std::string* next) {
  yyjson_doc* d = yyjson_read(json.data(), json.size(), 0);
  if (!d) return;
  yyjson_val* r = yyjson_doc_get_root(d);
  size_t i, n;
  yyjson_val* v;
  yyjson_arr_foreach(yyjson_obj_get(r, "items"), i, n, v) {
    std::string kind = sval(v, "kind");
    if (kind == "episode") {
      Episode e = episodeFrom(v);
      if (e.id.size() == 11 && !e.title.empty()) out.push_back(std::move(e));
    } else if (kind == "podcast" && shows) {
      Item it;
      it.kind = kind, it.id = sval(v, "id"), it.title = sval(v, "title"), it.subtitle = sval(v, "subtitle"), it.thumb = sval(v, "thumb");
      if (!it.id.empty()) shows->push_back(it);
    }
  }
  if (next) *next = sval(r, "next");
  yyjson_doc_free(d);
}

Episode episodeOf(const Item& it) {
  Episode e;
  if (const Kept* p = kept(it.id)) e = p->e;
  e.id = it.id;
  if (!it.title.empty()) e.title = it.title;
  if (!it.thumb.empty()) e.thumb = it.thumb;
  if (e.show.empty()) e.show = it.subtitle.substr(0, it.subtitle.find(" \xc2\xb7 "));
  if (it.durationSec > 0 && e.lengthMs <= 0) e.lengthMs = int64_t(it.durationSec) * 1000;
  return e;
}

void noteEpisodes(const std::vector<Episode>& list) {
  readStore();
  for (const Episode& e : list) {
    auto it = g_eps.find(e.id);
    if (it == g_eps.end()) continue;  // never played, queued or downloaded here: the row shows YouTube's
    Kept& p = it->second;
    if (e.progress >= 0) p.e.progress = e.progress, p.e.played = e.played;
    if (!e.status.empty()) p.e.status = e.status;
    if (e.lengthMs > 0 && p.e.lengthMs <= 0) p.e.lengthMs = e.lengthMs;
    if (e.progress < 0) continue;  // signed out: YouTube says nothing of the account's
    const BookState* st = g_library.bookState(episodeKey(e.id));
    PlayedSync sync{p.agreed, p.pending, p.toldAt};
    PlayedAction a = syncPlayed(sync, st && st->finished, e.played, wallNowMs());
    p.agreed = sync.agreed, p.pending = sync.pending;
    if (a == PlayedAction::AdoptPlayed || a == PlayedAction::AdoptUnplayed) {  // marked on the phone
      BookState s;
      if (st) s = *st;
      s.key = episodeKey(e.id);
      s.finished = a == PlayedAction::AdoptPlayed;
      if (s.finished && !s.lastPlayed) s.lastPlayed = wallNowMs();
      if (!s.finished && p.e.lengthMs > 0 && s.positionMs >= p.e.lengthMs - kFinishedWithinMs) s.positionMs = 0;  // unplayed: from its start
      g_library.setBookState(s);
    } else if (a == PlayedAction::Tell) {
      push(e.id);
    }
    changed();
  }
}

// ---- playing ----

namespace {
void startAt(const Episode& e, int64_t ms, bool fromPlace) {
  Kept& p = keep(e);
  BookPlay d;
  if (!episodeDetails(episodeKey(e.id), d)) return;
  if (fromPlace) playSpokenFrom(d.key, {0, ms});
  else playSpokenDetails(d);
  (void)p;
}

// /music/episode for one: its chapters and length, then `then` (also when it couldn't be asked)
void details(const std::string& id, std::function<void(bool)> then) {
  g_core.get("/music/episode/" + Api::escape(id), podcastCacheMs() ? 600000 : 0, [id, then](int status, const std::string& body) {
    Episode e;
    bool ok = status == 200 && parseEpisode(body, e);
    if (ok) {
      keep(e);
      noteEpisodes({e});
      BookPlay d;
      if (episodeDetails(episodeKey(id), d)) spokenDetailsCame(d);  // the one playing: its chapters now
    }
    if (then) then(ok);
  });
}
}  // namespace

void playEpisode(const Episode& in) {
  Kept& p = keep(in);
  Episode e = p.e;
  const BookState* st = g_library.bookState(episodeKey(e.id));
  bool startedHere = st && st->lastPlayed > 0;
  // started on the phone, never here: from about where YouTube says (its percent of the length)
  int pct = in.progress >= 0 ? in.progress : e.progress;
  if (!startedHere && pct > 0 && pct < 100) {
    auto go = [e, pct]() {
      const Kept* k = kept(e.id);
      int64_t len = k ? k->e.lengthMs : 0;
      if (len <= 0) return startAt(e, 0, false);
      startAt(e, std::max<int64_t>(0, len * pct / 100 - 5000), true);
    };
    if (e.lengthMs > 0) {
      go();
      if (!e.detailed) details(e.id, nullptr);
    } else {
      details(e.id, [go](bool) { go(); });
    }
    return;
  }
  startAt(e, 0, false);
  if (!e.detailed) details(e.id, nullptr);
}

void playEpisodeFrom(const Episode& e, int64_t ms) {
  Kept& p = keep(e);
  startAt(p.e, ms, true);
  if (!p.e.detailed) details(e.id, nullptr);
}

void queueEpisode(const Episode& e, bool next) {
  keep(e);
  g_upNext.erase(std::remove(g_upNext.begin(), g_upNext.end(), e.id), g_upNext.end());
  const BookPlay* now = bookNow();
  PlayerState s = g_player.state();
  bool music = !now && g_player.active() && s.index >= 0 && s.playing && !s.video && !s.fmOn;
  if (!now && !music) {  // nothing plays: at once
    changed();
    playEpisode(e);
    openNowPlaying();
    return;
  }
  if (next) g_upNext.insert(g_upNext.begin(), e.id);
  else g_upNext.push_back(e.id);
  changed();
  if (music && !g_afterSong) g_afterSong = true, g_afterSerial = s.trackSerial;  // after this song
  g_app->toast(now ? (isEpisodeKey(now->key) ? tr("podcasts.afterThisEpisode") : tr("podcasts.afterThisBook"))
                   : tr("podcasts.afterThisSong"));
  if (!kept(e.id)->e.detailed) details(e.id, nullptr);
}

std::vector<Episode> episodesUpNext() {
  readStore();
  std::vector<Episode> v;
  for (const std::string& id : g_upNext)
    if (const Kept* p = kept(id)) v.push_back(p->e);
  return v;
}

void removeFromUpNext(const std::string& id) {
  g_upNext.erase(std::remove(g_upNext.begin(), g_upNext.end(), id), g_upNext.end());
  if (g_upNext.empty()) g_afterSong = false;
  changed();
}

void markEpisodePlayed(const Episode& e, bool played) {
  keep(e);
  markSpokenFinished(episodeKey(e.id), played);  // and episodeFinishedChanged tells YouTube
  Kept* p = kept(e.id);
  if (p && p->pending < 0 && p->agreed != int(played)) p->pending = played, push(e.id);
}

void setEpisodeLater(const Episode& e, bool later, std::function<void(bool)> done) {
  std::string id = e.id;
  g_core.post("/music/episode/" + Api::escape(id) + "/later", later ? "{\"later\":true}" : "{\"later\":false}", [id, later, done](int status, const std::string&) {
    if (status == 200) {
      g_changedAt = now_ns();
      if (Kept* p = kept(id)) p->e.later = later, changed();
    }
    if (done) done(status == 200);
  });
}

void saveShow(const std::string& showId, bool saved, std::function<void(bool)> done) {
  g_core.post("/music/podcast/" + Api::escape(showId) + "/save", saved ? "{\"saved\":true}" : "{\"saved\":false}", [done](int status, const std::string&) {
    if (status == 200) g_changedAt = now_ns();
    if (done) done(status == 200);
  });
}

std::string episodeSubtitle(const Episode& e, bool withShow) {
  std::string state;
  const BookState* st = g_library.bookState(episodeKey(e.id));
  const Kept* p = kept(e.id);
  int64_t len = e.lengthMs > 0 ? e.lengthMs : p ? p->e.lengthMs : 0;
  if (st && st->finished) state = tr("podcasts.played");
  else if (st && st->lastPlayed > 0 && len > 0)
    state = tr("book.left", {{"time", listenTime(std::max<int64_t>(0, len - st->positionMs), spokenSpeed(episodeKey(e.id)))}});
  else if (st && st->lastPlayed > 0) state = tr("podcasts.started");
  else if (e.played) state = tr("podcasts.played");
  else if (!e.status.empty()) state = e.status;  // YouTube's: its length, or what's left of it
  else if (len > 0) state = listenTime(len, 100);
  std::string out;
  for (const std::string& part : {withShow ? e.show : std::string(), e.date, state})
    if (!part.empty()) out += (out.empty() ? "" : " \xc2\xb7 ") + part;
  return out;
}

bool episodeNew(const Episode& e) {
  const BookState* st = g_library.bookState(episodeKey(e.id));
  if (st && (st->lastPlayed > 0 || st->finished)) return false;
  return !e.played && e.progress <= 0;
}

std::vector<Episode> episodesInProgress() {
  readStore();
  std::vector<std::pair<int64_t, Episode>> v;
  for (auto& [id, p] : g_eps) {
    const BookState* st = g_library.bookState(episodeKey(id));
    if (st && st->lastPlayed > 0 && !st->finished) v.push_back({st->lastPlayed, p.e});
  }
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<Episode> out;
  for (auto& [t, e] : v) out.push_back(e);
  return out;
}

// ---- downloads ----

std::string podcastDownloadDir() { return std::string(files::kRoots[0]) + "/Podcasts"; }

bool downloadEpisode(const Episode& e) {
  Kept& p = keep(e);
  // its show's square picture when the show's page was seen (a show's cover suits a file), else its own
  p.cover = e.thumb;
  writeStore();
  Item it;
  it.kind = "episode", it.id = e.id, it.title = e.title, it.subtitle = e.show, it.thumb = e.thumb;
  return downloadEpisodeJob(it);
}

std::string downloadedEpisode(const std::string& id) {
  const Kept* p = kept(id);
  return p && fileThere(p->file) ? p->file : "";
}

std::vector<Episode> downloadedEpisodes() {
  readStore();
  std::vector<std::pair<int64_t, Episode>> v;
  for (auto& [id, p] : g_eps)
    if (fileThere(p.file)) v.push_back({p.downloadedAt, p.e});
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<Episode> out;
  for (auto& [t, e] : v) out.push_back(e);
  return out;
}

bool deleteEpisodeDownload(const std::string& id) {
  Kept* p = kept(id);
  if (!p || p->file.empty()) return false;
  std::string file = p->file;
  // playing from it now: on from the stream, where it is
  if (const BookPlay* b = bookNow())
    if (b->key == episodeKey(id) && !b->paths.empty() && b->paths[0] == file) {
      PlayerState s = g_player.state();
      int64_t at = PlayerHost::positionAt(s, now_ns());
      bool playing = s.playing && !s.ended;
      p->file.clear();
      playSpokenFrom(episodeKey(id), {0, at}, playing);
    }
  bool ok = unlink(file.c_str()) == 0 || !fileThere(file);
  p->file.clear();
  changed();
  writeStore();
  // its show's folder goes when it's empty
  rmdir(file.substr(0, file.rfind('/')).c_str());
  g_library.scanAsync();
  return ok;
}

void episodeDownloaded(const std::string& id, const std::string& file) {
  readStore();
  Kept& p = g_eps[id];
  p.e.id = id;
  p.file = file;
  p.downloadedAt = wallNowMs();
  p.used = p.downloadedAt;
  // the one playing, streamed: it goes on from the card, where it is
  if (const BookPlay* b = bookNow())
    if (b->key == episodeKey(id) && !b->paths.empty() && isStream(b->paths[0])) {
      PlayerState s = g_player.state();
      playSpokenFrom(episodeKey(id), {0, PlayerHost::positionAt(s, now_ns())}, s.playing && !s.ended);
    }
  changed();
  writeStore();
}

bool episodeDownloadInfo(const std::string& id, Episode& out) {
  const Kept* p = kept(id);
  if (!p) return false;
  out = p->e;
  if (!p->cover.empty()) out.thumb = p->cover;
  return true;
}

// ---- the loop ----

bool podcastsTick(int64_t now) {
  readStore();
  bool shown = false;
  PlayerState s = g_player.state();
  // the one playing, its length not known yet: the player's, once it says
  if (const BookPlay* b = bookNow())
    if (isEpisodeKey(b->key) && b->lengthMs <= 0 && s.durationMs > 0)
      if (Kept* p = kept(episodeIdOf(b->key))) {
        p->e.lengthMs = s.durationMs;
        changed();
        BookPlay d;
        if (episodeDetails(b->key, d)) spokenDetailsCame(d), shown = true;
      }
  // Play next / Add to queue while music played: the song is over (the next one began, or the end)
  if (g_afterSong) {
    if (bookNow() || g_upNext.empty()) g_afterSong = false;
    else if (s.trackSerial != g_afterSerial || s.ended) {
      g_afterSong = false;
      if (startNext()) shown = true;
    }
  }
  if (g_pushAt && now >= g_pushAt) {
    g_pushAt = 0;
    for (auto& [id, p] : g_eps)
      if (p.pending >= 0) push(id);
  }
  if (g_dirty && now - g_writtenAt > 2 * kSec) writeStore();
  return shown;
}

void podcastsSaveNow() {
  if (g_dirty) writeStore();
}

// ---- the dashboard's ----

std::string episodeJson(const Episode& e) {
  const BookState* st = g_library.bookState(episodeKey(e.id));
  const Kept* p = kept(e.id);
  int64_t len = e.lengthMs > 0 ? e.lengthMs : p ? p->e.lengthMs : 0;
  std::string s = "{\"kind\":\"episode\",\"id\":" + jq(e.id) + ",\"title\":" + jq(e.title) + ",\"show\":" + jq(e.show) + ",\"showId\":" + jq(e.showId) +
                  ",\"thumb\":" + jq(e.thumb) + ",\"date\":" + jq(e.date) + ",\"status\":" + jq(e.status) + ",\"lengthMs\":" + std::to_string(len) +
                  ",\"subtitle\":" + jq(episodeSubtitle(e, false)) + ",\"new\":" + (episodeNew(e) ? "true" : "false") +
                  ",\"played\":" + ((st && st->finished) || (!st && e.played) ? "true" : "false") +
                  ",\"positionMs\":" + std::to_string(st && st->lastPlayed > 0 && !st->finished ? st->positionMs : 0) +
                  ",\"onCard\":" + (p && fileThere(p->file) ? "true" : "false");
  if (e.later >= 0) s += std::string(",\"later\":") + (e.later ? "true" : "false");
  const BookPlay* b = bookNow();
  s += std::string(",\"playing\":") + (b && b->key == episodeKey(e.id) ? "true" : "false");
  return s + "}";
}

std::string podcastsStateJson() {
  auto list = [](const std::vector<Episode>& v) {
    std::string s = "[";
    for (size_t k = 0; k < v.size(); k++) s += (k ? "," : "") + episodeJson(v[k]);
    return s + "]";
  };
  return "{\"inProgress\":" + list(episodesInProgress()) + ",\"upNext\":" + list(episodesUpNext()) + ",\"downloaded\":" + list(downloadedEpisodes()) +
         ",\"signedIn\":" + (g_prefs.accountMusic ? "true" : "false") + "}";
}

}  // namespace rondo::shell
