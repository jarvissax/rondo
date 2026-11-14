// fill_in.h.
#include "fill_in.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "../core/lyrics.h"
#include "../core/net.h"
#include "../core/tag_write.h"
#include "../core/version.h"
#include "app.h"
#include "main_queue.h"
#include "screens.h"

namespace rondo::shell {

namespace {

// ---------- the web ----------

size_t collect(char* p, size_t s, size_t n, void* out) {
  std::string* b = static_cast<std::string*>(out);
  if (b->size() + s * n > (8u << 20)) return 0;  // a service's answer is never this big
  b->append(p, s * n);
  return s * n;
}
// a GET for core/lookup.h on this thread's own handle; a dropped connection tried again
lookup::Fetch fetcher(CURL* c) {
  return [c](const std::string& url, std::string& body) -> int {
    static const std::string ua = std::string("Rondo/") + kRondoVersion + " ( https://github.com/jarvissax/rondo )";
    for (int attempt = 0; attempt < 3; attempt++) {
      body.clear();
      curl_easy_reset(c);
      curl_easy_setopt(c, CURLOPT_URL, url.c_str());
      curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
      curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
      curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
      curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
      curl_easy_setopt(c, CURLOPT_TIMEOUT, 40L);
      curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
      curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
      net::trust(c, true);  // the iTunes Store's and MusicBrainz's are DigiCert's: Android's roots
      CURLcode rc = curl_easy_perform(c);
      if (rc == CURLE_OK) {
        long st = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
        return int(st);
      }
      if (!net::retryable(rc) && rc != CURLE_OPERATION_TIMEDOUT && rc != CURLE_COULDNT_RESOLVE_HOST) return 0;
      sleep(2);
    }
    return 0;
  };
}

// The iTunes Stores a name points to by its letters, before the page's own and the US one: Turkish
// for ğ ş ı, Azerbaijani (then Turkish) for ə, Russian for Cyrillic, and a few more
std::vector<std::string> storesFor(const std::string& s) {
  std::vector<std::string> out;
  auto has = [&](std::initializer_list<const char*> letters) {
    for (const char* l : letters)
      if (s.find(l) != std::string::npos) return true;
    return false;
  };
  auto range = [&](uint32_t from, uint32_t to) {
    for (size_t i = 0; i < s.size();) {
      unsigned char c = s[i];
      int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
      uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
      for (int k = 1; k < n && i + k < s.size(); k++) cp = cp << 6 | (uint8_t(s[i + k]) & 0x3f);
      if (cp >= from && cp <= to) return true;
      i += n;
    }
    return false;
  };
  if (has({"ə", "Ə"})) out.push_back("az"), out.push_back("tr");
  if (has({"ğ", "Ğ", "ş", "Ş", "ı", "İ"})) out.push_back("tr");
  if (range(0x0400, 0x04ff)) out.push_back("ru");
  if (range(0x0370, 0x03ff)) out.push_back("gr");
  if (range(0x0590, 0x05ff)) out.push_back("il");
  if (range(0x0600, 0x06ff)) out.push_back("ae");
  if (range(0x3040, 0x30ff)) out.push_back("jp");
  if (range(0xac00, 0xd7af)) out.push_back("kr");
  if (has({"ą", "ę", "ł", "ń", "ś", "ź", "ż", "Ł"})) out.push_back("pl");
  if (has({"ř", "ů", "ě", "Ř"})) out.push_back("cz");
  if (has({"ã", "õ"})) out.push_back("br");
  return out;
}

std::string baseOf(const std::string& p) { return p.substr(p.rfind('/') + 1); }
std::string stripSingle(std::string a) {  // the iTunes Store's "Kelebekler - Single" -> "Kelebekler"
  for (const char* tail : {" - Single", " - EP"}) {
    size_t n = strlen(tail);
    if (a.size() > n && a.compare(a.size() - n, n, tail) == 0) a.resize(a.size() - n);
  }
  return a;
}

// ---------- the job ----------

struct Song {  // what a job needs of a library song, copied on the UI thread
  std::string path, title, artist, albumArtist, album, genre;
  int track = 0, disc = 0, year = 0, durationSec = 0;
};
struct Group {
  std::string artist, album, title;  // what's asked: the album, else the song by its title
  std::vector<Song> songs;
};
struct Planned {  // a proposal and the changes it saves, song by song
  FillProposal shown;
  std::vector<std::pair<std::string, TagChanges>> changes;
};

std::mutex g_mu;
FillStatus g_status;
std::vector<Planned> g_planned;
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_active{false}, g_titlesActive{false};

void update(const std::function<void(FillStatus&)>& f) {
  std::lock_guard<std::mutex> l(g_mu);
  f(g_status);
}

bool missing(const Track& t) { return t.genre.empty() || !t.year || t.album.empty() || !t.track; }

std::vector<Song> songsNow(bool all) {  // UI thread
  std::vector<Song> out;
  for (const Track* t : g_library.songs()) {
    if (t->gone || (!all && !missing(*t))) continue;
    out.push_back({t->path, t->title, t->artist, t->albumArtist, t->album, t->genre, t->track, t->disc, t->year, t->durationSec});
  }
  return out;
}

std::vector<Group> groupsOf(const std::vector<Song>& songs) {
  std::map<std::string, size_t> at;
  std::vector<Group> out;
  for (const Song& s : songs) {
    if (!canWriteTags(s.path)) continue;
    std::string artist = !s.albumArtist.empty() ? s.albumArtist : s.artist;
    if (artist.empty()) continue;
    std::string k = s.album.empty() ? "\x1f" + s.path : fold(artist) + "\x1f" + fold(s.album);
    auto f = at.find(k);
    if (f == at.end()) {
      at[k] = out.size();
      out.push_back({artist, s.album, s.title, {}});
      f = at.find(k);
    }
    out[f->second].songs.push_back(s);
  }
  return out;
}

// what a found album changes in a group's songs (only what they lack)
Planned plan(int id, const Group& g, const lookup::Album& a) {
  Planned p;
  p.shown.id = id, p.shown.artist = g.artist, p.shown.album = g.album;
  p.shown.foundArtist = a.artist, p.shown.foundAlbum = stripSingle(a.album), p.shown.source = a.source;
  for (const Song& s : g.songs) {
    TagChanges c;
    if (s.genre.empty() && !a.genre.empty()) c.genre = a.genre, p.shown.genre = a.genre;
    if (!s.year && a.year) c.year = a.year, p.shown.year = a.year;
    if (s.album.empty() && !a.album.empty()) c.album = stripSingle(a.album), p.shown.newAlbum = c.album;
    if (!s.track) {
      std::string want = lookup::matchName(s.title);
      for (const lookup::Album::Track& t : a.tracks)
        if (t.track > 0 && lookup::matchName(t.title) == want) {
          c.track = t.track;
          if (!s.disc && t.disc > 1) c.disc = t.disc;
          p.shown.tracks++;
          break;
        }
    }
    if (c.genre.empty() && !c.year && c.album.empty() && !c.track) continue;
    p.changes.push_back({s.path, c});
    p.shown.songs++;
  }
  return p;
}

void run(std::function<void()> work) {
  auto w = new std::function<void()>(std::move(work));
  pthread_t t;
  pthread_create(&t, nullptr, [](void* p) -> void* {
    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(p));
    (*f)();
    return nullptr;
  }, w);
  pthread_detach(t);
}

// a job starts: "" when it may
std::string begin(const char* kind, int total) {
  if (g_library.scanning()) return "The card is being read: try again in a moment";
  if (organizeStatus().running) return "The card is being organized: try again when it's done";
  std::lock_guard<std::mutex> l(g_mu);
  if (g_status.running) return "Rondo is busy with the last one: wait for it, or cancel it";
  g_status = FillStatus();
  g_status.kind = kind, g_status.running = true, g_status.total = total;
  g_cancel = false, g_active = true;
  return "";
}
void end(const std::string& stopped = "") {
  std::lock_guard<std::mutex> l(g_mu);
  g_status.running = false, g_status.finished = true;
  g_status.canceled = g_cancel.load();
  if (g_status.stopped.empty()) g_status.stopped = stopped;
  g_active = false;
}

// a file saved: its entry in the library takes the new tags (in batches, on the UI thread)
struct Retag {
  std::string path;
  Tags tags;
  int64_t size, modified;
};
void flush(std::vector<Retag>& batch) {
  if (batch.empty()) return;
  auto done = std::make_shared<std::vector<Retag>>(std::move(batch));
  batch.clear();
  postToMain([done] {
    for (const Retag& r : *done) g_library.retagged(r.path, r.tags, r.size, r.modified);
    g_library.saveAsync();
  });
}
void saved(const std::string& path, std::vector<Retag>& batch) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return;
  batch.push_back({path, readTags(path.c_str()), int64_t(st.st_size), int64_t(st.st_mtime) * 1000});
  if (batch.size() >= 25) flush(batch);
}

