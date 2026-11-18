// Downloads; see downloads.h.
#include "downloads.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <thread>

#include <yyjson.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include "../core/lyrics.h"
#include "../core/organize.h"
#include "../core/tag_write.h"
#include "app.h"
#include "file_ops.h"
#include "fill_in.h"
#include "screens.h"

namespace rondo::shell {

std::string downloadDir() { return std::string(files::kRoots[0]) + "/Videos/YouTube"; }
std::string musicDownloadDir() { return std::string(files::kRoots[0]) + "/Music/YouTube Music"; }

bool isMixId(const std::string& id) { return id.compare(0, 2, "RD") == 0 && id.compare(0, 6, "RDCLAK") != 0; }

namespace {

std::vector<DownloadJob> g_jobs;
int g_at = -1;  // the job being done
enum Step { Idle, Resolving, Picture, Sound, Captions, Cover, Words, Joining } g_step = Idle;
bool g_coreBusy = false;  // the core's one download is this job's (or a cancelled one, still ending)
int32_t g_seq = 0;        // the core's downloadSeq before this part began: its state counts once past it
int g_videoItag = 0, g_audioItag = 0;
int64_t g_videoBytes = 0, g_audioBytes = 0;
std::vector<std::pair<std::string, std::string>> g_subs;  // the subtitle files to write: name part ("en", "az.auto"), WebVTT
int g_subsPending = 0;
uint32_t g_generation = 0;  // a reply for a job cancelled meanwhile is dropped
std::atomic<int> g_joined{0};  // 0 joining, 1 done, 2 failed, 3 stopped (the card went to the computer: done again later)
std::atomic<bool> g_joining{false};  // the joiner's thread has files open on the card
std::atomic<bool> g_joinStop{false};  // the card is going to the computer: the joiner stops at once
std::thread g_joiner;
bool g_changed = false;
uint32_t g_serial = 0;  // downloadsSerial(): one more each time g_changed is handed out
bool g_queueDirty = false;  // the queue to write again (download-queue.json)
bool g_queueRead = false;

// what holds the queue: the card at the computer (set from the USB switch's thread), a connection that
// failed (tried again from g_retryAt, waiting longer each time), the card full (looked at again from g_fullUntil)
std::atomic<bool> g_cardAway{false};
std::string g_held;  // downloadsHeld(): why nothing starts now
int64_t g_retryAt = 0, g_fullUntil = 0, g_heldAt = 0;
int64_t g_offlineSince = 0, g_onlineAt = 0;  // a download running with no network since (0: online), looked at when
int g_misses = 0;  // connection failures in a row

// each part on the card (a paused, waiting or running download's): what of it the card has for sure,
// by the part's file (download-part beside prefs.json, a line each)
struct Mark {
  std::string id, file;
  int64_t synced = 0, total = 0;
};
std::map<std::string, Mark> g_marks;
bool g_markRead = false;
std::string g_markFile;  // the part the core is fetching now
int32_t g_markSeq = -1;  // that download (the core's downloadSeq), -1 none

// what's on the card: id -> file, read again when the folder changes
std::map<std::string, std::string> g_files;
int64_t g_filesAt = -1;  // the folder's mtime when read (ns)

std::string dataDir() { return g_prefsPath.substr(0, g_prefsPath.rfind('/')); }
std::string metaPath() { return dataDir() + "/downloads.json"; }
std::string queuePath() { return dataDir() + "/download-queue.json"; }
std::string markPath() { return dataDir() + "/download-part"; }

struct Meta {
  std::string title, channel;
  int quality = 0;
  int64_t at = 0;
};
std::map<std::string, Meta> g_meta;
bool g_metaRead = false;

// the downloaded songs: id -> where it went (music-downloads.json beside prefs.json)
struct SongMeta {
  std::string file, title, artist, album;
  int64_t at = 0;
  bool alone = true;  // downloaded by itself (false: only for playlists)
};
std::map<std::string, SongMeta> g_songs;
bool g_songsRead = false;
std::string songsPath() { return dataDir() + "/music-downloads.json"; }
// the downloaded playlists (its "playlists"): YouTube id -> its file's name and its songs in order
struct ListRec {
  std::string title, name;
  std::string thumb;  // its picture's address (the server's or direct mode's /thumb), for its cover
  bool mix = false;
  std::vector<std::string> ids;
  int64_t at = 0;
};
std::map<std::string, ListRec> g_lists;
// a playlist whose songs are being asked for, page by page
struct ListLoad {
  std::string title, thumb;  // thumb: its picture (the server's or direct mode's /thumb)
  bool mix = false;
  std::vector<Item> items;
  int64_t at = 0;
};
std::map<std::string, ListLoad> g_loads;
// the song being downloaded: what /music/song said, else what its row did
struct SongInfo {
  std::string title, artist, albumArtist, album, cover;
  int year = 0, track = 0, tracks = 0, seconds = -1;
};
SongInfo g_song;
std::string g_words;  // its lyrics: LRC when timed

std::string jstr(const std::string& v) {
  std::string o = "\"";
  for (unsigned char c : v) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else o += char(c);
  }
  return o + "\"";
}

std::string sval(yyjson_val* o, const char* f) {
  yyjson_val* x = yyjson_obj_get(o, f);
  return yyjson_is_str(x) ? std::string(yyjson_get_str(x)) : std::string();
}

// a small file written whole or not at all
bool writeWhole(const std::string& path, const std::string& text) {
  std::string tmp = path + ".part";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (ok && rename(tmp.c_str(), path.c_str()) == 0) return true;
  unlink(tmp.c_str());
  return false;
}

void readMeta() {
  if (g_metaRead) return;
  g_metaRead = true;
  yyjson_doc* d = yyjson_read_file(metaPath().c_str(), 0, nullptr, nullptr);
  if (!d) return;
  yyjson_val* root = yyjson_doc_get_root(d);
  size_t i, n;
  yyjson_val *k, *v;
  yyjson_obj_foreach(root, i, n, k, v) {
    Meta m;
    m.title = sval(v, "title"), m.channel = sval(v, "channel");
    m.quality = int(yyjson_get_int(yyjson_obj_get(v, "quality")));
    m.at = yyjson_get_sint(yyjson_obj_get(v, "at"));
    g_meta[yyjson_get_str(k)] = m;
  }
  yyjson_doc_free(d);
}

void writeMeta() {
  std::string j = "{";
  int n = 0;
  for (auto& [id, m] : g_meta)
    j += std::string(n++ ? "," : "") + jstr(id) + ":{\"title\":" + jstr(m.title) + ",\"channel\":" + jstr(m.channel) +
         ",\"quality\":" + std::to_string(m.quality) + ",\"at\":" + std::to_string(m.at) + "}";
  writeWhole(metaPath(), j + "}\n");
}

void mkdirs(const std::string& dir) {
  for (size_t at = 1; at <= dir.size(); at++)
    if (at == dir.size() || dir[at] == '/') mkdir(dir.substr(0, at).c_str(), 0775);
}

// a title as a file name: no / \ : * ? " < > |, no dot or space at the ends, 80 bytes at most (whole characters)
std::string fileTitle(const std::string& title) {
  std::string t;
  for (char c : title) t += strchr("/\\:*?\"<>|", c) || (unsigned char)c < 0x20 ? ' ' : c;
  std::string out;
  for (char c : t)
    if (!(c == ' ' && (out.empty() || out.back() == ' '))) out += c;
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  while (!out.empty() && (out[0] == '.' || out[0] == ' ')) out.erase(0, 1);
  if (out.size() > 80) {
    size_t cut = 80;
    while (cut > 0 && (uint8_t(out[cut]) & 0xc0) == 0x80) cut--;
    out = out.substr(0, cut);
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  }
  return out.empty() ? "Video" : out;
}

std::string partsBase(const std::string& id) { return downloadDir() + "/." + id; }  // hidden: the scan skips them
std::string songParts(const std::string& id) { return musicDownloadDir() + "/." + id; }

void readMarks() {
  if (g_markRead) return;
  g_markRead = true;
  // "<id> <synced> <total> <file>"
  if (FILE* f = fopen(markPath().c_str(), "r")) {
    char id[64] = "", line[1024] = "";
    long long synced = 0, total = 0;
    while (fgets(line, sizeof(line), f))
      if (sscanf(line, "%63s %lld %lld", id, &synced, &total) == 3) {
        const char* p = line;
        for (int sp = 0; *p && sp < 3; p++)
          if (*p == ' ') sp++;
        std::string file = p;
        while (!file.empty() && (file.back() == '\n' || file.back() == '\r')) file.pop_back();
        if (!file.empty()) g_marks[file] = {id, file, synced, total};
      }
    fclose(f);
  }
}
void writeMarks() {
  if (g_marks.empty()) {
    unlink(markPath().c_str());
    return;
  }
  std::string t;
  for (auto& [file, m] : g_marks) t += m.id + " " + std::to_string(m.synced) + " " + std::to_string(m.total) + " " + m.file + "\n";
  writeWhole(markPath(), t);
}
// a download's marks go (done, cancelled, failed: its parts are gone)
void clearMarks(const std::string& id) {
  readMarks();
  bool any = false;
  for (auto it = g_marks.begin(); it != g_marks.end();)
    if (it->second.id == id) {
      if (it->first == g_markFile) g_markFile.clear(), g_markSeq = -1;
      it = g_marks.erase(it), any = true;
    } else {
      ++it;
    }
  if (any) writeMarks();
}

void removeParts(const std::string& id) {
  for (const char* x : {".video", ".video.part", ".audio", ".audio.part", ".mp4.part"}) unlink((partsBase(id) + x).c_str());
  for (const char* x : {".audio", ".audio.part", ".cover", ".cover.part", ".saving.m4a"}) unlink((songParts(id) + x).c_str());
  clearMarks(id);
}

void readSongs() {
  if (g_songsRead) return;
  g_songsRead = true;
  yyjson_doc* d = yyjson_read_file(songsPath().c_str(), 0, nullptr, nullptr);
  if (!d) return;
  size_t i, n;
  yyjson_val *k, *v;
  yyjson_obj_foreach(yyjson_doc_get_root(d), i, n, k, v) {
    if (!strcmp(yyjson_get_str(k), "playlists")) {  // the downloaded playlists
      size_t pi, pn;
      yyjson_val *pk, *pv;
      yyjson_obj_foreach(v, pi, pn, pk, pv) {
        ListRec r;
        r.title = sval(pv, "title"), r.name = sval(pv, "name"), r.thumb = sval(pv, "thumb");
        r.mix = yyjson_get_bool(yyjson_obj_get(pv, "mix"));
        r.at = yyjson_get_sint(yyjson_obj_get(pv, "at"));
        size_t ii, in;
        yyjson_val* idv;
        yyjson_arr_foreach(yyjson_obj_get(pv, "ids"), ii, in, idv) if (yyjson_is_str(idv)) r.ids.push_back(yyjson_get_str(idv));
        if (!r.name.empty()) g_lists[yyjson_get_str(pk)] = r;
      }
      continue;
    }
    SongMeta m;
    m.file = sval(v, "file"), m.title = sval(v, "title"), m.artist = sval(v, "artist"), m.album = sval(v, "album");
    m.at = yyjson_get_sint(yyjson_obj_get(v, "at"));
    yyjson_val* alone = yyjson_obj_get(v, "alone");
    m.alone = !yyjson_is_bool(alone) || yyjson_get_bool(alone);  // before playlists, every one was
    if (!m.file.empty()) g_songs[yyjson_get_str(k)] = m;
  }
  yyjson_doc_free(d);
}
void writeSongs() {
  std::string j = "{";
  int n = 0;
  for (auto& [id, m] : g_songs)
    j += std::string(n++ ? "," : "") + jstr(id) + ":{\"file\":" + jstr(m.file) + ",\"title\":" + jstr(m.title) + ",\"artist\":" + jstr(m.artist) +
         ",\"album\":" + jstr(m.album) + ",\"at\":" + std::to_string(m.at) + ",\"alone\":" + (m.alone ? "true" : "false") + "}";
  j += std::string(n ? "," : "") + "\"playlists\":{";
  int p = 0;
  for (auto& [id, r] : g_lists) {
    std::string ids;
    for (const std::string& s : r.ids) ids += (ids.empty() ? "" : ",") + jstr(s);
    j += std::string(p++ ? "," : "") + jstr(id) + ":{\"title\":" + jstr(r.title) + ",\"name\":" + jstr(r.name) + ",\"thumb\":" + jstr(r.thumb) +
         ",\"mix\":" + (r.mix ? "true" : "false") +
         ",\"at\":" + std::to_string(r.at) + ",\"ids\":[" + ids + "]}";
  }
  writeWhole(songsPath(), j + "}}\n");
}
bool fileThere(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
int64_t fileSize(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? int64_t(st.st_size) : -1;
}

// ---- the queue, kept ----

const char* stateName(DownloadJob::State s) {
  return s == DownloadJob::Waiting ? "waiting" : s == DownloadJob::Running ? "running" : s == DownloadJob::Paused ? "paused" : s == DownloadJob::Done ? "done" : "failed";
}

void saveQueue() {
  g_queueDirty = false;
  std::string j = "{\"jobs\":[";
  int n = 0;
  for (const DownloadJob& d : g_jobs) {
    if (d.state == DownloadJob::Done || (d.state == DownloadJob::Failed && d.error == "Cancelled")) continue;
    // a running one is waiting when read again
    const char* st = d.state == DownloadJob::Running ? "waiting" : stateName(d.state);
    j += std::string(n++ ? "," : "") + "{\"id\":" + jstr(d.id) + ",\"song\":" + (d.song ? "true" : "false") + ",\"alone\":" + (d.alone ? "true" : "false") +
         ",\"title\":" + jstr(d.title) + ",\"channel\":" + jstr(d.channel) + ",\"thumb\":" + jstr(d.thumb) + ",\"quality\":" + std::to_string(d.quality) +
         ",\"state\":\"" + st + "\",\"percent\":" + std::to_string(d.percent) + ",\"error\":" + jstr(d.error) + "}";
  }
  writeWhole(queuePath(), j + "]}\n");
}

void readQueue() {
  if (g_queueRead) return;
  g_queueRead = true;
  yyjson_doc* d = yyjson_read_file(queuePath().c_str(), 0, nullptr, nullptr);
  if (!d) return;
  size_t i, n;
  yyjson_val* v;
  yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(d), "jobs"), i, n, v) {
    DownloadJob j;
    j.id = sval(v, "id"), j.title = sval(v, "title"), j.channel = sval(v, "channel"), j.thumb = sval(v, "thumb"), j.error = sval(v, "error");
    j.song = yyjson_get_bool(yyjson_obj_get(v, "song"));
    j.alone = yyjson_get_bool(yyjson_obj_get(v, "alone"));
    j.quality = int(yyjson_get_int(yyjson_obj_get(v, "quality")));
    j.percent = int(yyjson_get_int(yyjson_obj_get(v, "percent")));
    std::string st = sval(v, "state");
    j.state = st == "paused" ? DownloadJob::Paused : st == "failed" ? DownloadJob::Failed : DownloadJob::Waiting;
    if (j.id.empty()) continue;
    bool dup = false;
    for (const DownloadJob& o : g_jobs) dup = dup || o.id == j.id;
    if (!dup) g_jobs.push_back(j);
  }
  yyjson_doc_free(d);
}

