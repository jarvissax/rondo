// Tags and durations read straight from the files (FLAC, MP4/M4A, MP3 with ID3v2, Ogg Vorbis/Opus,
// WAV), the fields today's Rondo takes from MediaMetadataRetriever.
#pragma once
#include <string>

namespace rondo {

struct Tags {
  std::string title, artist, albumArtist, album;
  int track = 0;
  int durationSec = -1;  // -1: unknown
  bool read = false;     // the file was understood
};

Tags readTags(const char* path);

}  // namespace rondo
