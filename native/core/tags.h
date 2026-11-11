// Tags and durations read straight from the files (FLAC, MP4/M4A, MP3 with ID3v2, Ogg Vorbis/Opus,
// WAV), the fields today's Rondo takes from MediaMetadataRetriever. The other formats' (WMA, APE,
// WavPack, AIFF, DSD...) come from a fallback reader (FFmpeg's, player/probe.h).
#pragma once
#include <string>

namespace rondo {

struct Tags {
  std::string title, artist, albumArtist, album, genre;
  int track = 0;
  int disc = 0;  // 0: not set
  int year = 0;  // 0: not set
  int durationSec = -1;  // -1: unknown
  bool read = false;     // the file was understood
};

Tags readTags(const char* path);

// what reads the rest: called when the file wasn't understood here, or left a title or the length out
using TagsReader = bool (*)(const char* path, Tags& t);
void setFallbackTagsReader(TagsReader reader);  // at startup, before any scan

}  // namespace rondo