void ensureRead() {
  readQueue();
  readMarks();
}

bool going(const DownloadJob& j) { return j.state == DownloadJob::Waiting || j.state == DownloadJob::Running || j.state == DownloadJob::Paused; }

void refreshFiles(bool force = false) {
  struct stat st;
  int64_t m = stat(downloadDir().c_str(), &st) == 0 ? int64_t(st.st_mtime) * 1000000000LL + st.st_mtim.tv_nsec : -2;
  if (!force && m == g_filesAt) return;
  g_filesAt = m;
  g_files.clear();
  DIR* d = opendir(downloadDir().c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (name[0] == '.' || name.size() < 18 || name.compare(name.size() - 4, 4, ".mp4") != 0) continue;
    std::string id = downloadIdOf(name);
    if (!id.empty()) g_files[id] = downloadDir() + "/" + name;
  }
  closedir(d);
}

DownloadJob* current() { return g_at >= 0 && g_at < int(g_jobs.size()) ? &g_jobs[size_t(g_at)] : nullptr; }

// ---- what holds the queue ----

bool cardMounted() {
  FILE* f = fopen("/proc/mounts", "r");
  if (!f) return false;
  char line[512], dev[256], at[256];
  bool found = false;
  while (!found && fgets(line, sizeof(line), f))
    if (sscanf(line, "%255s %255s", dev, at) == 2 && !strcmp(at, files::kRoots[0])) found = true;
  fclose(f);
  return found;
}
// a default route: Wi-Fi up (main.cpp's readOnline)
bool online() {
  FILE* f = fopen("/proc/net/route", "r");
  if (!f) return false;
  char line[256];
  bool on = false;
  while (fgets(line, sizeof(line), f)) {
    char iface[32];
    unsigned dest = 1;
    if (sscanf(line, "%31s %x", iface, &dest) == 2 && dest == 0) on = true;
  }
  fclose(f);
  return on;
}
int64_t g_onlineSince = 0;  // Wi-Fi back since (0: not): a moment for it to settle before asking anything
std::string holdNow() {
  if (g_cardAway) return "The card is on the computer";
  if (!cardMounted()) return "Waiting for the card";
  if (!online()) {
    g_onlineSince = 0;
    return "Waiting for Wi-Fi";
  }
  if (!g_onlineSince) g_onlineSince = now_ns();
  if (now_ns() - g_onlineSince < 2 * kSec) return "Waiting for Wi-Fi";
  if (now_ns() < g_retryAt) return "Waiting to try again";
  if (now_ns() < g_fullUntil) return "The card is full";
  return "";
}

// the job running goes back to `to` (Waiting: held; Paused: the user's), its part kept
void stopRunning(DownloadJob::State to) {
  DownloadJob* j = current();
  if (!j || g_step == Joining) return;  // being saved: it finishes in a moment
  if (g_coreBusy) g_core.cancelDownload();  // the part stays (its last synced size noted as it ends)
  j->state = to, j->stage = "";
  g_at = -1, g_step = Idle, g_subs.clear(), g_subsPending = 0;
  g_generation++;
  g_changed = g_queueDirty = true;
}