// a failure in a row of them: the internet's gone, the job stops (one found resets it)
constexpr int kFailuresToStop = 8;

}  // namespace

FillSummary fillInSummary() {
  FillSummary s;
  std::vector<Song> all = songsNow(true);
  s.songs = int(all.size());
  std::vector<Song> lacking;
  for (const Song& x : all) {
    if (!canWriteTags(x.path)) {
      s.unsupported++;
      continue;
    }
    s.noGenre += x.genre.empty(), s.noYear += !x.year, s.noAlbum += x.album.empty(), s.noTrack += !x.track;
    if (x.genre.empty() || !x.year || x.album.empty() || !x.track) lacking.push_back(x);
  }
  s.groups = int(groupsOf(lacking).size());
  return s;
}

FillStatus fillInStatus() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_status;
}

bool fillInActive() { return g_active.load() || g_titlesActive.load(); }

bool fillInWriting() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_status.running && (g_status.kind == "save" || g_status.kind == "lyrics");
}

void fillInCancel() { g_cancel = true; }

std::string fillInDetails(const std::vector<std::string>& countries) {
  auto groups = std::make_shared<std::vector<Group>>(groupsOf(songsNow(false)));
  if (groups->empty()) return "Every song has its genre, year, album and track number";
  std::string why = begin("details", int(groups->size()));
  if (!why.empty()) return why;
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_planned.clear();
  }
  run([groups, countries] {
    CURL* c = curl_easy_init();
    lookup::Fetch get = fetcher(c);
    int inRow = 0, n = 0;
    for (const Group& g : *groups) {
      if (g_cancel) break;
      std::vector<std::string> stores = storesFor(g.artist + " " + g.album + " " + g.title);
      for (const std::string& x : countries) stores.push_back(x);
      stores.push_back("us");
      lookup::Album a;
      bool needTracks = !g.album.empty() && std::any_of(g.songs.begin(), g.songs.end(), [](const Song& s) { return !s.track; });
      lookup::Result r = lookup::findAlbum(get, g.artist, g.album, g.title, stores, needTracks, a, g.songs.size() == 1);
      Planned p;
      if (r == lookup::Result::Found) p = plan(++n, g, a);
      inRow = r == lookup::Result::Failed ? inRow + 1 : 0;
      update([&](FillStatus& s) {
        s.done++;
        if (r == lookup::Result::Failed) s.failed++;
        else if (r == lookup::Result::NotFound || p.changes.empty()) s.notFound++;
        else s.proposals.push_back(p.shown);
      });
      if (!p.changes.empty()) {
        std::lock_guard<std::mutex> l(g_mu);
        g_planned.push_back(std::move(p));
      }
      if (inRow >= kFailuresToStop) {
        end("The services didn't answer: check that your Rondo is online, then try again");
        curl_easy_cleanup(c);
        return;
      }
    }
    curl_easy_cleanup(c);
    end();
  });
  return "";
}

