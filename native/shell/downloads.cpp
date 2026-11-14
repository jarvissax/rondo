// Downloads; see downloads.h.
#include "downloads.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
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
std::atomic<int> g_joined{0};  // 0 joining, 1 done, 2 failed
std::thread g_joiner;
bool g_changed = false;

// what's on the card: id -> file, read again when the folder changes
std::map<std::string, std::string> g_files;
int64_t g_filesAt = -1;  // the folder's mtime when read (ns)

std::string metaPath() { return g_prefsPath.substr(0, g_prefsPath.rfind('/')) + "/downloads.json"; }

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
};
std::map<std::string, SongMeta> g_songs;
bool g_songsRead = false;
std::string songsPath() { return g_prefsPath.substr(0, g_prefsPath.rfind('/')) + "/music-downloads.json"; }
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
    auto s = [&](const char* f) {
      yyjson_val* x = yyjson_obj_get(v, f);
      return yyjson_is_str(x) ? std::string(yyjson_get_str(x)) : std::string();
    };
    m.title = s("title"), m.channel = s("channel");
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
  j += "}\n";
  std::string tmp = metaPath() + ".part";
  if (FILE* f = fopen(tmp.c_str(), "wb")) {
    bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
    ok = fclose(f) == 0 && ok;
    if (ok) rename(tmp.c_str(), metaPath().c_str());
    else unlink(tmp.c_str());
  }
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
void removeParts(const std::string& id) {
  for (const char* x : {".video", ".video.part", ".audio", ".audio.part", ".mp4.part"}) unlink((partsBase(id) + x).c_str());
  for (const char* x : {".audio", ".audio.part", ".cover", ".cover.part", ".saving.m4a"}) unlink((songParts(id) + x).c_str());
}