// no connection (or the server not answering): waiting, tried again after 5 s, 15, 30, 1, 2, 4, then 5 minutes
void retryLater() {
  g_misses++;
  g_retryAt = now_ns() + (g_misses == 1 ? 5 : std::min<int64_t>(300, int64_t(15) << std::min(g_misses - 2, 5))) * kSec;
  stopRunning(DownloadJob::Waiting);
}

void finish(DownloadJob::State state, const std::string& error = "") {
  DownloadJob* j = current();
  if (!j) return;
  j->state = state, j->error = error, j->stage = "";
  if (state == DownloadJob::Failed) {
    removeParts(j->id);
    g_app->toast("Download failed: " + j->title);
  }
  if (state == DownloadJob::Done) clearMarks(j->id), g_misses = 0;
  g_at = -1, g_step = Idle, g_subs.clear(), g_subsPending = 0;
  g_generation++;
  g_changed = g_queueDirty = true;
}

// Picture and sound into one .mp4 as they are (no re-encoding), its index at the front so it starts
// at once; the parts are what /s/ served (YouTube's fragmented MP4: H.264 and AAC). Tagged with its
// title and channel, so the library doesn't guess them from the file's name ("Title - Part" read as
// an artist and a title).
bool join(const std::string& video, const std::string& audio, const std::string& out, const std::string& title,
          const std::string& channel, const std::string& id, bool song = false) {
  AVFormatContext* in[2] = {nullptr, nullptr};
  AVFormatContext* o = nullptr;
  bool ok = false;
  int map[2][16];
  int64_t last[2] = {INT64_MIN, INT64_MIN};
  bool more[2] = {true, true};
  int inputs = audio.empty() ? 1 : 2;
  AVPacket* pkt = av_packet_alloc();
  do {
    if (avformat_open_input(&in[0], video.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(in[0], nullptr) < 0) break;
    if (inputs == 2 && (avformat_open_input(&in[1], audio.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(in[1], nullptr) < 0)) break;
    if (avformat_alloc_output_context2(&o, nullptr, "mp4", out.c_str()) < 0 || !o) break;
    bool bad = false;
    for (int k = 0; k < inputs && !bad; k++) {
      for (unsigned i = 0; i < in[k]->nb_streams && i < 16; i++) {
        AVCodecParameters* cp = in[k]->streams[i]->codecpar;
        bool keep = inputs == 1 ? (cp->codec_type == AVMEDIA_TYPE_VIDEO || cp->codec_type == AVMEDIA_TYPE_AUDIO)
                                : cp->codec_type == (k == 0 ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO);
        map[k][i] = -1;
        if (!keep) continue;
        AVStream* s = avformat_new_stream(o, nullptr);
        if (!s || avcodec_parameters_copy(s->codecpar, cp) < 0) {
          bad = true;
          break;
        }
        s->codecpar->codec_tag = 0;
        s->time_base = in[k]->streams[i]->time_base;
        map[k][i] = s->index;
      }
    }
    if (bad || o->nb_streams == 0) break;
    if (!song) {  // a song's tags are writeTags' (its cover too), once it's whole
      av_dict_set(&o->metadata, "title", title.c_str(), 0);
      if (!channel.empty()) av_dict_set(&o->metadata, "artist", channel.c_str(), 0);
      av_dict_set(&o->metadata, "comment", ("https://www.youtube.com/watch?v=" + id).c_str(), 0);
    }
    if (avio_open(&o->pb, out.c_str(), AVIO_FLAG_WRITE) < 0) break;
    AVDictionary* opts = nullptr;
    // a video: its index at the front, so it starts at once; a song's stays at the end, where its tags
    // then grow without the file being written again
    if (!song) av_dict_set(&opts, "movflags", "+faststart", 0);
    int hr = avformat_write_header(o, &opts);
    av_dict_free(&opts);
    if (hr < 0) break;
    bool failed = false;
    for (int k = 0; k < inputs; k++) more[k] = true;
    while (!failed) {
      if (g_joinStop) {  // the card is going to the computer
        failed = true;
        break;
      }
      // the input behind in time goes next, so the two interleave
      int k = -1;
      for (int i = 0; i < inputs; i++)
        if (more[i] && (k < 0 || last[i] < last[k])) k = i;
      if (k < 0) break;
      if (av_read_frame(in[k], pkt) < 0) {
        more[k] = false;
        continue;
      }
      int si = pkt->stream_index;
      int target = si < 16 ? map[k][si] : -1;
      if (target < 0) {
        av_packet_unref(pkt);
        continue;
      }
      AVRational from = in[k]->streams[si]->time_base;
      if (pkt->dts != AV_NOPTS_VALUE) last[k] = av_rescale_q(pkt->dts, from, AVRational{1, 1000000});
      av_packet_rescale_ts(pkt, from, o->streams[target]->time_base);
      pkt->stream_index = target;
      pkt->pos = -1;
      if (av_interleaved_write_frame(o, pkt) < 0) failed = true;
      av_packet_unref(pkt);
    }
    if (failed) break;
    ok = av_write_trailer(o) == 0;
  } while (false);
  av_packet_free(&pkt);
  for (AVFormatContext*& c : in)
    if (c) avformat_close_input(&c);
  if (o) {
    if (o->pb) avio_closep(&o->pb);
    avformat_free_context(o);
  }
  return ok;
}

void startPart(Step step);
void startCover();

// a download's parts on the card, and room to save it: putting it together writes it again whole (and
// 8 MB more); without that it waits, its parts kept, looked at again in a minute
int64_t partsSize(const DownloadJob& j) {
  int64_t n = 0;
  for (const char* x : {".video", ".audio"}) n += std::max<int64_t>(0, fileSize((j.song ? songParts(j.id) : partsBase(j.id)) + x));
  return n;
}
bool roomLeft() {  // a save that failed: for lack of room?
  int64_t room = files::freeBytes(files::kRoots[0]);
  return room < 0 || room >= (32 << 20);
}
bool roomToSave() {
  DownloadJob* j = current();
  int64_t room = files::freeBytes(files::kRoots[0]);
  if (!j || room < 0 || room >= partsSize(*j) + (8 << 20)) return true;
  g_fullUntil = now_ns() + 60 * kSec;
  g_app->toast("The card is full: downloads wait");
  stopRunning(DownloadJob::Waiting);
  return false;
}

// the next step's request (each step's reply or download leads to the next)
void startCaptions() {
  DownloadJob* j = current();
  g_step = Captions;
  j->stage = "Getting subtitles";
  g_changed = true;
  uint32_t gen = g_generation;
  std::string id = j->id;
  g_core.get("/captions/" + Api::escape(id), 0, [gen, id](int status, const std::string& body) {
    if (gen != g_generation) return;
    std::vector<CaptionTrack> tracks;
    if (status == 200) parseCaptions(body, tracks);
    // the video's own tracks (ten at most) and the speech recognition's
    std::vector<CaptionTrack> keep;
    for (const CaptionTrack& t : tracks)
      if (!t.autoGen && keep.size() < 10) keep.push_back(t);
    for (const CaptionTrack& t : tracks)
      if (t.autoGen) keep.push_back(t);
    g_subsPending = int(keep.size());
    auto joinNow = [] {
      if (g_cardAway) return stopRunning(DownloadJob::Waiting);
      if (!roomToSave()) return;
      DownloadJob* j = current();
      g_step = Joining;
      j->stage = "Saving";
      g_changed = true;
      g_joined = 0;
      std::string base = partsBase(j->id);
      std::string video = base + ".video", audio = g_audioItag ? base + ".audio" : "", out = base + ".mp4.part";
      std::string title = j->title, channel = j->channel, id = j->id;
      if (g_joiner.joinable()) g_joiner.join();
      g_joining = true;
      g_joiner = std::thread([video, audio, out, title, channel, id] {
        bool ok = join(video, audio, out, title, channel, id);
        g_joined = g_joinStop ? 3 : ok ? 1 : 2;
        g_joining = false;
      });
    };
    if (keep.empty()) return joinNow();
    for (const CaptionTrack& t : keep) {
      std::string name = cleanLang(t.lang) + (t.autoGen ? ".auto" : "");
      g_core.get("/captions/" + Api::escape(id) + "/" + Api::escape(t.id), 0, [gen, name, joinNow](int status, const std::string& vtt) {
        if (gen != g_generation) return;
        if (status == 200 && vtt.compare(0, 6, "WEBVTT") == 0) g_subs.push_back({name, vtt});
        if (--g_subsPending == 0) joinNow();
      });
    }
  });
}

// a part that came whole: the next step
void partDone(Step step) {
  DownloadJob* j = current();
  g_misses = 0;
  if (j->song) startCover();
  else if (step == Picture && g_audioItag) startPart(Sound);
  else startCaptions();
}

void startPart(Step step) {
  DownloadJob* j = current();
  g_step = step;
  int itag = step == Picture ? g_videoItag : g_audioItag;
  int64_t bytes = step == Picture ? g_videoBytes : g_audioBytes;
  // the words a row shows ("Picture" read like the video's name for a picture, Nov 12)
  j->stage = j->song ? "Downloading" : step == Picture ? (g_audioItag ? "Downloading video" : "Downloading") : "Downloading sound";
  g_changed = true;
  if (g_cardAway) return stopRunning(DownloadJob::Waiting);  // the card is going to the computer
  std::string file = (j->song ? songParts(j->id) : partsBase(j->id)) + (step == Picture ? ".video" : ".audio");
  if (bytes > 0 && fileSize(file) == bytes) return partDone(step);  // it came whole before a restart
  // what of it came before (paused, a restart, a power cut): the part goes on from the bytes the card
  // surely has, if it's of the same file (YouTube gives the same size)
  readMarks();
  auto mark = g_marks.find(file);
  int64_t from = mark != g_marks.end() && mark->second.id == j->id && mark->second.total == bytes ? mark->second.synced : 0;
  int64_t have = fileSize(file + ".part");
  if (have < 0) from = 0;
  // whole and on the card for sure, only its rename lost (a power cut as it ended): it's done
  if (bytes > 0 && from == bytes && have == bytes && rename((file + ".part").c_str(), file.c_str()) == 0) return partDone(step);
  // not room for the rest of it (and 16 MB more): held, looked at again in a minute
  int64_t room = files::freeBytes(files::kRoots[0]);
  if (room >= 0 && room < bytes - from + (16 << 20)) {
    g_fullUntil = now_ns() + 60 * kSec;
    g_app->toast("The card is full: downloads wait");
    return stopRunning(DownloadJob::Waiting);
  }
  g_marks[file] = {j->id, file, from, bytes};
  writeMarks();
  g_markFile = file;
  g_coreBusy = true;
  g_seq = g_core.state().downloadSeq;
  g_markSeq = g_seq + 1;  // the core's next download is this one
  g_core.download("/s/" + Api::escape(j->id) + "/" + std::to_string(itag), file, from);
}

// ---- a song (YouTube Music) ----

// its file: <Album artist>/<Album> (<Year>)/<NN> - <Title>.m4a, as Organize names music
std::string songFile(const SongInfo& s) {
  std::string artist = cardName(!s.albumArtist.empty() ? s.albumArtist : !s.artist.empty() ? s.artist : "Unknown Artist");
  std::string album = cardName(!s.album.empty() ? s.album : s.title);
  if (artist.empty()) artist = "Unknown Artist";
  if (album.empty()) album = "Unknown Album";
  std::string title = cardName(s.title);
  if (title.empty()) title = "Song";
  char nn[16] = "";
  if (s.track > 0) snprintf(nn, sizeof(nn), "%02d - ", s.track);
  std::string dir = musicDownloadDir() + "/" + artist + "/" + album + (s.year ? " (" + std::to_string(s.year) + ")" : "");
  std::string file = dir + "/" + nn + title + ".m4a";
  for (int k = 2; fileThere(file); k++) file = dir + "/" + nn + title + " (" + std::to_string(k) + ").m4a";
  return file;
}

void startSaving() {
  if (g_cardAway) return stopRunning(DownloadJob::Waiting);
  if (!roomToSave()) return;
  DownloadJob* j = current();
  g_step = Joining;
  j->stage = "Saving";
  g_changed = true;
  g_joined = 0;
  std::string base = songParts(j->id), id = j->id;
  SongInfo s = g_song;
  std::string words = g_words;
  if (g_joiner.joinable()) g_joiner.join();
  g_joining = true;
  g_joiner = std::thread([base, id, s, words] {
    std::string out = base + ".saving.m4a";  // an .m4a: writeTags knows a file by its extension
    bool ok = join(base + ".audio", "", out, "", "", id, true) && !g_joinStop;
    std::string lyrics = words;
    if (ok && lyrics.compare(0, 1, "[") != 0) {  // not timed (YouTube Music's own often aren't): LRCLIB's if it has them
      bool timed = false;
      std::string other = lrclibLyrics(s.artist, s.title, s.album, s.seconds, timed);
      if (timed || lyrics.empty()) lyrics = other.empty() ? lyrics : other;
    }
    if (ok) {
      TagChanges t;
      t.title = s.title, t.artist = s.artist, t.albumArtist = !s.albumArtist.empty() ? s.albumArtist : s.artist, t.album = s.album;
      if (t.album.empty()) t.album = s.title;  // a single: the album Jellyfin files it under
      t.year = s.year, t.track = s.track, t.trackTotal = s.tracks, t.lyrics = lyrics;
      t.comment = "https://music.youtube.com/watch?v=" + id;
      if (FILE* f = fopen((base + ".cover").c_str(), "rb")) {
        char b[8192];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0 && t.cover.size() < (4u << 20)) t.cover.append(b, n);
        fclose(f);
      }
      bool picture = t.cover.size() > 3 && ((uint8_t(t.cover[0]) == 0xff && uint8_t(t.cover[1]) == 0xd8) || !t.cover.compare(1, 3, "PNG"));
      if (!picture) t.cover.clear();  // an error page, not a picture
      std::string why = g_joinStop ? "stopped" : writeTags(out, t);  // (LRCLIB took a while: the card may be going)
      if (!why.empty()) fprintf(stderr, "rondo: download %s: its tags weren't saved: %s\n", id.c_str(), why.c_str());
      ok = why.empty();
    } else {
      fprintf(stderr, "rondo: download %s: its sound couldn't be put in an .m4a\n", id.c_str());
    }
    g_joined = g_joinStop ? 3 : ok ? 1 : 2;
    g_joining = false;
  });
}

void startWords() {
  DownloadJob* j = current();
  g_step = Words;
  j->stage = "Getting lyrics";
  g_changed = true;
  uint32_t gen = g_generation;
  std::string q = "/lyrics?title=" + Api::escape(g_song.title) + "&artist=" + Api::escape(g_song.artist);
  if (!g_song.album.empty()) q += "&album=" + Api::escape(g_song.album);
  if (g_song.seconds > 0) q += "&duration=" + std::to_string(g_song.seconds);
  q += "&id=" + Api::escape(j->id);
  g_core.get(q, 0, [gen](int status, const std::string& body) {
    if (gen != g_generation) return;
    Lyrics l;
    g_words.clear();
    if (status == 200 && parseLyrics(body, l)) {
      if (!l.synced.empty()) {  // LRC: "[mm:ss.xx]line"
        for (const LyricLine& line : l.synced) {
          char t[16];
          snprintf(t, sizeof(t), "[%02d:%02d.%02d]", int(line.t / 60000), int(line.t / 1000 % 60), int(line.t % 1000 / 10));
          g_words += t + line.text + "\n";
        }
      } else {
        g_words = l.plain;
      }
    }
    startSaving();
  });
}

void startCover() {
  DownloadJob* j = current();
  if (g_song.cover.empty()) return startWords();
  if (g_cardAway) return stopRunning(DownloadJob::Waiting);
  g_step = Cover;
  j->stage = "Getting the cover";
  g_changed = true;
  g_coreBusy = true;
  g_seq = g_core.state().downloadSeq;
  // the largest /thumb gives (720 px a side; without w and h, a row's 240)
  std::string cover = g_song.cover + (g_song.cover.find('?') == std::string::npos ? "?" : "&") + "w=720&h=720";
  g_core.download(cover, songParts(j->id) + ".cover");
}

void startSong() {
  DownloadJob* j = current();
  uint32_t gen = g_generation;
  std::string id = j->id;
  // its details first: /music/song (a server before it has none: the row's own "Artist · Album")
  g_core.get("/music/song/" + Api::escape(id), 0, [gen, id](int status, const std::string& body) {
    if (gen != g_generation) return;
    DownloadJob* j = current();
    if (!j) return;
    if (status == 0) return retryLater();  // no connection: its details and its sound when there is one
    SongInfo s;
    s.title = j->title;
    size_t dot = j->channel.find(" \xc2\xb7 ");
    s.artist = j->channel.substr(0, dot);
    if (dot != std::string::npos) s.album = j->channel.substr(dot + 4);
    s.cover = j->thumb;
    if (status == 200)
      if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
        yyjson_val* r = yyjson_doc_get_root(d);
        auto str = [&](const char* k) { return sval(r, k); };
        auto num = [&](const char* k) { return int(yyjson_get_num(yyjson_obj_get(r, k))); };
        if (!str("title").empty()) s.title = str("title");
        if (!str("artist").empty()) s.artist = str("artist");
        s.albumArtist = str("albumArtist");
        if (!str("album").empty()) s.album = str("album");
        s.year = atoi(str("year").c_str());
        s.track = num("track"), s.tracks = num("tracks");
        if (!str("cover").empty()) s.cover = str("cover");
        if (num("durationSec") > 0) s.seconds = num("durationSec");
        yyjson_doc_free(d);
      }
    g_song = s;
    j->title = s.title;
    g_core.get("/resolve/" + Api::escape(id), 0, [gen](int status, const std::string& body) {
      if (gen != g_generation) return;
      DownloadJob* j = current();
      if (!j) return;
      if (status == 0) return retryLater();
      if (status != 200) return finish(DownloadJob::Failed, "YouTube didn't give this song");
      int64_t bytes = -1;
      if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
        yyjson_val* f = yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(d), "formats"), "140");
        if (f) bytes = int64_t(yyjson_get_num(yyjson_obj_get(f, "contentLength")));
        if (!g_song.seconds || g_song.seconds < 0) g_song.seconds = int(yyjson_get_num(yyjson_obj_get(yyjson_doc_get_root(d), "duration")));
        yyjson_doc_free(d);
      }
      if (bytes < 0) return finish(DownloadJob::Failed, "No sound the Y1 plays");
      g_videoItag = 0, g_audioItag = 140, g_videoBytes = 0, g_audioBytes = bytes;
      startPart(Sound);
    });
  });
}