std::string fillInSave(const std::set<int>& skip) {
  auto work = std::make_shared<std::vector<std::pair<std::string, TagChanges>>>();
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_status.running) return "Rondo is busy with the last one: wait for it, or cancel it";
    if (g_status.kind != "details" || !g_status.finished) return "Look the details up first";
    for (const Planned& p : g_planned)
      if (!skip.count(p.shown.id)) work->insert(work->end(), p.changes.begin(), p.changes.end());
  }
  if (work->empty()) return "Nothing to save";
  std::string why = begin("save", int(work->size()));
  if (!why.empty()) return why;
  run([work] {
    std::vector<Retag> batch;
    for (const auto& w : *work) {
      if (g_cancel) break;
      std::string err = writeTags(w.first, w.second);
      if (err.empty()) saved(w.first, batch);
      update([&](FillStatus& s) {
        s.done++;
        if (err.empty()) s.saved++;
        else if (s.errors.size() < 200) s.errors.push_back(baseOf(w.first) + ": " + err);
      });
    }
    flush(batch);
    sync();
    end();
  });
  return "";
}

std::string fillInLyrics() {
  auto songs = std::make_shared<std::vector<Song>>(songsNow(true));
  if (songs->empty()) return "There are no songs on the card";
  std::string why = begin("lyrics", int(songs->size()));
  if (!why.empty()) return why;
  run([songs] {
    CURL* c = curl_easy_init();
    lookup::Fetch get = fetcher(c);
    std::vector<Retag> batch;
    int inRow = 0;
    for (const Song& s : *songs) {
      if (g_cancel) break;
      Lyrics have = lyricsForFile(s.path);  // an .lrc beside it, or lyrics in its tags
      if (!have.synced.empty() || !have.plain.empty()) {
        update([](FillStatus& st) { st.done++, st.had++; });
        continue;
      }
      std::string artist = !s.artist.empty() ? s.artist : s.albumArtist;
      lookup::Lyrics l;
      lookup::Result r = lookup::findLyrics(get, artist, s.title, s.album, s.durationSec, l);
      inRow = r == lookup::Result::Failed ? inRow + 1 : 0;
      std::string err;
      bool beside = false;
      if (r == lookup::Result::Found && !l.text.empty()) {
        if (canWriteTags(s.path)) {
          TagChanges ch;
          ch.lyrics = l.text;
          err = writeTags(s.path, ch);
          if (err.empty()) saved(s.path, batch);
        } else {  // a kind that can't hold them: an .lrc beside it, which Rondo reads as well
          size_t dot = s.path.rfind('.');
          std::string lrc = (dot == std::string::npos ? s.path : s.path.substr(0, dot)) + ".lrc";
          if (FILE* f = fopen(lrc.c_str(), "wb")) {
            bool ok = fwrite(l.text.data(), 1, l.text.size(), f) == l.text.size();
            if (fclose(f) != 0 || !ok) err = "couldn't write its .lrc";
            beside = err.empty();
          } else {
            err = "couldn't write its .lrc";
          }
        }
      }
      update([&](FillStatus& st) {
        st.done++;
        if (r == lookup::Result::Failed) st.failed++;
        else if (r == lookup::Result::NotFound) st.notFound++;
        else if (l.instrumental) st.instrumental++;
        else if (!err.empty()) {
          if (st.errors.size() < 200) st.errors.push_back(baseOf(s.path) + ": " + err);
        } else {
          st.found++, st.saved++;
          if (l.synced) st.synced++;
          if (beside) st.beside++;
        }
      });
      if (inRow >= kFailuresToStop) {
        flush(batch);
        end("LRCLIB didn't answer: check that your Rondo is online, then try again");
        curl_easy_cleanup(c);
        return;
      }
    }
    flush(batch);
    sync();
    curl_easy_cleanup(c);
    end();
  });
  return "";
}

