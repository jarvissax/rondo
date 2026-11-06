// Embedded cover pictures in music files: FLAC PICTURE blocks, MP4 `covr` atoms and ID3v2 APIC frames.
// Reads only the parts of the file it needs.
#pragma once
#include <stdint.h>

#include <vector>

namespace rondo {

// The front cover (or, without one, the first picture) as its encoded bytes (JPEG or PNG).
bool readCover(const char* path, std::vector<uint8_t>& out);

}  // namespace rondo
