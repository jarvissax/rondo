// The player (PlayerService and LocalAudioEngine in 1.0.x), natively: a process of its own,
// rondo-player, forked at startup like rondo-media. It decodes with FFmpeg (LGPL, shared libraries),
// resamples to 44.1 kHz stereo and plays through OpenSL ES. The next track is decoded ahead and joined
// in the same stream, so songs follow each other without a gap.
//
// The UI sends commands over a socket; the player publishes its state in shared memory that the UI
// reads every frame without a system call.
//
// 1.0.x's rules: a queue in an order (shuffle puts the current song first and shuffles the rest);
// repeat off, all or one; Previous goes back to the start after 3 s, else to the previous song; a
// seek stops 1 s before the end; Play next and Add to queue; a file that can't be played is skipped.
#pragma once
#include <stdint.h>
#include <sys/types.h>

#include <algorithm>
#include <string>
#include <vector>

namespace rondo {

constexpr int kMaxQueue = 4096;

// Written by the player under a sequence lock: read with PlayerHost::state().
struct PlayerState {
  uint32_t seq;
  int32_t playing;  // 1 while sound comes out (or should: the stream is starting)
  int32_t buffering;
  int32_t pos;    // position in the play order
  int32_t index;  // the queue item playing (-1: none)
  int32_t count;  // queue items
  int32_t shuffle, repeat;  // repeat: 0 off, 1 all, 2 one
  int64_t positionMs, durationMs;
  int64_t publishedNs;  // CLOCK_MONOTONIC when this was written
  int32_t volume, volumeMax;  // STREAM_MUSIC on the current output
  uint32_t trackSerial;  // changes whenever the track does
  int32_t ended;         // the queue played to its end
  int32_t nextIndex;     // the queue item up next (-1: none), by the order and repeat
  char path[512];
  char error[160];
  int32_t order[kMaxQueue];
};

class PlayerHost {
 public:
  // Forks rondo-player: call before any other thread exists.
  bool start();
  // A consistent copy of the state. withOrder: the play order too (16 KB), else it is left out.
  PlayerState state(bool withOrder = false) const;

  void playQueue(const std::vector<std::string>& paths, int start);
  void togglePlay();
  void play();
  void pause();
  void next();
  void previous();
  void seekTo(int64_t ms);
  void seekBy(int64_t deltaMs);
  void jumpTo(int index);
  void insertNext(const std::string& path);
  void append(const std::string& path);
  void setShuffle(bool on);
  void setRepeat(int mode);
  void changeVolume(int delta);  // one step of STREAM_MUSIC (1.0.x's wheel in Now Playing)

  // The position now: positionMs moved on by the time since it was written, while sound comes out.
  static int64_t positionAt(const PlayerState& s, int64_t nowNs) {
    int64_t p = s.positionMs;
    if (s.playing && !s.buffering && !s.ended && s.publishedNs && nowNs > s.publishedNs) p += std::min<int64_t>((nowNs - s.publishedNs) / 1000000, 250);
    return s.durationMs > 0 && p > s.durationMs ? s.durationMs : p;
  }

  bool active() const;  // something is in the queue
  const std::vector<std::string>& queue() const { return queue_; }  // as sent (indexes match state.index)

 private:
  void send(uint32_t type, const void* payload = nullptr, uint32_t len = 0);
  int sock_ = -1;
  PlayerState* shm_ = nullptr;
  pid_t child_ = -1;
  std::vector<std::string> queue_;
};

}  // namespace rondo