// ---------- Organize's lookup ----------

namespace {
std::mutex g_tmu;
TitlesStatus g_titles;

// a series whose years may be out of date or unknown, a movie without its year
bool needsLookUp(const VideoTitle& t) {
  if (!t.series) return t.years.empty();
  return t.years.size() != 9;  // "2010-2022" is final; "", "2016" and "2023-" aren't
}
}  // namespace

std::vector<VideoTitle> titlesToLookUp() {
  std::vector<VideoTitle> out;
  for (const std::string& root : organizeRoots())
    for (VideoTitle& t : videoTitles(organizeItems(root), root))
      if (needsLookUp(t)) out.push_back(std::move(t));
  return out;
}

TitlesStatus titlesStatus() {
  std::lock_guard<std::mutex> l(g_tmu);
  return g_titles;
}

std::string titlesLookupStart() {
  auto titles = std::make_shared<std::vector<VideoTitle>>(titlesToLookUp());
  if (titles->empty()) return "Every show and movie has its years";
  {
    std::lock_guard<std::mutex> l(g_tmu);
    if (g_titles.running) return "";  // looking already: its result comes
    g_titles = TitlesStatus();
    g_titles.running = true, g_titles.total = int(titles->size());
  }
  g_titlesActive = true;
  run([titles] {
    CURL* c = curl_easy_init();
    lookup::Fetch get = fetcher(c);
    for (const VideoTitle& t : *titles) {
      TitleMatch m;
      m.title = t;
      lookup::Result r = t.series ? lookup::findSeries(get, t.name, m.found) : lookup::findMovie(get, t.name, m.found);
      m.failed = r == lookup::Result::Failed;
      std::string want = lookup::matchName(t.name);
      for (size_t i = 0; i < m.found.size() && m.pick < 0; i++)
        if (lookup::matchName(m.found[i].name) == want) m.pick = int(i);
      std::lock_guard<std::mutex> l(g_tmu);
      g_titles.done++;
      g_titles.matches.push_back(std::move(m));
    }
    curl_easy_cleanup(c);
    std::lock_guard<std::mutex> l(g_tmu);
    g_titles.running = false, g_titles.finished = true;
    g_titlesActive = false;
  });
  return "";
}

}  // namespace rondo::shell