void start(int at) {
  g_at = at;
  DownloadJob* j = current();
  // on the card already (Rondo stopped between saving it and noting the queue): nothing to fetch
  std::string there = j->song ? downloadedSong(j->id) : downloadedFile(j->id);
  if (!there.empty()) {
    j->file = there, j->percent = 100;
    return finish(DownloadJob::Done);
  }
  j->state = DownloadJob::Running, j->stage = "Starting";
  g_step = Resolving;
  g_changed = g_queueDirty = true;
  if (j->song) {
    mkdirs(musicDownloadDir());
    g_words.clear();
    ++g_generation;
    return startSong();
  }
  mkdirs(downloadDir());
  uint32_t gen = ++g_generation;
  g_core.get("/resolve/" + Api::escape(j->id), 0, [gen](int status, const std::string& body) {
    if (gen != g_generation) return;
    DownloadJob* j = current();
    if (!j) return;
    if (status == 0) return retryLater();
    if (status != 200) return finish(DownloadJob::Failed, "YouTube didn't give this video");
    yyjson_doc* d = yyjson_read(body.data(), body.size(), 0);
    std::map<int, std::pair<int, int64_t>> formats;  // itag -> height, bytes
    if (d) {
      size_t i, n;
      yyjson_val *k, *v;
      yyjson_obj_foreach(yyjson_obj_get(yyjson_doc_get_root(d), "formats"), i, n, k, v) {
        int h = int(yyjson_get_num(yyjson_obj_get(v, "h")));
        yyjson_val* len = yyjson_obj_get(v, "contentLength");
        formats[atoi(yyjson_get_str(k))] = {h, yyjson_is_num(len) ? int64_t(yyjson_get_num(len)) : 0};
      }
      yyjson_doc_free(d);
    }
    // the height asked for, else the next one down (as playback falls back)
    g_videoItag = g_audioItag = 0;
    std::vector<int> ladder;
    if (j->quality >= 720) ladder.push_back(136);
    if (j->quality >= 480) ladder.push_back(135);
    ladder.push_back(134);
    for (int itag : ladder)
      if (!g_videoItag && formats.count(itag)) g_videoItag = itag;
    if (g_videoItag && formats.count(140)) g_audioItag = 140;
    else if (formats.count(18)) g_videoItag = 18, g_audioItag = 0;  // picture and sound in one (360p)
    else return finish(DownloadJob::Failed, "No format the Y1 plays");
    j->quality = g_videoItag == 136 ? 720 : g_videoItag == 135 ? 480 : 360;
    g_videoBytes = formats[g_videoItag].second, g_audioBytes = g_audioItag ? formats[g_audioItag].second : 0;
    startPart(Picture);
  });
}

