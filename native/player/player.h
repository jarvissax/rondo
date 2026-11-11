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

#include "dsp.h"

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
  int32_t volumeLimit;        // the highest step allowed (Settings › Sound › Volume limit), ≤ volumeMax
  uint32_t trackSerial;  // changes whenever the track does
  int32_t ended;         // the queue played to its end
  int32_t nextIndex;     // the queue item up next (-1: none), by the order and repeat
  int32_t video;         // a video plays: the queue is its sound, its picture is on "rondo.video"
  int32_t videoHeight;   // the picture's height (0 until known)
  int64_t bufferedMs;    // how far the video's picture has been read
  int32_t videoFailed;   // the video's picture can't be shown (its sound may still play)
  int32_t failed;        // a streamed song stopped where it couldn't be read: Play goes on from there
  int32_t loudnessKnown, loudnessTotal;  // Volume leveling: the library's songs measured, of all
  int32_t amplifier;  // the Y1's amplifier: -1 not known, else its level 0..3 (amplifier.h)
  // FM radio (player/fm.h): it plays instead of music (music is paused meanwhile)
  int32_t fmOn, fmStarting, fmMuted, fmSeeking, fmScanning, fmStereo, fmSignal, fmSpeaker;
  int32_t fmError;      // 1: no wired headphones (the antenna), 2: the chip didn't start
  int32_t fmFrequency;  // tenths of MHz
  int32_t fmVolume;     // the radio's own volume step (-1: not set yet)
  uint32_t fmScanSerial;  // moves on when a scan has finished: its stations in fmFound
  uint32_t fmSeekSerial;  // moves on when a seek has finished: its station in fmFrequency
  int32_t fmFoundCount;
  int16_t fmFound[200];
  char fmName[64], fmText[128];  // RDS
  char path[512];
  char error[160];
  char codec[8];  // what the heard track was read as: "Opus", "AAC", "FLAC" … ("" not known yet)
  int32_t order[kMaxQueue];
};

class PlayerHost {
 public:
  // Forks rondo-player: call before any other thread exists. dataDir: where it keeps the volumes;
  // development: a run on 1.0.x's firmware (Android set the volume ranges up)
  bool start(const std::string& dataDir = "/data/rondo", bool development = false);
  bool alive();  // false once rondo-player has died (Rondo restarts: init brings it back)
  // A consistent copy of the state. withOrder: the play order too (16 KB), else it is left out.
  PlayerState state(bool withOrder = false) const;

  void playQueue(const std::vector<std::string>& paths, int start);
  // the queue as Rondo left it (a restart, a crash): the song at `start` from `ms`, playing or paused
  void restoreQueue(const std::vector<std::string>& paths, int start, int64_t ms, bool play);
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
  void setVolumeLimit(int limit);  // Settings › Sound › Volume limit: the highest step, 0-13
  void playback(int crossfadeMs, bool fadePause);  // Settings › Sound › Playback
  void release();                // paused, the song's file closed (the USB drive); Play picks it up again
  void configure(const std::string& baseUrl, const std::string& token);  // where /s/<id>/140 streams come from
  // A video (1.0.x's playVideo): whatever played stops; the sound plays as the queue's one item, the
  // picture goes to the VideoSurface (made before this is called). stopVideo() leaves nothing playing.
  // The pictures are tried in order: the quality asked for, then the one that stands in for it.
  void playVideo(const std::string& soundPath, const std::vector<std::string>& picturePaths);
  void setVideoQuality(const std::vector<std::string>& picturePaths);
  void stopVideo();
  // FM radio (FmRadio.kt): frequencies in tenths of MHz; volume: the radio's own step (-1: music's)
  void fmStart(int frequency, int volume, bool speaker);
  void fmStop();
  void fmTune(int frequency);
  void fmSeek(bool up, int min, int max);
  void fmScan(int min, int max);
  void fmMute(bool muted);
  void fmSpeaker(bool on);
  void click();  // the wheel's click sound
  void sound(const SoundSettings& s);  // Settings › Sound for the output in use
  // Volume leveling's measurements: their file, the library's songs, whether measuring them may run
  void loudnessFile(const std::string& path);
  void analyzeList(const std::vector<std::string>& paths);
  void analyzeAllowed(bool on);
  // the Y1's amplifier, level 0 (as shipped) to 3; backup: where the shipped table is kept
  void amplifier(int level, const std::string& backup);
  void wired(int jack);  // the headphone jack (power::headset()): the audio policy routes to it

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