void readSongs() {
  if (g_songsRead) return;
  g_songsRead = true;
  yyjson_doc* d = yyjson_read_file(songsPath().c_str(), 0, nullptr, nullptr);
  if (!d) return;
  size_t i, n;
  yyjson_val *k, *v;
  yyjson_obj_foreach(yyjson_doc_get_root(d), i, n, k, v) {
    auto s = [&](const char* f) {
      yyjson_val* x = yyjson_obj_get(v, f);
      return yyjson_is_str(x) ? std::string(yyjson_get_str(x)) : std::string();
    };
    SongMeta m;
    m.file = s("file"), m.title = s("title"), m.artist = s("artist"), m.album = s("album");
    m.at = yyjson_get_sint(yyjson_obj_get(v, "at"));
    if (!m.file.empty()) g_songs[yyjson_get_str(k)] = m;
  }
  yyjson_doc_free(d);
}
void writeSongs() {
  std::string j = "{";
  int n = 0;
  for (auto& [id, m] : g_songs)
    j += std::string(n++ ? "," : "") + jstr(id) + ":{\"file\":" + jstr(m.file) + ",\"title\":" + jstr(m.title) + ",\"artist\":" + jstr(m.artist) +
         ",\"album\":" + jstr(m.album) + ",\"at\":" + std::to_string(m.at) + "}";
  j += "}\n";
  std::string tmp = songsPath() + ".part";
  if (FILE* f = fopen(tmp.c_str(), "wb")) {
    bool ok = fwrite(j.data(), 1, j.size(), f) == j.size();
    ok = fclose(f) == 0 && ok;
    if (ok) rename(tmp.c_str(), songsPath().c_str());
    else unlink(tmp.c_str());
  }
}
bool fileThere(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

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

void finish(DownloadJob::State state, const std::string& error = "") {
  DownloadJob* j = current();
  if (!j) return;
  j->state = state, j->error = error, j->stage = "";
  if (state == DownloadJob::Failed) {
    removeParts(j->id);
    g_app->toast("Download failed: " + j->title);
  }
  g_at = -1, g_step = Idle, g_subs.clear(), g_subsPending = 0;
  g_generation++;
  g_changed = true;
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
      DownloadJob* j = current();
      g_step = Joining;
      j->stage = "Saving";
      g_changed = true;
      g_joined = 0;
      std::string base = partsBase(j->id);
      std::string video = base + ".video", audio = g_audioItag ? base + ".audio" : "", out = base + ".mp4.part";
      std::string title = j->title, channel = j->channel, id = j->id;
      if (g_joiner.joinable()) g_joiner.join();
      g_joiner = std::thread([video, audio, out, title, channel, id] { g_joined = join(video, audio, out, title, channel, id) ? 1 : 2; });
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

void startPart(Step step) {
  DownloadJob* j = current();
  g_step = step;
  int itag = step == Picture ? g_videoItag : g_audioItag;
  // the words a row shows ("Picture" read like the video's name for a picture, Nov 12)
  j->stage = j->song ? "Downloading" : step == Picture ? (g_audioItag ? "Downloading video" : "Downloading") : "Downloading sound";
  g_changed = true;
  g_coreBusy = true;
  g_seq = g_core.state().downloadSeq;
  std::string base = j->song ? songParts(j->id) : partsBase(j->id);
  g_core.download("/s/" + Api::escape(j->id) + "/" + std::to_string(itag), base + (step == Picture ? ".video" : ".audio"));
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
  DownloadJob* j = current();
  g_step = Joining;
  j->stage = "Saving";
  g_changed = true;
  g_joined = 0;
  std::string base = songParts(j->id), id = j->id;
  SongInfo s = g_song;
  std::string words = g_words;
  if (g_joiner.joinable()) g_joiner.join();
  g_joiner = std::thread([base, id, s, words] {
    std::string out = base + ".saving.m4a";  // an .m4a: writeTags knows a file by its extension
    bool ok = join(base + ".audio", "", out, "", "", id, true);
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
      std::string why = writeTags(out, t);
      if (!why.empty()) fprintf(stderr, "rondo: download %s: its tags weren't saved: %s\n", id.c_str(), why.c_str());
      ok = why.empty();
    } else {
      fprintf(stderr, "rondo: download %s: its sound couldn't be put in an .m4a\n", id.c_str());
    }
    g_joined = ok ? 1 : 2;
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
    SongInfo s;
    s.title = j->title;
    size_t dot = j->channel.find(" \xc2\xb7 ");
    s.artist = j->channel.substr(0, dot);
    if (dot != std::string::npos) s.album = j->channel.substr(dot + 4);
    s.cover = j->thumb;
    if (status == 200)
      if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
        yyjson_val* r = yyjson_doc_get_root(d);
        auto str = [&](const char* k) {
          yyjson_val* v = yyjson_obj_get(r, k);
          return yyjson_is_str(v) ? std::string(yyjson_get_str(v)) : std::string();
        };
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
      if (status != 200) return finish(DownloadJob::Failed, status == 0 ? "No connection" : "YouTube didn't give this song");
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
  j->state = DownloadJob::Running, j->percent = 0, j->stage = "Starting";
  g_step = Resolving;
  g_changed = true;
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
    if (status != 200) return finish(DownloadJob::Failed, status == 0 ? "No connection" : "YouTube didn't give this video");
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

}  // namespace

bool joinDownloadParts(const std::string& video, const std::string& audio, const std::string& out) {
  return join(video, audio, out, "", "", "");
}

bool downloadVideo(const Item& item, int quality) {
  if (item.id.empty() || item.id[0] == '/') return false;
  if (!downloadedFile(item.id).empty()) {
    g_app->toast("Already on the card");
    return false;
  }
  for (const DownloadJob& j : g_jobs)
    if (j.id == item.id && (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running)) {
      g_app->toast("Already downloading");
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
  g_changed = true;
  g_app->toast(g_at >= 0 ? "Download waiting: " + j.title : "Downloading " + std::to_string(quality) + "p: " + j.title);
  return true;
}

const std::vector<DownloadJob>& downloadJobs() { return g_jobs; }

bool downloadsActive() {
  for (const DownloadJob& j : g_jobs)
    if (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running) return true;
  return false;
}

void cancelDownload(const std::string& id) {
  for (size_t i = 0; i < g_jobs.size(); i++) {
    DownloadJob& j = g_jobs[i];
    if (j.id != id) continue;
    if (j.state == DownloadJob::Waiting) {
      j.state = DownloadJob::Failed, j.error = "Cancelled";
    } else if (j.state == DownloadJob::Running && int(i) == g_at) {
      if (g_step == Joining) return;  // nearly done: let it finish
      if (g_coreBusy) g_core.cancelDownload();  // it ends as failed: the next waits for that (downloadsTick)
      j.state = DownloadJob::Failed, j.error = "Cancelled";
      removeParts(id);
      g_at = -1, g_step = Idle, g_subs.clear(), g_subsPending = 0;
      g_generation++;
    }
    g_changed = true;
  }
}

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
  DownloadJob* j = current();
  if (!j) {
    CoreState c = g_core.state();
    if (g_coreBusy && (c.downloadSeq != g_seq && c.downloadState != 1)) g_coreBusy = false;  // a cancelled one has ended
    for (size_t i = 0; i < g_jobs.size() && !g_coreBusy; i++)
      if (g_jobs[i].state == DownloadJob::Waiting) {
        if (c.downloadState == 1) break;  // Software update's download first
        start(int(i));
        break;
      }
  } else if (g_step == Picture || g_step == Sound) {
    CoreState c = g_core.state();
    if (c.downloadSeq == g_seq) return false;  // the core hasn't begun this part yet
    int64_t total = g_videoBytes + g_audioBytes;
    int64_t done = (g_step == Sound ? g_videoBytes : 0) + c.downloadDone;
    int pct = total > 0 ? int(std::min<int64_t>(99, done * 100 / total)) : 0;
    if (pct != j->percent) j->percent = pct, g_changed = true;
    if (c.downloadState == 2) {
      g_coreBusy = false;
      if (j->song) startCover();
      else if (g_step == Picture && g_audioItag) startPart(Sound);
      else startCaptions();
    } else if (c.downloadState == 3) {
      g_coreBusy = false;
      finish(DownloadJob::Failed, c.downloadStatus == 0 ? "The connection was lost" : "The stream was refused (" + std::to_string(c.downloadStatus) + ")");
    }
  } else if (g_step == Cover) {  // a song's cover: without one it's saved all the same
    CoreState c = g_core.state();
    if (c.downloadSeq != g_seq && (c.downloadState == 2 || c.downloadState == 3)) {
      g_coreBusy = false;
      startWords();
    }
  } else if (g_step == Joining && g_joined != 0 && j->song) {
    if (g_joiner.joinable()) g_joiner.join();
    std::string part = songParts(j->id) + ".saving.m4a", file = songFile(g_song);
    mkdirs(file.substr(0, file.rfind('/')));
    if (g_joined == 1 && rename(part.c_str(), file.c_str()) == 0) {
      removeParts(j->id);
      readSongs();
      g_songs[j->id] = {file, g_song.title, g_song.artist, g_song.album, int64_t(time(nullptr))};
      writeSongs();
      j->file = file, j->percent = 100;
      std::string title = j->title;
      finish(DownloadJob::Done);
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
  bool c = g_changed;
  g_changed = false;
  return c;
}

// ---- songs ----

bool downloadSong(const Item& item, bool quiet) {
  if (item.id.empty() || item.id[0] == '/') return false;
  if (!downloadedSong(item.id).empty()) {
    if (!quiet) g_app->toast("Already on the card");
    return false;
  }
  for (const DownloadJob& j : g_jobs)
    if (j.id == item.id && (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running)) {
      if (!quiet) g_app->toast("Already downloading");
      return false;
    }
  g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [&](const DownloadJob& j) { return j.id == item.id; }), g_jobs.end());
  if (g_at >= int(g_jobs.size())) g_at = -1;
  for (size_t i = 0; i < g_jobs.size(); i++)
    if (g_jobs[i].state == DownloadJob::Running) g_at = int(i);
  DownloadJob j;
  j.id = item.id, j.title = item.title.empty() ? item.id : item.title, j.thumb = item.thumb, j.song = true;
  j.channel = item.subtitle;  // "Artist · Album": the song's details when /music/song can't say
  g_jobs.push_back(j);
  g_changed = true;
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
  // its album's and artist's folders when that was their last song
  std::string d = file.substr(0, file.rfind('/'));
  for (int k = 0; k < 2 && d.size() > dir.size() && rmdir(d.c_str()) == 0; k++) d = d.substr(0, d.rfind('/'));
  g_changed = true;
  return true;
}

std::string downloadsJson() {
  std::string s = "{\"dir\":" + jstr(downloadDir()) + ",\"jobs\":[";
  int n = 0;
  for (const DownloadJob& j : g_jobs) {
    const char* st = j.state == DownloadJob::Waiting ? "waiting" : j.state == DownloadJob::Running ? "running"
                     : j.state == DownloadJob::Done  ? "done"
                                                     : "failed";
    s += std::string(n++ ? "," : "") + "{\"id\":" + jstr(j.id) + ",\"kind\":\"" + (j.song ? "song" : "video") + "\",\"title\":" + jstr(j.title) + ",\"channel\":" + jstr(j.channel) +
         ",\"quality\":" + std::to_string(j.quality) + ",\"state\":\"" + st + "\",\"percent\":" + std::to_string(j.percent) +
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
  for (const Item& it : downloadedSongs()) {
    std::string id;
    for (auto& [k, m] : g_songs)
      if (m.file == it.id) id = k;
    s += std::string(n++ ? "," : "") + "{\"file\":" + jstr(it.id) + ",\"id\":" + jstr(id) + ",\"title\":" + jstr(it.title) +
         ",\"subtitle\":" + jstr(it.subtitle) + "}";
  }
  return s + "]}";
}

}  // namespace rondo::shell