// ---- downloaded playlists ----

int jobOf(const std::string& id) {
  for (size_t i = 0; i < g_jobs.size(); i++)
    if (g_jobs[i].id == id) return int(i);
  return -1;
}

// its file in Library: the songs on the card so far, in its order (none yet: no file)
void writeListFile(const std::string& pid) {
  auto it = g_lists.find(pid);
  if (it == g_lists.end()) return;
  const ListRec& r = it->second;
  std::vector<std::string> paths;
  for (const std::string& id : r.ids) {
    auto s = g_songs.find(id);
    if (s != g_songs.end()) paths.push_back(s->second.file);
  }
  Playlist had;
  if (paths.empty() && !g_playlists.get(Playlists::youTubeKey(r.name), had)) return;
  if (!g_playlists.writeYouTube(r.name, paths)) fprintf(stderr, "rondo: downloads: playlist %s not written\n", r.name.c_str());
}

// a downloaded playlist's picture: <its file name>.jpg (or .png) in Playlists/YouTube Music
std::string listCoverBase(const std::string& name) { return g_playlists.youTubeFolder() + "/" + name; }
void removeListCover(const std::string& name) {
  for (const char* ext : {".jpg", ".png"}) unlink((listCoverBase(name) + ext).c_str());
}
// asked for beside its other requests (not the queue's one download), into a hidden file, then put in
// place as a .jpg or a .png, whichever it is (an error page is dropped)
void saveListCover(const std::string& pid, const std::string& thumb) {
  auto it = g_lists.find(pid);
  if (thumb.empty() || it == g_lists.end() || g_cardAway) return;
  std::string name = it->second.name, folder = g_playlists.youTubeFolder();
  mkdirs(folder);
  std::string tmp = folder + "/." + name + ".cover";
  std::string path = thumb + (thumb.find('?') == std::string::npos ? "?" : "&") + "w=480&h=480";
  g_core.save(path, tmp, [pid, name, tmp](int status, const std::string&) {
    uint8_t m[4] = {0, 0, 0, 0};
    if (FILE* f = fopen(tmp.c_str(), "rb")) {
      if (fread(m, 1, 4, f) != 4) m[0] = 0;
      fclose(f);
    }
    bool jpeg = m[0] == 0xff && m[1] == 0xd8, png = m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G';
    auto still = g_lists.find(pid);  // deleted meanwhile: none
    if (status != 200 || (!jpeg && !png) || still == g_lists.end() || still->second.name != name || g_cardAway) {
      unlink(tmp.c_str());
      return;
    }
    removeListCover(name);
    if (rename(tmp.c_str(), (listCoverBase(name) + (jpeg ? ".jpg" : ".png")).c_str()) != 0) unlink(tmp.c_str());
    g_changed = true;
  });
}

// A downloaded playlist with no cover on the card (downloaded before covers were, or its picture didn't
// come): asked for once a session, its picture's address first if it isn't known (its first page)
std::set<std::string> g_coverTried;
int64_t g_coverAt = 0;
bool online();
bool cardMounted();
void fetchMissingCovers() {
  if (g_cardAway || now_ns() - g_coverAt < 5 * kSec) return;
  g_coverAt = now_ns();
  for (auto& [pid, r] : g_lists) {
    if (g_coverTried.count(pid) || g_loads.count(pid) || !Playlists::coverOf(listCoverBase(r.name) + ".m3u8").empty()) continue;
    if (!online() || !cardMounted()) return;
    g_coverTried.insert(pid);
    if (!r.thumb.empty()) return saveListCover(pid, r.thumb);
    std::string id = pid;
    g_core.get("/music/playlist/" + Api::escape(pid), 120000, [id](int status, const std::string& body) {
      auto it = g_lists.find(id);
      if (status != 200 || it == g_lists.end()) return;
      std::string thumb;
      if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
        thumb = sval(yyjson_doc_get_root(d), "thumb");
        yyjson_doc_free(d);
      }
      if (thumb.empty()) {  // none: its first song's
        ItemsPage page;
        parseItems(body, page);
        if (!page.items.empty()) thumb = page.items[0].thumb;
      }
      if (thumb.empty()) return;
      it->second.thumb = thumb;
      writeSongs();
      saveListCover(id, thumb);
    });
    return;  // one at a time
  }
}

void playlistLoaded(const std::string& pid, ListLoad& l) {
  readSongs();
  ListRec& r = g_lists[pid];
  if (r.name.empty()) {
    // a file name no other downloaded playlist has, even one whose file isn't there yet (no song on the card)
    r.name = g_playlists.freeYouTubeName(l.title, [&pid](const std::string& n) {
      for (auto& [id, other] : g_lists)
        if (id != pid && !strcasecmp(other.name.c_str(), n.c_str())) return true;
      return false;
    });
    r.at = time(nullptr);
  }
  r.title = l.title, r.mix = l.mix;
  r.ids.clear();
  std::set<std::string> seen;
  std::vector<const Item*> songs;
  for (const Item& it : l.items)
    if (!it.id.empty() && it.id[0] != '/' && seen.insert(it.id).second) r.ids.push_back(it.id), songs.push_back(&it);
  int queued = 0, there = 0;
  for (const Item* it : songs) {
    if (!downloadedSong(it->id).empty()) {
      there++;
      continue;
    }
    int k = jobOf(it->id);
    if (k >= 0 && going(g_jobs[size_t(k)])) continue;  // on its way already
    if (downloadSong(*it, true, false)) queued++;
  }
  // its cover beside it: YouTube's picture of the playlist, else its first song's (again at each update:
  // a mix's changes)
  r.thumb = !l.thumb.empty() ? l.thumb : songs.empty() ? "" : songs[0]->thumb;
  writeSongs();
  writeListFile(pid);
  saveListCover(pid, r.thumb);
  g_changed = true;
  if (songs.empty()) g_app->toast("No songs in " + l.title);
  else if (queued) g_app->toast("Downloading " + std::to_string(queued) + (queued == 1 ? " song" : " songs") + " of " + l.title);
  else if (there == int(songs.size())) g_app->toast(l.title + " is on the card");
  else g_app->toast("Its songs are on their way");
}

