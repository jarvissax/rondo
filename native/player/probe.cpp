// probe.h: FFmpeg's demuxers read the tags, the length and the cover of the formats core/tags.cpp and
// core/cover.cpp don't parse themselves.
#include "probe.h"

#include <stdlib.h>

#include <initializer_list>

extern "C" {
#include <libavformat/avformat.h>
}

namespace rondo {
namespace {

std::string trimmed(const char* v) {
  std::string s = v ? v : "";
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// The header only: a quarter of a megabyte is plenty, and nothing is decoded
AVFormatContext* open(const char* path) {
  AVFormatContext* fmt = nullptr;
  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "probesize", "262144", 0);
  int r = avformat_open_input(&fmt, path, nullptr, &opts);
  av_dict_free(&opts);
  return r < 0 ? nullptr : fmt;
}

}  // namespace

bool probeTags(const char* path, Tags& t) {
  AVFormatContext* fmt = open(path);
  if (!fmt) return false;
  int audio = -1;
  for (unsigned i = 0; i < fmt->nb_streams; i++)
    if (audio < 0 && fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio = int(i);
  if (audio < 0) {
    avformat_close_input(&fmt);
    return false;
  }
  // the container's tags, else the audio stream's (Ogg's, some Matroska files'); FFmpeg names them
  // alike for every format (ASF's WM/AlbumTitle is "album", APEv2's "Album Artist" stays as it is)
  auto get = [&](std::initializer_list<const char*> keys) -> std::string {
    for (const char* k : keys) {
      for (AVDictionary* d : {fmt->metadata, fmt->streams[audio]->metadata}) {
        AVDictionaryEntry* e = d ? av_dict_get(d, k, nullptr, 0) : nullptr;
        std::string v = e ? trimmed(e->value) : "";
        if (!v.empty()) return v;
      }
    }
    return "";
  };
  auto fill = [](std::string& field, const std::string& v) {
    if (field.empty()) field = v;
  };
  fill(t.title, get({"title"}));
  fill(t.artist, get({"artist", "author", "performer"}));
  fill(t.albumArtist, get({"album_artist", "album artist", "albumartist"}));
  fill(t.album, get({"album"}));
  fill(t.genre, get({"genre"}));
  if (!t.track) t.track = atoi(get({"track", "tracknumber"}).c_str());  // "3/12": 3
  if (!t.disc) t.disc = atoi(get({"disc", "discnumber"}).c_str());
  if (!t.year) {
    int y = atoi(get({"date", "year", "originaldate"}).c_str());
    if (y >= 1000 && y <= 2999) t.year = y;
  }
  if (t.durationSec < 0) {
    // a raw stream (AC-3, DTS, AMR) says no length in its header: estimated from its bitrate
    if (fmt->duration == AV_NOPTS_VALUE || fmt->duration <= 0) {
      for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (int(i) != audio) fmt->streams[i]->discard = AVDISCARD_ALL;
      avformat_find_stream_info(fmt, nullptr);
    }
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) t.durationSec = int(fmt->duration / AV_TIME_BASE);
  }
  t.read = true;
  avformat_close_input(&fmt);
  return true;
}

bool probeCover(const char* path, std::vector<uint8_t>& out) {
  AVFormatContext* fmt = open(path);
  if (!fmt) return false;
  bool ok = false;
  // a picture in the file is a stream of its own with the picture as its one packet (APEv2's cover art,
  // ASF's WM/Picture, Matroska's attachments, ID3 in AIFF, WAV and DSF)
  for (unsigned i = 0; i < fmt->nb_streams && !ok; i++) {
    AVStream* s = fmt->streams[i];
    if (!(s->disposition & AV_DISPOSITION_ATTACHED_PIC) || s->attached_pic.size <= 0) continue;
    AVCodecID c = s->codecpar->codec_id;
    if (c != AV_CODEC_ID_MJPEG && c != AV_CODEC_ID_PNG) continue;  // the pictures Rondo decodes
    out.assign(s->attached_pic.data, s->attached_pic.data + s->attached_pic.size);
    ok = true;
  }
  avformat_close_input(&fmt);
  return ok;
}

}  // namespace rondo
