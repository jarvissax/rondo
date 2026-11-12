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

#include "app.h"
#include "file_ops.h"

namespace rondo::shell {

std::string downloadDir() { return std::string(files::kRoots[0]) + "/Videos/YouTube"; }

namespace {

std::vector<DownloadJob> g_jobs;
int g_at = -1;  // the job being done
enum Step { Idle, Resolving, Picture, Sound, Captions, Joining } g_step = Idle;
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
void removeParts(const std::string& id) {
  for (const char* x : {".video", ".video.part", ".audio", ".audio.part", ".mp4.part"}) unlink((partsBase(id) + x).c_str());
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
          const std::string& channel, const std::string& id) {
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
    av_dict_set(&o->metadata, "title", title.c_str(), 0);
    if (!channel.empty()) av_dict_set(&o->metadata, "artist", channel.c_str(), 0);
    av_dict_set(&o->metadata, "comment", ("https://www.youtube.com/watch?v=" + id).c_str(), 0);
    if (avio_open(&o->pb, out.c_str(), AVIO_FLAG_WRITE) < 0) break;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
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
  j->stage = step == Picture ? (g_audioItag ? "Downloading video" : "Downloading") : "Downloading sound";
  g_changed = true;
  g_coreBusy = true;
  g_seq = g_core.state().downloadSeq;
  g_core.download("/s/" + Api::escape(j->id) + "/" + std::to_string(itag), partsBase(j->id) + (step == Picture ? ".video" : ".audio"));
}

void start(int at) {
  g_at = at;
  DownloadJob* j = current();
  j->state = DownloadJob::Running, j->percent = 0, j->stage = "Starting";
  g_step = Resolving;
  g_changed = true;
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
      if (g_step == Picture && g_audioItag) startPart(Sound);
      else startCaptions();
    } else if (c.downloadState == 3) {
      g_coreBusy = false;
      finish(DownloadJob::Failed, c.downloadStatus == 0 ? "The connection was lost" : "The stream was refused (" + std::to_string(c.downloadStatus) + ")");
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

std::string downloadsJson() {
  std::string s = "{\"dir\":" + jstr(downloadDir()) + ",\"jobs\":[";
  int n = 0;
  for (const DownloadJob& j : g_jobs) {
    const char* st = j.state == DownloadJob::Waiting ? "waiting" : j.state == DownloadJob::Running ? "running"
                     : j.state == DownloadJob::Done  ? "done"
                                                     : "failed";
    s += std::string(n++ ? "," : "") + "{\"id\":" + jstr(j.id) + ",\"title\":" + jstr(j.title) + ",\"channel\":" + jstr(j.channel) +
         ",\"quality\":" + std::to_string(j.quality) + ",\"state\":\"" + st + "\",\"percent\":" + std::to_string(j.percent) +
         ",\"stage\":" + jstr(j.stage) + ",\"error\":" + jstr(j.error) + ",\"file\":" + jstr(j.file) + "}";
  }
  s += "],\"files\":[";
  n = 0;
  for (const Item& it : downloadedVideos())
    s += std::string(n++ ? "," : "") + "{\"file\":" + jstr(it.id) + ",\"id\":" + jstr(downloadIdOf(it.id)) + ",\"title\":" + jstr(it.title) +
         ",\"subtitle\":" + jstr(it.subtitle) + "}";
  return s + "]}";
}

}  // namespace rondo::shell