// its songs, page after page (a mix: the first page; it never ends)
void loadPage(const std::string& pid, const std::string& next) {
  std::string path = "/music/playlist/" + Api::escape(pid) + (next.empty() ? "" : "?next=" + Api::escape(next));
  g_core.get(path, 0, [pid, first = next.empty()](int status, const std::string& body) {
    auto it = g_loads.find(pid);
    if (it == g_loads.end()) return;  // deleted meanwhile
    ListLoad& l = it->second;
    if (status != 200 && (first || status != 410)) {  // 410: a next page's token ran out: what came is kept
      g_app->toast("Couldn't get " + l.title + ": " + (status == 0 ? "no connection" : "HTTP " + std::to_string(status)));
      g_loads.erase(it);
      g_changed = true;
      return;
    }
    ItemsPage page;
    if (status == 200) {
      parseItems(body, page);
      if (first)
        if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
          std::string t = sval(yyjson_doc_get_root(d), "title");
          if (!t.empty()) l.title = t;
          l.thumb = sval(yyjson_doc_get_root(d), "thumb");
          yyjson_doc_free(d);
        }
      for (const Item& i : page.items)
        if (i.kind == "song" || i.kind == "video") l.items.push_back(i);
      g_changed = true;
    }
    if (status == 200 && !page.next.empty() && !l.mix && l.items.size() < 5000) return loadPage(pid, page.next);
    playlistLoaded(pid, l);
    g_loads.erase(pid);
  });
}

// the ids a downloaded playlist other than `except`, or a download asked for by itself, still wants
std::set<std::string> wanted(const std::string& except) {
  std::set<std::string> w;
  for (auto& [id, r] : g_lists)
    if (id != except) w.insert(r.ids.begin(), r.ids.end());
  for (auto& [id, m] : g_songs)
    if (m.alone) w.insert(id);
  for (const DownloadJob& j : g_jobs)
    if (j.alone && going(j)) w.insert(j.id);
  return w;
}

// a job out of the queue altogether (its parts too)
void dropJob(const std::string& id) {
  int k = jobOf(id);
  if (k < 0) return;
  if (k == g_at) {
    if (g_step == Joining) return;  // being saved: it stays a song on the card
    stopRunning(DownloadJob::Waiting);
  }
  g_jobs.erase(g_jobs.begin() + k);
  if (g_at > k) g_at--;
  removeParts(id);
  g_changed = g_queueDirty = true;
}

}  // namespace

bool joinDownloadParts(const std::string& video, const std::string& audio, const std::string& out) {
  return join(video, audio, out, "", "", "");
}

bool downloadVideo(const Item& item, int quality) {
  ensureRead();
  if (item.id.empty() || item.id[0] == '/') return false;
  if (!downloadedFile(item.id).empty()) {
    g_app->toast("Already on the card");
    return false;
  }
  for (const DownloadJob& j : g_jobs)
    if (j.id == item.id && going(j)) {
      g_app->toast(j.state == DownloadJob::Paused ? "It's paused in Downloads" : "Already downloading");
      return false;
    }
  // a failed or cancelled one is tried again in its place
  g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [&](const DownloadJob& j) { return j.id == item.id; }), g_jobs.end());
  if (g_at >= int(g_jobs.size())) g_at = -1;
  for (size_t i = 0; i < g_jobs.size(); i++)  // the index of the running one may have moved
    if (g_jobs[i].state == DownloadJob::Running) g_at = int(i);
  DownloadJob j;
  // the channel alone: a row's subtitle also has its views and age ("Channel · 380 views · 1 hour ago")
  j.id = item.id, j.title = item.title.empty() ? item.id : item.title, j.thumb = item.thumb;
  j.channel = item.subtitle.substr(0, item.subtitle.find(" \xc2\xb7 "));
  j.quality = quality;
  g_jobs.push_back(j);
  g_changed = g_queueDirty = true;
  g_app->toast(g_at >= 0 ? "Download waiting: " + j.title : "Downloading " + std::to_string(quality) + "p: " + j.title);
  return true;
}

const std::vector<DownloadJob>& downloadJobs() {
  ensureRead();
  return g_jobs;
}

bool downloadsActive() {
  ensureRead();
  bool waiting = false;
  for (const DownloadJob& j : g_jobs) {
    if (j.state == DownloadJob::Running) return true;
    waiting = waiting || j.state == DownloadJob::Waiting;
  }
  // held: only a short wait to try again keeps the CPU up (no Wi-Fi, the card away or full don't)
  return waiting && (g_held.empty() || (g_held == "Waiting to try again" && g_misses <= 3));
}

void cancelDownload(const std::string& id) {
  ensureRead();
  for (size_t i = 0; i < g_jobs.size(); i++) {
    DownloadJob& j = g_jobs[i];
    if (j.id != id) continue;
    if (j.state == DownloadJob::Failed) {  // one that didn't download: off the list
      g_jobs.erase(g_jobs.begin() + long(i));
      if (g_at > int(i)) g_at--;
      g_changed = g_queueDirty = true;
      return;
    }
    if (j.state == DownloadJob::Waiting || j.state == DownloadJob::Paused) {
      j.state = DownloadJob::Failed, j.error = "Cancelled";
      removeParts(id);
    } else if (j.state == DownloadJob::Running && int(i) == g_at) {
      if (g_step == Joining) return;  // nearly done: let it finish
      if (g_coreBusy) g_core.cancelDownload();  // it ends as failed: the next waits for that (downloadsTick)
      j.state = DownloadJob::Failed, j.error = "Cancelled";
      removeParts(id);
      g_at = -1, g_step = Idle, g_subs.clear(), g_subsPending = 0;
      g_generation++;
    }
    g_changed = g_queueDirty = true;
  }
}

void retryDownload(const std::string& id) {
  ensureRead();
  for (DownloadJob& j : g_jobs)
    if (j.id == id && j.state == DownloadJob::Failed) {
      j.state = DownloadJob::Waiting, j.error = "", j.percent = 0, j.stage = "";
      g_changed = g_queueDirty = true;
    }
}

void pauseDownload(const std::string& id) {
  ensureRead();
  for (size_t i = 0; i < g_jobs.size(); i++) {
    DownloadJob& j = g_jobs[i];
    if (j.id != id) continue;
    if (j.state == DownloadJob::Waiting) j.state = DownloadJob::Paused, g_changed = g_queueDirty = true;
    else if (j.state == DownloadJob::Running && int(i) == g_at) stopRunning(DownloadJob::Paused);
  }
}

void resumeDownload(const std::string& id) {
  ensureRead();
  for (DownloadJob& j : g_jobs)
    if (j.id == id && j.state == DownloadJob::Paused) j.state = DownloadJob::Waiting, g_changed = g_queueDirty = true;
  g_retryAt = 0, g_fullUntil = 0;  // asked for: tried at once
}

void pauseDownloads() {
  ensureRead();
  if (current()) stopRunning(DownloadJob::Paused);
  for (DownloadJob& j : g_jobs)
    if (j.state == DownloadJob::Waiting) j.state = DownloadJob::Paused, g_changed = g_queueDirty = true;
}

void resumeDownloads() {
  ensureRead();
  for (DownloadJob& j : g_jobs)
    if (j.state == DownloadJob::Paused) j.state = DownloadJob::Waiting, g_changed = g_queueDirty = true;
  g_retryAt = 0, g_fullUntil = 0;
}

bool downloadsGoing() {
  ensureRead();
  for (const DownloadJob& j : g_jobs)
    if (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running) return true;
  return false;
}

bool downloadsPaused() {
  ensureRead();
  for (const DownloadJob& j : g_jobs)
    if (j.state == DownloadJob::Paused) return true;
  return false;
}

std::string downloadsHeld() { return g_held; }

// a change not handed out by downloadsTick yet counts already
uint32_t downloadsSerial() { return g_serial + (g_changed ? 1 : 0); }

void downloadsCardAway(bool away) {
  g_joinStop = away;  // a save under way stops at once (done again when the card is back)
  g_cardAway = away;
}

bool downloadsUsingCard() { return g_core.state().downloadState == 1 || g_joining; }

std::string downloadIdOf(const std::string& path) {
  // "<Title> [<id>].mp4": the 11 characters of a YouTube id in the last brackets
  size_t close = path.rfind("].");
  if (close == std::string::npos || close < 12 || path[close - 12] != '[') return "";
  std::string id = path.substr(close - 11, 11);
  for (char c : id)
    if (!isalnum((unsigned char)c) && c != '-' && c != '_') return "";
  return id;
}

std::string downloadedFile(const std::string& id) {
  refreshFiles();
  auto it = g_files.find(id);
  return it == g_files.end() ? "" : it->second;
}

std::vector<Item> downloadedVideos() {
  refreshFiles();
  readMeta();
  struct Row {
    Item item;
    int64_t at;
  };
  std::vector<Row> rows;
  for (auto& [id, path] : g_files) {
    Item it;
    it.kind = "video", it.id = path;
    auto m = g_meta.find(id);
    std::string name = path.substr(path.rfind('/') + 1);
    size_t br = name.rfind(" [");
    it.title = m != g_meta.end() && !m->second.title.empty() ? m->second.title : name.substr(0, br == std::string::npos ? name.size() - 4 : br);
    struct stat st;
    int64_t size = stat(path.c_str(), &st) == 0 ? int64_t(st.st_size) : 0;
    char mb[32];
    snprintf(mb, sizeof(mb), "%.0f MB", double(size) / 1e6);
    std::string sub = m != g_meta.end() ? m->second.channel : "";
    if (m != g_meta.end() && m->second.quality) sub += (sub.empty() ? "" : " \xc2\xb7 ") + std::to_string(m->second.quality) + "p";
    it.subtitle = sub + (sub.empty() ? "" : " \xc2\xb7 ") + mb;
    it.thumb = path;
    rows.push_back({it, m != g_meta.end() && m->second.at ? m->second.at : int64_t(st.st_mtime)});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.at > b.at; });
  std::vector<Item> out;
  for (Row& r : rows) out.push_back(std::move(r.item));
  return out;
}

