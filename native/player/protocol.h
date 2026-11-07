// The messages between the UI and rondo-player: a 32-bit length, a 32-bit type, then the payload.
#pragma once
#include <stdint.h>
#include <unistd.h>

namespace rondo {

enum PlayerCommand : uint32_t {
  kPlayQueue = 1,  // u32 start, then the paths, each ending in a zero byte
  kToggle,
  kPlay,
  kPause,
  kNext,
  kPrevious,
  kSeekTo,  // i64 ms
  kSeekBy,  // i64 ms
  kJumpTo,  // i64 queue index
  kInsertNext,  // the path
  kAppend,      // the path
  kShuffle,     // u8
  kRepeat,      // u8: 0 off, 1 all, 2 one
  kVolume,      // i64 steps
  kRelease,     // the card goes to a computer: close the song's file, keep the place (Play reopens it)
  kConfigure,   // the server streams come from: its base URL, a zero byte, the token
  kPlayVideo,   // a video: the sound's path, a zero byte, the picture's (a /s/ path on the server or a file)
  kVideoQuality,  // the picture's path at another quality, from where the sound is
  kStopVideo,   // the video ends: nothing plays
  kFmStart,     // i32 frequency (tenths of MHz), i32 the radio's volume (-1: music's), u8 speaker
  kFmStop,
  kFmTune,      // i32 frequency
  kFmSeek,      // u8 up, i32 min, i32 max
  kFmScan,      // i32 min, i32 max
  kFmMute,      // u8
  kFmSpeaker,   // u8
};

inline bool readFully(int fd, void* buf, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) return false;
    p += r, n -= size_t(r);
  }
  return true;
}

inline bool writeFully(int fd, const void* buf, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n) {
    ssize_t r = write(fd, p, n);
    if (r <= 0) return false;
    p += r, n -= size_t(r);
  }
  return true;
}

struct PlayerState;
[[noreturn]] void runPlayer(int sock, PlayerState* shm);

}  // namespace rondo
