// A still from a video on the card, for its artwork (1.0.x: MediaMetadataRetriever.getFrameAtTime).
#pragma once
#include <stdint.h>

#include "../ui/image.h"

namespace rondo {

// The frame nearest before atUs (a key frame), as RGBA at the video's display size; false without one.
bool grabVideoFrame(const char* path, int64_t atUs, ui::Pixels& out);

}  // namespace rondo