bool deleteDownload(const std::string& file) {
  if (file.compare(0, downloadDir().size() + 1, downloadDir() + "/") != 0 || file.find("/..") != std::string::npos) return false;
  std::string id = downloadIdOf(file);
  if (id.empty() || unlink(file.c_str()) != 0) return false;
  // its subtitles: "<name>.<lang>.vtt"
  std::string stem = file.substr(0, file.size() - 4), dir = downloadDir();
  std::string base = stem.substr(stem.rfind('/') + 1);
  if (DIR* d = opendir(dir.c_str())) {
    std::vector<std::string> gone;
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > base.size() + 4 && n.compare(0, base.size() + 1, base + ".") == 0 && n.compare(n.size() - 4, 4, ".vtt") == 0)
        gone.push_back(dir + "/" + n);
    }
    closedir(d);
    for (const std::string& g : gone) unlink(g.c_str());
  }
  readMeta();
  g_meta.erase(id);
  writeMeta();
  refreshFiles(true);
  g_library.scanAsync();
  g_changed = true;
  return true;
}

bool downloadsTick() {
  ensureRead();
  readSongs();
  fetchMissingCovers();
  CoreState c = g_core.state();
  // what of the part the card surely has, as the core says (it flushes every 1 MB, and as it stops)
  if (g_markSeq >= 0 && c.downloadSeq == g_markSeq) {
    auto mark = g_marks.find(g_markFile);
    if (mark != g_marks.end() && mark->second.synced != c.downloadSynced) {
      mark->second.synced = c.downloadSynced;
      writeMarks();
    }
  }
  DownloadJob* j = current();
  // the card going to the computer: what runs lets go of it first (usb_screen.cpp waits for that)
  if (j && g_cardAway && g_step != Joining) {
    stopRunning(DownloadJob::Waiting);
    j = nullptr;
  }
  // Wi-Fi gone for 3 s while one runs: it waits for it, keeping what came (the core would try for 2.5
  // minutes, the row saying it downloads)
  if (j && g_step != Joining && now_ns() - g_onlineAt >= kSec) {
    g_onlineAt = now_ns();
    if (online()) g_offlineSince = 0;
    else if (!g_offlineSince) g_offlineSince = now_ns();
    else if (now_ns() - g_offlineSince >= 3 * kSec) {
      g_offlineSince = 0;
      stopRunning(DownloadJob::Waiting);
      j = nullptr;
    }
  }
  if (!j) {
    if (g_coreBusy && (c.downloadSeq != g_seq && c.downloadState != 1)) g_coreBusy = false;  // a cancelled one has ended
    int next = -1;
    for (size_t i = 0; i < g_jobs.size() && next < 0; i++)
      if (g_jobs[i].state == DownloadJob::Waiting) next = int(i);
    if (next < 0) {
      if (!g_held.empty()) g_held.clear(), g_changed = true;
    } else if (!g_coreBusy && c.downloadState != 1 && now_ns() - g_heldAt >= 900 * kMs) {  // Software update's download first
      g_heldAt = now_ns();
      std::string h = holdNow();
      if (h != g_held) g_held = h, g_changed = true;
      if (h.empty()) start(next);
    }
  } else if (g_step == Picture || g_step == Sound) {
    if (c.downloadSeq != g_seq) {  // the core has begun this part
      int64_t total = g_videoBytes + g_audioBytes;
      int64_t done = (g_step == Sound ? g_videoBytes : 0) + c.downloadDone;
      int pct = total > 0 ? int(std::min<int64_t>(99, done * 100 / total)) : 0;
      if (pct != j->percent) j->percent = pct, g_changed = true;
      if (c.downloadState == 2) {
        g_coreBusy = false;
        partDone(g_step);
      } else if (c.downloadState == 3) {
        g_coreBusy = false;
        if (c.downloadStatus != 0) {
          finish(DownloadJob::Failed, "The stream was refused (" + std::to_string(c.downloadStatus) + ")");
        } else if (g_cardAway || !cardMounted()) {  // the card went: on when it's back
          stopRunning(DownloadJob::Waiting);
        } else {
          int64_t room = files::freeBytes(files::kRoots[0]);
          if (room >= 0 && room < (8 << 20)) {
            g_fullUntil = now_ns() + 60 * kSec;
            g_app->toast("The card is full: downloads wait");
            stopRunning(DownloadJob::Waiting);
          } else {
            retryLater();  // the connection: what came is kept, the rest when it's back
          }
        }
      }
    }
  } else if (g_step == Cover) {  // a song's cover: without one it's saved all the same
    if (c.downloadSeq != g_seq && (c.downloadState == 2 || c.downloadState == 3)) {
      g_coreBusy = false;
      startWords();
    }
  } else if (g_step == Joining && g_joined != 0 && g_cardAway) {
    // ended as the card went: moved into place (or done again) once it's back
  } else if (g_step == Joining && (g_joined == 3 || (g_joined == 2 && !roomLeft()))) {
    // stopped for the computer, or the card filled up: its parts kept, saved again later
    if (g_joiner.joinable()) g_joiner.join();
    unlink((songParts(j->id) + ".saving.m4a").c_str());
    unlink((partsBase(j->id) + ".mp4.part").c_str());
    if (g_joined == 2) g_fullUntil = now_ns() + 60 * kSec, g_app->toast("The card is full: downloads wait");
    g_step = Idle;  // (stopRunning leaves a save be)
    stopRunning(DownloadJob::Waiting);
  } else if (g_step == Joining && g_joined != 0 && j->song) {
    if (g_joiner.joinable()) g_joiner.join();
    std::string part = songParts(j->id) + ".saving.m4a", file = songFile(g_song);
    mkdirs(file.substr(0, file.rfind('/')));
    if (g_joined == 1 && rename(part.c_str(), file.c_str()) == 0) {
      removeParts(j->id);
      readSongs();
      bool alone = j->alone || (g_songs.count(j->id) && g_songs[j->id].alone);
      g_songs[j->id] = {file, g_song.title, g_song.artist, g_song.album, int64_t(time(nullptr)), alone};
      writeSongs();
      j->file = file, j->percent = 100;
      std::string title = j->title, id = j->id;
      finish(DownloadJob::Done);
      for (auto& [pid, r] : g_lists)  // in its place in the playlists it's in
        if (std::find(r.ids.begin(), r.ids.end(), id) != r.ids.end()) writeListFile(pid);
      g_library.scanAsync();
      g_app->toast("Downloaded: " + title);
    } else {
      finish(DownloadJob::Failed, "Couldn't save it to the card");
    }
  } else if (g_step == Joining && g_joined != 0) {
    if (g_joiner.joinable()) g_joiner.join();
    std::string base = partsBase(j->id);
    std::string name = fileTitle(j->title) + " [" + j->id + "]";
    std::string file = downloadDir() + "/" + name + ".mp4";
    if (g_joined == 1 && rename((base + ".mp4.part").c_str(), file.c_str()) == 0) {
      for (auto& [lang, vtt] : g_subs)
        if (FILE* f = fopen((downloadDir() + "/" + name + "." + lang + ".vtt").c_str(), "wb")) {
          fwrite(vtt.data(), 1, vtt.size(), f);
          fclose(f);
        }
      removeParts(j->id);
      readMeta();
      g_meta[j->id] = {j->title, j->channel, j->quality, int64_t(time(nullptr))};
      writeMeta();
      j->file = file, j->percent = 100;
      std::string title = j->title;
      finish(DownloadJob::Done);
      refreshFiles(true);
      g_library.scanAsync();
      g_app->toast("Downloaded: " + title);
    } else {
      finish(DownloadJob::Failed, "Couldn't save it to the card");
    }
  }
  if (g_queueDirty) saveQueue();
  bool changed = g_changed;
  if (changed) g_serial++;
  g_changed = false;
  return changed;
}

// ---- songs ----

bool downloadSong(const Item& item, bool quiet, bool alone) {
  ensureRead();
  if (item.id.empty() || item.id[0] == '/') return false;
  if (!downloadedSong(item.id).empty()) {
    if (alone && !g_songs[item.id].alone) g_songs[item.id].alone = true, writeSongs();  // kept if its playlist goes
    if (!quiet) g_app->toast("Already on the card");
    return false;
  }
  for (DownloadJob& j : g_jobs)
    if (j.id == item.id && going(j)) {
      if (alone && !j.alone) j.alone = true, g_queueDirty = true;
      if (!quiet) g_app->toast(j.state == DownloadJob::Paused ? "It's paused in Downloads" : "Already downloading");
      return false;
    }
  g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [&](const DownloadJob& j) { return j.id == item.id; }), g_jobs.end());
  if (g_at >= int(g_jobs.size())) g_at = -1;
  for (size_t i = 0; i < g_jobs.size(); i++)
    if (g_jobs[i].state == DownloadJob::Running) g_at = int(i);
  DownloadJob j;
  j.id = item.id, j.title = item.title.empty() ? item.id : item.title, j.thumb = item.thumb, j.song = true, j.alone = alone;
  j.channel = item.subtitle;  // "Artist · Album": the song's details when /music/song can't say
  g_jobs.push_back(j);
  g_changed = g_queueDirty = true;
  if (!quiet) g_app->toast(g_at >= 0 ? "Download waiting: " + j.title : "Downloading: " + j.title);
  return true;
}

