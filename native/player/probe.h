// The tags, length and cover of the music formats core/ reads no further than their name (WMA, APE,
// WavPack, Musepack, TTA, TAK, AIFF, DSD, Matroska audio, AC-3, DTS, AMR, CAF, WAV's tags), read by
// FFmpeg's demuxers; main.cpp hands them to core/tags.h and core/cover.h as their fallbacks.
#pragma once
#include <stdint.h>

#include <vector>

#include "../core/tags.h"

namespace rondo {

// what the file says that `t` doesn't have yet (fields already read stay); false: not an audio file
bool probeTags(const char* path, Tags& t);
// an embedded JPEG or PNG
bool probeCover(const char* path, std::vector<uint8_t>& out);

}  // namespace rondo
