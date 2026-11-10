// A still from a video on the card, for its artwork (1.0.x: MediaMetadataRetriever.getFrameAtTime).
#pragma once
#include <stdint.h>

#include "../ui/image.h"

namespace rondo {

// The frame nearest before atUs (a key frame), as RGBA at the video's display size: 1, 0 without one,
// -1 when it couldn't be read this time (mediaserver's answer couldn't be mapped): ask again later.
int grabVideoFrame(const char* path, int64_t atUs, ui::Pixels& out);
// Opens binder on the calling thread: call it on the process's main thread, after any fork, before a
// grab. This kernel's binder gives file descriptors (a frame's memory, a video's picture buffers) to a
// process through the thread that opened it; opened by a grab's own thread, which ends, every later
// answer carrying one failed ("no unused fd available", Nov 10: only the first frame ever came).
void prepareVideoFrames();

}  // namespace rondo