int downloadSongs(const std::vector<Item>& songs) {
  int n = 0;
  for (const Item& s : songs)
    if ((s.kind == "song" || s.kind == "video") && downloadSong(s, true)) n++;
  g_app->toast(n ? "Downloading " + std::to_string(n) + (n == 1 ? " song" : " songs") : "They're on the card already");
  return n;
}

std::string downloadedSong(const std::string& id) {
  readSongs();
  auto it = g_songs.find(id);
  return it != g_songs.end() && fileThere(it->second.file) ? it->second.file : "";
}

std::vector<Item> downloadedSongs() {
  readSongs();
  std::vector<std::pair<int64_t, Item>> rows;
  for (auto& [id, m] : g_songs) {
    if (!fileThere(m.file)) continue;  // deleted or moved on the card since
    Item it;
    it.kind = "song", it.id = m.file, it.title = m.title;
    it.subtitle = m.artist + (m.album.empty() ? "" : " \xc2\xb7 " + m.album);
    it.thumb = m.file;
    if (const Track* t = g_library.find(m.file)) it.durationSec = t->durationSec;
    rows.push_back({m.at, it});
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<Item> out;
  for (auto& r : rows) out.push_back(std::move(r.second));
  return out;
}

bool deleteSong(const std::string& file) {
  std::string dir = musicDownloadDir();
  if (file.compare(0, dir.size() + 1, dir + "/") != 0 || file.find("/..") != std::string::npos) return false;
  readSongs();
  bool known = false;
  for (auto it = g_songs.begin(); it != g_songs.end();)
    if (it->second.file == file) it = g_songs.erase(it), known = true;
    else ++it;
  if (!known) return false;
  writeSongs();
  if (!g_library.deleteFile(file) && unlink(file.c_str()) != 0 && errno != ENOENT) return false;
  g_playlists.forget(file);  // out of every playlist (the downloaded ones keep its id: Update brings it back)
  // its album's and artist's folders when that was their last song
  std::string d = file.substr(0, file.rfind('/'));
  for (int k = 0; k < 2 && d.size() > dir.size() && rmdir(d.c_str()) == 0; k++) d = d.substr(0, d.rfind('/'));
  g_changed = true;
  return true;
}

// ---- downloaded playlists ----

bool downloadPlaylist(const std::string& id, const std::string& title) {
  ensureRead();
  readSongs();
  if (id.empty()) return false;
  if (g_loads.count(id)) {
    g_app->toast("Getting its songs already");
    return false;
  }
  ListLoad& l = g_loads[id];
  l.title = title.empty() ? "Playlist" : title;
  l.mix = isMixId(id);
  l.at = time(nullptr);
  g_changed = true;
  g_app->toast((g_lists.count(id) ? "Updating " : "Downloading ") + l.title);
  loadPage(id, "");
  return true;
}

std::vector<PlaylistDownload> playlistDownloads() {
  ensureRead();
  readSongs();
  // one deleted from the card (Files, a computer) with nothing on its way: forgotten (looked at every 10 s)
  static int64_t lookedAt = 0;
  bool card = now_ns() - lookedAt > 10 * kSec && !g_cardAway && cardMounted();
  if (card) lookedAt = now_ns();
  std::vector<std::pair<int64_t, PlaylistDownload>> rows;
  std::set<std::string> goingIds, pausedIds;
  for (const DownloadJob& j : g_jobs) {
    if (going(j)) goingIds.insert(j.id);
    if (j.state == DownloadJob::Paused) pausedIds.insert(j.id);
  }
  bool pruned = false;
  for (auto it = g_lists.begin(); it != g_lists.end();) {
    const ListRec& r = it->second;
    PlaylistDownload p;
    p.id = it->first, p.title = r.title, p.name = Playlists::youTubeKey(r.name), p.mix = r.mix, p.loading = g_loads.count(it->first) > 0;
    p.total = int(r.ids.size());
    for (const std::string& id : r.ids)
      p.onCard += g_songs.count(id) ? 1 : 0, p.going += goingIds.count(id) ? 1 : 0, p.paused += pausedIds.count(id) ? 1 : 0;
    Playlist had;
    if (card && !p.loading && !p.going && !g_playlists.get(p.name, had)) {
      removeListCover(r.name);
      it = g_lists.erase(it), pruned = true;
      continue;
    }
    p.cover = Playlists::coverOf(listCoverBase(r.name) + ".m3u8");
    rows.push_back({r.at, p});
    ++it;
  }
  if (pruned) writeSongs();
  for (auto& [id, l] : g_loads)  // a new one, its songs still being asked for
    if (!g_lists.count(id)) {
      PlaylistDownload p;
      p.id = id, p.title = l.title, p.mix = l.mix, p.loading = true, p.total = int(l.items.size());
      rows.push_back({l.at, p});
    }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<PlaylistDownload> out;
  for (auto& r : rows) out.push_back(std::move(r.second));
  return out;
}

bool playlistDownload(const std::string& id, PlaylistDownload& out) {
  for (PlaylistDownload& p : playlistDownloads())
    if (p.id == id) {
      out = p;
      return true;
    }
  return false;
}

std::string playlistDownloadOf(const std::string& libraryName) {
  readSongs();
  for (auto& [id, r] : g_lists)
    if (!strcasecmp(Playlists::youTubeKey(r.name).c_str(), libraryName.c_str())) return id;
  return "";
}

bool deletePlaylistDownload(const std::string& pid, bool songsToo) {
  ensureRead();
  readSongs();
  g_loads.erase(pid);
  auto it = g_lists.find(pid);
  if (it == g_lists.end()) return false;
  ListRec r = it->second;
  g_lists.erase(it);
  std::set<std::string> want = wanted(pid);
  // the downloads only it waited for
  for (const std::string& id : r.ids) {
    int k = jobOf(id);
    if (k >= 0 && !want.count(id) && going(g_jobs[size_t(k)])) dropJob(id);
  }
  g_playlists.remove(Playlists::youTubeKey(r.name));
  removeListCover(r.name);
  writeSongs();
  int gone = 0;
  if (songsToo)
    for (const std::string& id : r.ids) {
      if (want.count(id)) continue;
      auto s = g_songs.find(id);
      if (s != g_songs.end() && deleteSong(std::string(s->second.file))) gone++;
    }
  if (gone) g_library.scanAsync();
  g_changed = true;
  return true;
}

bool deleteLibraryPlaylist(const std::string& name, bool songsToo) {
  std::string pid = playlistDownloadOf(name);
  if (!pid.empty()) return deletePlaylistDownload(pid, songsToo);
  Playlist p;
  std::string cover = g_playlists.get(name, p) && p.youtube ? p.cover : "";  // a downloaded one Rondo lost track of
  if (!g_playlists.remove(name).empty()) return false;
  if (!cover.empty()) unlink(cover.c_str());
  return true;
}

std::string downloadsJson() {
  ensureRead();
  std::string s = "{\"dir\":" + jstr(downloadDir()) + ",\"held\":" + jstr(g_held) + ",\"jobs\":[";
  int n = 0;
  for (const DownloadJob& j : g_jobs) {
    s += std::string(n++ ? "," : "") + "{\"id\":" + jstr(j.id) + ",\"kind\":\"" + (j.song ? "song" : "video") + "\",\"title\":" + jstr(j.title) + ",\"channel\":" + jstr(j.channel) +
         ",\"quality\":" + std::to_string(j.quality) + ",\"state\":\"" + stateName(j.state) + "\",\"percent\":" + std::to_string(j.percent) +
         ",\"stage\":" + jstr(j.stage) + ",\"error\":" + jstr(j.error) + ",\"file\":" + jstr(j.file) + "}";
  }
  s += "],\"files\":[";
  n = 0;
  for (const Item& it : downloadedVideos())
    s += std::string(n++ ? "," : "") + "{\"file\":" + jstr(it.id) + ",\"id\":" + jstr(downloadIdOf(it.id)) + ",\"title\":" + jstr(it.title) +
         ",\"subtitle\":" + jstr(it.subtitle) + "}";
  s += "],\"songs\":[";
  n = 0;
  readSongs();
  std::map<std::string, std::string> idOf;
  for (auto& [k, m] : g_songs) idOf[m.file] = k;
  for (const Item& it : downloadedSongs())
    s += std::string(n++ ? "," : "") + "{\"file\":" + jstr(it.id) + ",\"id\":" + jstr(idOf[it.id]) + ",\"title\":" + jstr(it.title) +
         ",\"subtitle\":" + jstr(it.subtitle) + "}";
  s += "],\"playlists\":[";
  n = 0;
  for (const PlaylistDownload& p : playlistDownloads())
    s += std::string(n++ ? "," : "") + "{\"id\":" + jstr(p.id) + ",\"title\":" + jstr(p.title) + ",\"name\":" + jstr(p.name) + ",\"cover\":" + jstr(p.cover) +
         ",\"mix\":" + (p.mix ? "true" : "false") +
         ",\"loading\":" + (p.loading ? "true" : "false") + ",\"total\":" + std::to_string(p.total) + ",\"onCard\":" + std::to_string(p.onCard) +
         ",\"going\":" + std::to_string(p.going) + ",\"paused\":" + std::to_string(p.paused) + "}";
  return s + "]}";
}

}  // namespace rondo::shell
