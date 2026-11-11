// rondo-player: the decoding and playing side of player.h, in its own process.
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/replaygain.h>
#include <libswresample/swresample.h>
}

#include "../core/clock.h"
#include "../platform/aosp_begin.h"
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <media/AudioSystem.h>
#include "../platform/permission_service.h"
#include "../platform/aosp_end.h"

#include <curl/curl.h>

#include <memory>

#include "http_source.h"
#include "player.h"
#include "click.h"
#include "dsp.h"
#include "amplifier.h"
#include "loudness.h"
#include "fm.h"
#include "protocol.h"
#include "video.h"

namespace rondo {
namespace {

// A file that says how many channels but not which (WAV, AIFF, CAF without a channel mask): the usual
// layout for that many, so the downmix to stereo knows the center and the surrounds. Always true.
bool knownLayout(AVChannelLayout& l) {
  if (l.order == AV_CHANNEL_ORDER_UNSPEC && l.nb_channels > 0) {
    int n = l.nb_channels;
    av_channel_layout_uninit(&l);
    av_channel_layout_default(&l, n);
  }
  return true;
}

constexpr int kRate = 44100;
constexpr int kBufferFrames = 2048;  // per OpenSL buffer: 46 ms
constexpr int kBuffers = 3;
// 12 s of PCM: a stream rides out a stall of the Wi-Fi (the sound settings work at the output, so they
// follow at once however far ahead the ring is)
constexpr uint32_t kRingFrames = 1u << 19;
constexpr uint32_t kFmRingFrames = 1u << 14;  // the radio's: 0.37 s

int64_t monoNs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

struct Marker {  // where in the stream a track begins (pos -1: the queue's end)
  uint64_t at;
  int pos;
  int64_t durationMs, startMs;
  bool failed = false;  // a stream stopped here (the network, the server): playback stops at startMs of pos
  float gainDb = NAN;   // the track's ReplayGain (Settings › Sound › Volume leveling)
  char codec[8] = {0};  // what it was read as (PlayerState.codec)
};

// The streams a queued song can be read from, best first. The queue names a streamed song by its
// server path, "/s/<id>/140" (1.0.x's AAC), whatever is read. A song: YouTube's Opus (itag 251, about
// 130-160 kb/s VBR, in WebM) and then that AAC (140, 128 kb/s), which a server without Opus (before
// 2.0.0), a song without it or a stream that doesn't open falls back to; a video's sound: the AAC its
// picture was timed against in 1.0.x. tryOpus false: three songs in a row had no Opus (an older
// server), AAC at once. The one place that knows the server's stream paths, so a direct YouTube
// client can give its own URLs here.
std::vector<std::string> streamCandidates(const std::string& path, bool video, bool tryOpus) {
  size_t slash = path.rfind('/');
  std::string base = slash == std::string::npos ? path : path.substr(0, slash + 1);
  if (video || !tryOpus || path.compare(slash + 1, std::string::npos, "140") != 0) return {path};
  return {base + "251", path};
}

class Engine {
 public:
  Engine(int sock, PlayerState* shm) : sock_(sock), shm_(shm) {}
  void setData(const std::string& dir, bool development) { volumesFile_ = dir + "/volumes.txt", development_ = development; }
  [[noreturn]] void main();

 private:
  // ---- the queue (LocalAudioEngine) ----
  int nextPos() const {
    if (repeat_ == 2) return pos_;
    if (pos_ + 1 < int(order_.size())) return pos_ + 1;
    if (repeat_ == 1 && !order_.empty()) return 0;
    return -1;
  }
  int nextAfter(int p) const {
    if (repeat_ == 2) return p;
    if (p + 1 < int(order_.size())) return p + 1;
    if (repeat_ == 1 && !order_.empty()) return 0;
    return -1;
  }
  void rebuildOrder() {
    if (items_.empty()) return;
    int now = pos_ >= 0 && pos_ < int(order_.size()) ? order_[pos_] : 0;
    order_.clear();
    if (shuffle_) {
      order_.push_back(now);
      std::vector<int> rest;
      for (int i = 0; i < int(items_.size()); i++)
        if (i != now) rest.push_back(i);
      std::shuffle(rest.begin(), rest.end(), rng_);
      order_.insert(order_.end(), rest.begin(), rest.end());
      pos_ = 0;
    } else {
      for (int i = 0; i < int(items_.size()); i++) order_.push_back(i);
      pos_ = now;
    }
  }

  // ---- control ----
  void command(const std::vector<uint8_t>& m);
  void restart(int pos, int64_t atMs, bool play);
  void publish();

  // ---- decoding ----
  static void* decodeThread(void* self) {
    setpriority(PRIO_PROCESS, 0, -4);
    static_cast<Engine*>(self)->decodeLoop();
    return nullptr;
  }
  static void* analyzeThread(void* self) {
    setpriority(PRIO_PROCESS, 0, 19);  // this thread only (Linux): behind everything else
    static_cast<Engine*>(self)->analyzeLoop();
    return nullptr;
  }
  void decodeLoop();
  bool decodeTrack(int pos, int64_t seekMs, uint32_t gen);
  bool write(const int16_t* pcm, int frames, uint32_t gen);
  bool eager_ = true;  // the decode thread at -4 (filling), else at +10 (topping up)
  bool released_ = false;  // kRelease: no file open until Play
  std::string base_, token_;  // the server, for streamed songs (kConfigure)
  void* curl_ = nullptr;      // the decoder thread's libcurl handle
  int64_t releasedMs_ = 0;

  // ---- a video's picture, on the sound's clock ----
  int64_t videoClockUs(bool& moving);
  int64_t videoClockLocked(bool& moving) const;  // under mu_
  // the pictures to try, zero-separated: /s/ paths on the server or files (under mu_)
  std::vector<std::string> videoSources(const std::string& list) const {
    std::vector<std::string> out;
    for (size_t at = 0; at < list.size();) {
      size_t end = list.find('\0', at);
      if (end == std::string::npos) end = list.size();
      std::string p = list.substr(at, end - at);
      if (!p.empty()) out.push_back(p.compare(0, 3, "/s/") == 0 ? base_ + p : p);
      at = end + 1;
    }
    return out;
  }
  VideoDecoder video_{[this](bool& moving) { return videoClockUs(moving); }};
  bool videoOn_ = false;
  int64_t videoWaitNs_ = 0;  // since when the sound waits for the picture (8 s at most)
  int64_t restartMs_ = 0;    // where the last restart put the sound: the picture's clock counts on from it

  // ---- FM radio: its sound in a ring of its own, played instead of music's ----
  FmRadio fm_{[this] { fmChanged(); }, [this](const int16_t* pcm, size_t frames) { fmSound(pcm, frames); }};
  void fmChanged();  // from the radio's threads
  void fmSound(const int16_t* pcm, size_t frames);
  void fmVolumes(bool on);  // the radio's own volume while it sounds, music's again after
  void fmSpeakerApply(bool on);
  std::vector<int16_t> fmRing_ = std::vector<int16_t>(size_t(kFmRingFrames) * 2);  // under mu_
  uint64_t fmWrite_ = 0, fmRead_ = 0;
  bool fmSounding_ = false;  // under mu_: what fill() plays
  int fmVolume_ = -1, musicVolume_ = -1;
  bool fmSpeaker_ = false, speakerForced_ = false;
  int videoPasses_ = 0;      // passes of a video's sound decoded since then (more than one: it repeats)
  bool waitingForPicture() const { return videoOn_ && !video_.ready() && monoNs() - videoWaitNs_ < 8000000000LL; }

  // ---- output ----
  bool openOutput();
  static void onBuffer(SLAndroidSimpleBufferQueueItf q, void* self) { static_cast<Engine*>(self)->fill(q); }
  void fill(SLAndroidSimpleBufferQueueItf q);
  void setPlaying(bool on);
  void applyPlayState();
  bool applied_ = false;

  // ---- volume (STREAM_MUSIC, as 1.0.x's AudioManager steps it) ----
  void readVolume();
  void changeVolume(int delta);

  int sock_;
  PlayerState* shm_;
  std::mt19937 rng_{uint32_t(time(nullptr))};

  pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;  // everything below
  pthread_cond_t cv_ = PTHREAD_COND_INITIALIZER;
  std::vector<std::string> items_;
  std::vector<int> order_;
  int pos_ = -1;  // what the decoder works on is decodePos_; what is heard is heardPos_
  bool shuffle_ = false;
  int repeat_ = 0;
  bool playing_ = false;
  uint32_t gen_ = 0;          // a restart: the decoder drops its track, the ring empties
  int startPos_ = -1;         // where the decoder starts after a restart
  int64_t startMs_ = 0;
  int heardPos_ = -1;
  int64_t heardDuration_ = 0, heardStartMs_ = 0;
  uint64_t heardFrom_ = 0;     // stream frame where the heard track began
  bool ended_ = false;
  // A song the server streams that couldn't be read (no connection, the server failing): the player
  // stops where it failed, as 1.0.x's ExoPlayer did, instead of skipping on; Play goes on from there.
  bool failed_ = false;
  bool streamFailed_ = false;  // decodeTrack's answer
  uint64_t trackAt_ = 0;       // where in the stream the track decodeTrack opened begins
  std::string error_;

  // the ring: stream frames [readAt_, writeAt_) are decoded and not yet sent to OpenSL
  std::vector<int16_t> ring_ = std::vector<int16_t>(size_t(kRingFrames) * 2);
  uint64_t writeAt_ = 0, readAt_ = 0, playedAt_ = 0;
  std::deque<Marker> markers_;
  struct Sent {
    uint64_t from, to;  // stream frames in an OpenSL buffer
  };
  std::deque<Sent> inFlight_;
  int64_t lastCallbackNs_ = 0;

  SLObjectItf engineObj_ = nullptr, mixObj_ = nullptr, playerObj_ = nullptr;
  SLPlayItf play_ = nullptr;
  SLAndroidSimpleBufferQueueItf queue_ = nullptr;
  Click click_;
  Dsp dsp_;                  // Settings › Sound, on what goes out (under mu_, in fill())
  float heardGainDb_ = NAN;  // the ReplayGain of the track heard now
  char heardCodec_[8] = {0};
  int opusMisses_ = 0;  // streams in a row with no Opus: three, and AAC is asked for at once
  // Volume leveling's measurements: as songs play (from their start, to their end), and of the
  // library while the Y1 charges (analyzeLoop, a thread at the lowest priority)
  LoudnessCache loudness_;
  int ampState_ = -1;  // the Y1's amplifier: -1 not known, else its level 0..3 (amplifier.h)
  LoudnessMeter meter_;  // the decode thread's
  std::vector<std::string> analyzeList_;
  bool analyzeAllowed_ = false;
  size_t analyzeAt_ = 0;
  int analyzeKnown_ = 0;
  pthread_cond_t analyzeCv_ = PTHREAD_COND_INITIALIZER;
  void analyzeLoop();
  float measureFile(const std::string& path);
  void countKnown() {
    analyzeKnown_ = 0;
    float l;
    for (const std::string& p : analyzeList_) analyzeKnown_ += loudness_.find(p, l);
  }
  int16_t buffers_[kBuffers][kBufferFrames * 2];
  int next_ = 0;
  int volume_ = 0, volumeMax_ = 13;  // MediaTek's STREAM_MUSIC has 13 steps (the policy's index max, Nov 7)
  // Settings › Sound › Volume limit (the iPod's): no step above it, on any output, however it is asked
  // for; kept in <data>/volume-limit so it holds from the first moment after a start
  int volumeLimit_ = 13;
  void setVolumeLimit(int limit);
  // Each output's music volume, kept as Android's AudioService kept it (volume_music_speaker and the
  // rest): without Android the policy forgets it at every boot
  std::string volumesFile_;
  bool development_ = false;
  bool fmOnOutput_ = false;  // the radio's step is on the output (not music's)
  void restoreVolumes();
  void saveVolume(audio_devices_t device, int index);

 public:
  // mediaserver died (AudioSystem's error callback): until the new one has the volume ranges and this
  // Y1's levels again, the output plays silence. A fresh audio policy has one step per stream, so its
  // default step is full volume (Nov 10: vold stopped mediaserver for the USB drive, and the next song
  // played at full volume); AudioTrack reconnects to the new mediaserver by itself and would play at it.
  void audioLost();

 private:
  std::atomic<bool> audioLost_{false};
  std::atomic<bool> recovering_{false};
  void recoverAudio();
};

void Engine::publish() {
  // under mu_
  PlayerState& s = *shm_;
  __atomic_add_fetch(&s.seq, 1, __ATOMIC_ACQ_REL);  // odd: being written
  s.playing = playing_;
  s.buffering = playing_ && (readAt_ == writeAt_ || waitingForPicture()) && !ended_;
  s.pos = heardPos_;
  s.index = heardPos_ >= 0 && heardPos_ < int(order_.size()) ? order_[heardPos_] : -1;
  s.count = int(items_.size());
  s.shuffle = shuffle_;
  s.repeat = repeat_;
  int64_t played = int64_t(playedAt_ - heardFrom_) * 1000 / kRate + heardStartMs_;
  if (playing_ && lastCallbackNs_ && !ended_) played += std::min<int64_t>((monoNs() - lastCallbackNs_) / 1000000, 46);
  s.positionMs = std::max<int64_t>(0, std::min(played, heardDuration_ > 0 ? heardDuration_ : played));
  s.durationMs = heardDuration_;
  s.publishedNs = monoNs();
  s.volume = volume_;
  s.volumeMax = volumeMax_;
  s.volumeLimit = std::min(volumeMax_, volumeLimit_);
  s.ended = ended_;
  int np = heardPos_ >= 0 ? nextAfter(heardPos_) : -1;
  s.nextIndex = np >= 0 && np < int(order_.size()) ? order_[np] : -1;
  s.video = videoOn_;
  s.videoHeight = videoOn_ ? video_.height() : 0;
  s.bufferedMs = videoOn_ ? std::max(s.positionMs, video_.readMs()) : 0;
  s.videoFailed = videoOn_ && video_.failed();
  s.videoFrames = videoOn_ ? video_.rendered() : 0;
  s.failed = failed_;
  s.loudnessKnown = analyzeKnown_, s.loudnessTotal = int32_t(analyzeList_.size());
  s.amplifier = ampState_;
  FmStatus fm = fm_.status();
  s.fmOn = fm.on, s.fmStarting = fm.starting, s.fmMuted = fm.muted, s.fmSeeking = fm.seeking;
  s.fmScanning = fm.scanning, s.fmStereo = fm.stereo, s.fmSignal = fm.signal, s.fmSpeaker = fmSpeaker_;
  s.fmError = fm.error, s.fmFrequency = fm.frequency, s.fmVolume = fmVolume_;
  s.fmScanSerial = fm.scanSerial, s.fmSeekSerial = fm.seekSerial;
  s.fmFoundCount = int32_t(std::min<size_t>(fm.found.size(), 200));
  for (int i = 0; i < s.fmFoundCount; i++) s.fmFound[i] = int16_t(fm.found[size_t(i)]);
  snprintf(s.fmName, sizeof(s.fmName), "%s", fm.name.c_str());
  snprintf(s.fmText, sizeof(s.fmText), "%s", fm.text.c_str());
  if (s.index >= 0) snprintf(s.path, sizeof(s.path), "%s", items_[size_t(s.index)].c_str());
  else s.path[0] = 0;
  snprintf(s.error, sizeof(s.error), "%s", error_.c_str());
  memcpy(s.codec, heardCodec_, sizeof(s.codec));
  size_t n = std::min(order_.size(), size_t(kMaxQueue));
  for (size_t i = 0; i < n; i++) s.order[i] = order_[i];
  __atomic_add_fetch(&s.seq, 1, __ATOMIC_ACQ_REL);  // even: consistent
}

void Engine::restart(int pos, int64_t atMs, bool play) {
  // under mu_: the decoder starts over at `pos`, the stream is dropped
  gen_++;
  startPos_ = pos;
  startMs_ = atMs;
  readAt_ = writeAt_ = playedAt_ = 0;
  markers_.clear();
  // the buffers OpenSL still holds belong to the old stream: silence them, and count them as nothing
  memset(buffers_, 0, sizeof(buffers_));
  for (auto& f : inFlight_) f = {0, 0};
  heardPos_ = pos;
  heardFrom_ = 0;
  heardStartMs_ = atMs;
  heardDuration_ = 0;
  ended_ = false;
  failed_ = false;
  error_.clear();
  pos_ = pos;
  pthread_cond_broadcast(&cv_);
  setPlaying(play && pos >= 0);
  restartMs_ = atMs;
  videoPasses_ = 0;
  if (videoOn_) video_.seek(atMs), videoWaitNs_ = monoNs();  // the picture follows; the sound waits for it
}

// Where the sound is, to the microsecond: the frames handed to OpenSL since the last restart, and the
// part of the buffer it is playing now by the time since it took it. It stops while the ring is empty
// (buffering) and paused, and runs on across the passes of a repeat.
int64_t Engine::videoClockUs(bool& moving) {
  pthread_mutex_lock(&mu_);
  int64_t us = videoClockLocked(moving);
  pthread_mutex_unlock(&mu_);
  return us;
}

int64_t Engine::videoClockLocked(bool& moving) const {
  uint64_t at = playedAt_;
  if (playing_ && !inFlight_.empty() && lastCallbackNs_) {
    const Sent& f = inFlight_.front();
    if (f.to > f.from && f.from == playedAt_)
      at += std::min<uint64_t>(f.to - f.from, uint64_t(std::max<int64_t>(0, monoNs() - lastCallbackNs_)) * kRate / 1000000000ULL);
  }
  moving = playing_ && !ended_;
  return int64_t(at) * 1000000 / kRate + restartMs_ * 1000;
}

void Engine::setPlaying(bool on) { playing_ = on; }  // under mu_; OpenSL follows in applyPlayState()

// OpenSL's play state follows playing_, set outside mu_ (the buffer callback takes mu_)
void Engine::applyPlayState() {
  pthread_mutex_lock(&mu_);
  bool want = playing_ || fmSounding_;
  pthread_mutex_unlock(&mu_);
  if (play_ && want != applied_) {
    (*play_)->SetPlayState(play_, want ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_PAUSED);
    applied_ = want;
  }
}

void Engine::command(const std::vector<uint8_t>& m) {
  if (m.size() < 4) return;
  uint32_t type;
  memcpy(&type, m.data(), 4);
  const uint8_t* p = m.data() + 4;
  size_t len = m.size() - 4;
  auto i64 = [&]() {
    int64_t v = 0;
    if (len >= 8) memcpy(&v, p, 8);
    return v;
  };
  auto text = [&]() { return std::string((const char*)p, len); };
  auto i32at = [&](size_t at) {
    int32_t v = 0;
    if (len >= at + 4) memcpy(&v, p + at, 4);
    return v;
  };
  // the radio's calls report back through the engine's lock: they're made once it is let go
  std::function<void()> radio;
  static const bool logCommands = getenv("RONDO_PLAYER_LOG") != nullptr;
  int64_t asked = logCommands ? monoNs() : 0;
  pthread_mutex_lock(&mu_);
  if (logCommands) printf("player: command %u (the lock after %.1f ms)\n", type, (monoNs() - asked) / 1e6);
  int64_t heard = int64_t(playedAt_ - heardFrom_) * 1000 / kRate + heardStartMs_;
  switch (type) {
    case kPlayQueue: {
      if (videoOn_) videoOn_ = false, video_.stop();
      uint32_t start = 0;
      if (len >= 4) memcpy(&start, p, 4);
      items_.clear();
      for (size_t at = 4; at < len;) {
        size_t end = at;
        while (end < len && p[end]) end++;
        items_.emplace_back((const char*)p + at, end - at);
        at = end + 1;
      }
      order_.clear();
      for (int k = 0; k < int(items_.size()); k++) order_.push_back(k);
      pos_ = items_.empty() ? -1 : std::min<int>(int(start), int(items_.size()) - 1);
      if (shuffle_) rebuildOrder();
      restart(pos_, 0, true);
      break;
    }
    case kRestoreQueue: {  // as kPlayQueue, from where it was, playing or not
      if (len < 13) break;
      uint32_t start = 0;
      int64_t at = 0;
      memcpy(&start, p, 4);
      memcpy(&at, p + 4, 8);
      bool play = p[12] != 0;
      items_.clear();
      for (size_t i = 13; i < len;) {
        size_t end = i;
        while (end < len && p[end]) end++;
        items_.emplace_back((const char*)p + i, end - i);
        i = end + 1;
      }
      order_.clear();
      for (int k = 0; k < int(items_.size()); k++) order_.push_back(k);
      pos_ = items_.empty() ? -1 : std::min<int>(int(start), int(items_.size()) - 1);
      if (shuffle_) rebuildOrder();
      if (pos_ >= 0) restart(pos_, std::max<int64_t>(0, at), play);
      break;
    }
    case kToggle:
    case kPlay:
      if (failed_ && heardPos_ >= 0) {  // a stream that failed: again from where it stopped
        int64_t at = heardStartMs_ + int64_t(playedAt_ - heardFrom_) * 1000 / kRate;
        restart(heardPos_, std::max<int64_t>(0, at), true);
        break;
      }
      if (type == kPlay) {
        if (released_) {
          released_ = false;
          restart(heardPos_, releasedMs_, true);
        } else if (heardPos_ >= 0 && !playing_) {
          setPlaying(true);
        }
        break;
      }
      if (released_) {
        released_ = false;
        restart(heardPos_, releasedMs_, true);
      } else if (heardPos_ >= 0) {
        if (ended_) restart(heardPos_, 0, true);
        else setPlaying(!playing_);
      }
      break;
    case kConfigure: {
      std::string s(reinterpret_cast<const char*>(p), len);
      size_t z = s.find('\0');
      base_ = s.substr(0, z);
      while (!base_.empty() && base_.back() == '/') base_.pop_back();
      token_ = z == std::string::npos ? "" : s.substr(z + 1);
      break;
    }
    case kPlayVideo: {
      std::string s = text();
      size_t z = s.find('\0');
      std::string sound = s.substr(0, z), pictures = z == std::string::npos ? "" : s.substr(z + 1);
      videoOn_ = false;
      video_.stop();
      items_ = {sound};
      order_ = {0};
      pos_ = 0;
      restart(0, 0, true);
      videoOn_ = true;
      video_.play(videoSources(pictures), token_, 0);
      video_.setLooping(repeat_ != 0);  // one item: Repeat all or one plays it again
      videoWaitNs_ = monoNs();
      break;
    }
    case kVideoQuality:
      if (videoOn_) {
        // from where the sound is in this pass; the pass began this long ago on the clock
        bool moving;
        int64_t clock = videoClockLocked(moving);
        video_.play(videoSources(text()), token_, heard, false, std::max<int64_t>(0, clock - heard * 1000));
        video_.setLooping(repeat_ != 0);
      }
      break;
    case kStopVideo:
      if (videoOn_) {
        videoOn_ = false;
        video_.stop();
        items_.clear();
        order_.clear();
        pos_ = -1;
        restart(-1, 0, false);
      }
      break;
    case kFmStart: {
      int32_t f = i32at(0), v = i32at(4);
      setPlaying(false);  // opening the radio pauses music
      if (v >= 0) fmVolume_ = v;
      fmSpeaker_ = len > 8 && p[8];
      radio = [this, f] { fm_.start(f); };
      break;
    }
    case kFmStop: radio = [this] { fm_.stop(); }; break;
    case kFmTune: {
      int32_t f = i32at(0);
      radio = [this, f] { fm_.tune(f); };
      break;
    }
    case kFmSeek: {
      bool up = len && p[0];
      int32_t a = i32at(1), z = i32at(5);
      radio = [this, up, a, z] { fm_.seek(up, a, z); };
      break;
    }
    case kFmScan: {
      int32_t a = i32at(0), z = i32at(4);
      radio = [this, a, z] { fm_.scan(a, z); };
      break;
    }
    case kFmMute: {
      bool m = len && p[0];
      radio = [this, m] { fm_.setMuted(m); };
      break;
    }
    case kWired: {  // the jack: the policy learns of a wired headset (0x4) or headphones (0x8)
      int jack = len ? p[0] : 0;
      radio = [jack] {
        using android::AudioSystem;
        for (audio_devices_t d : {AUDIO_DEVICE_OUT_WIRED_HEADSET, AUDIO_DEVICE_OUT_WIRED_HEADPHONE}) {
          bool on = (jack == 1 && d == AUDIO_DEVICE_OUT_WIRED_HEADSET) || (jack == 2 && d == AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
          AudioSystem::setDeviceConnectionState(d, on ? AUDIO_POLICY_DEVICE_STATE_AVAILABLE : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE, "");
        }
        if (getenv("RONDO_PLAYER_LOG")) printf("player: the jack: %s\n", jack == 1 ? "a headset" : jack == 2 ? "headphones" : "nothing");
      };
      break;
    }
    case kAmplifier: {  // u8 level, then where the shipped table is kept: NVRAM, outside the lock
      int lvl = len ? p[0] : 0;
      std::string backup = len > 1 ? std::string(reinterpret_cast<const char*>(p + 1), len - 1) : "";
      radio = [this, lvl, backup] {
        std::string why;
        bool ok = !backup.empty() && amp::set(lvl, backup, why);
        if (!ok) fprintf(stderr, "rondo-player: amplifier not changed: %s\n", why.c_str());
        std::vector<uint8_t> t;
        int state = amp::read(t) ? amp::level(t) : -1;
        pthread_mutex_lock(&mu_);
        ampState_ = state;
        publish();
        pthread_mutex_unlock(&mu_);
      };
      break;
    }
    case kLoudness: {  // the measurements' file
      loudness_.open(std::string(reinterpret_cast<const char*>(p), len));
      countKnown();
      break;
    }
    case kAnalyzeList: {  // the library's songs, a line each
      analyzeList_.clear();
      std::string all(reinterpret_cast<const char*>(p), len);
      for (size_t at = 0; at < all.size();) {
        size_t nl = all.find('\n', at);
        if (nl == std::string::npos) nl = all.size();
        if (nl > at) analyzeList_.push_back(all.substr(at, nl - at));
        at = nl + 1;
      }
      analyzeAt_ = 0;
      countKnown();
      pthread_cond_broadcast(&analyzeCv_);
      break;
    }
    case kAnalyzeAllowed:
      analyzeAllowed_ = len && p[0];
      pthread_cond_broadcast(&analyzeCv_);
      break;
    case kSound: {
      SoundSettings snd;
      if (decodeSound(p, len, snd)) {
        dsp_.configure(snd);
        if (getenv("RONDO_PLAYER_LOG"))
          printf("player: sound %s, %zu filters, gain %.1f dB, leveling %d\n", snd.active() ? "on" : "off", snd.filters.size(),
                 snd.gainDb, snd.leveling);
      }
      break;
    }
    case kClick: {
      Click* c = &click_;
      radio = [c] { c->play(); };  // outside the lock, as the radio's calls
      break;
    }
    case kFmSpeaker: {
      fmSpeaker_ = len && p[0];
      bool on = fmSpeaker_ && fmSounding_;
      radio = [this, on] { fmSpeakerApply(on); };
      break;
    }
    case kRelease:
      // The card goes to a computer as a USB drive, and vold kills whoever still has a file open on it:
      // the decoder leaves the song (closing it), the stream is dropped, the place is kept.
      if (videoOn_) videoOn_ = false, video_.stop();
      if (heardPos_ >= 0 && !released_) {
        releasedMs_ = heard;
        int64_t duration = heardDuration_;
        restart(heardPos_, releasedMs_, false);
        startPos_ = -1;  // nothing decodes until Play
        heardDuration_ = duration;
        released_ = true;
      }
      break;
    case kPause: setPlaying(false); break;
    case kNext: {
      int n = heardPos_ + 1 < int(order_.size()) ? heardPos_ + 1 : repeat_ == 1 && !order_.empty() ? 0 : -1;
      if (n >= 0) restart(n, 0, true);
      break;
    }
    case kPrevious:
      if (heard > 3000 || heardPos_ <= 0) restart(heardPos_, 0, playing_);
      else restart(heardPos_ - 1, 0, true);
      break;
    case kSeekTo:
    case kSeekBy: {
      int64_t target = type == kSeekTo ? i64() : heard + i64();
      int64_t d = heardDuration_;
      target = d > 0 ? std::max<int64_t>(0, std::min(target, std::max<int64_t>(0, d - 1000))) : std::max<int64_t>(0, target);
      if (heardPos_ >= 0) restart(heardPos_, target, playing_);
      break;
    }
    case kJumpTo: {
      int32_t index = int32_t(i64());
      auto it = std::find(order_.begin(), order_.end(), index);
      if (it != order_.end()) restart(int(it - order_.begin()), 0, true);
      break;
    }
    case kInsertNext:
    case kAppend: {
      std::string path = text();
      if (items_.empty()) {
        items_ = {path};
        order_ = {0};
        restart(0, 0, true);
        break;
      }
      items_.push_back(path);
      int k = int(items_.size()) - 1;
      if (type == kInsertNext) order_.insert(order_.begin() + heardPos_ + 1, k);
      else order_.push_back(k);
      // In a song's last seconds the next one (or the queue's end) is already in the stream: decode
      // again from where it is heard, so the song just queued is the one that follows (1.0.x's
      // prepareNext()).
      // (Add to queue changes what follows only when the queue's end was next.)
      bool endNext = std::any_of(markers_.begin(), markers_.end(), [](const Marker& m) { return m.pos < 0; });
      if (heardPos_ >= 0 && (type == kInsertNext ? !markers_.empty() : endNext)) restart(heardPos_, heard, playing_);
      break;
    }
    case kShuffle:
      shuffle_ = len && p[0];
      pos_ = heardPos_;
      rebuildOrder();
      heardPos_ = pos_;
      // what is decoded ahead may now be the wrong song: decode again from where it is heard
      if (heardPos_ >= 0) restart(heardPos_, heard, playing_);
      break;
    case kRepeat: {
      repeat_ = len ? p[0] : 0;
      // A video (one item, decoded once) only needs decoding again when its end is already queued;
      // Shorts turn Repeat on as they start.
      bool endQueued = std::any_of(markers_.begin(), markers_.end(), [](const Marker& m) { return m.pos < 0; });
      if (videoOn_) video_.setLooping(repeat_ != 0);
      if (heardPos_ >= 0 && (!videoOn_ || endQueued)) restart(heardPos_, heard, playing_);
      break;
    }
    case kVolume: {
      int32_t delta = int32_t(i64());
      pthread_mutex_unlock(&mu_);
      changeVolume(delta);
      pthread_mutex_lock(&mu_);
      break;
    }
    case kVolumeLimit: {
      int32_t limit = i32at(0);
      pthread_mutex_unlock(&mu_);
      setVolumeLimit(limit);
      pthread_mutex_lock(&mu_);
      publish();
      break;
    }
  }
  // music started: the radio stops (PlayerService.changed in 1.0.x)
  if (playing_ && fm_.active() && type != kFmStart) radio = [this] { fm_.stop(); };
  publish();
  pthread_mutex_unlock(&mu_);
  if (radio) radio();
  applyPlayState();
}

void Engine::fmSound(const int16_t* pcm, size_t frames) {
  pthread_mutex_lock(&mu_);
  for (size_t k = 0; k < frames; k++) {
    size_t at = size_t((fmWrite_ + k) % kFmRingFrames) * 2;
    fmRing_[at] = pcm[k * 2];
    fmRing_[at + 1] = pcm[k * 2 + 1];
  }
  fmWrite_ += frames;
  // the recording and the output run on the same clock; at most 0.19 s behind, or it catches up
  if (fmWrite_ - fmRead_ > kFmRingFrames / 2) fmRead_ = fmWrite_ - kFmRingFrames / 4;
  pthread_mutex_unlock(&mu_);
}

void Engine::fmChanged() {
  bool sounding = fm_.sounding();
  pthread_mutex_lock(&mu_);
  bool switched = sounding != fmSounding_;
  if (switched) {
    fmSounding_ = sounding;
    fmRead_ = fmWrite_;
    if (sounding) {  // what OpenSL still holds of music isn't heard now
      memset(buffers_, 0, sizeof(buffers_));
      for (auto& f : inFlight_) f = {readAt_, readAt_};
    }
  }
  publish();
  pthread_mutex_unlock(&mu_);
  if (switched) fmVolumes(sounding);
  applyPlayState();
}

// STREAM_FM had its own volume in 1.0.x (the wheel set it while the radio played): the radio's step is
// put on the output while it sounds, and music's back after it
void Engine::fmVolumes(bool on) {
  using android::AudioSystem;
  audio_devices_t device = AudioSystem::getDevicesForStream(AUDIO_STREAM_MUSIC);
  readVolume();
  fmOnOutput_ = on;
  if (on) {
    musicVolume_ = volume_;
    if (fmVolume_ < 0) fmVolume_ = volume_;
    if (fmVolume_ > volumeLimit_) fmVolume_ = volumeLimit_;
    if (AudioSystem::setStreamVolumeIndex(AUDIO_STREAM_MUSIC, fmVolume_, device) == 0) volume_ = fmVolume_;
  } else {
    fmVolume_ = volume_;
    if (musicVolume_ >= 0 && AudioSystem::setStreamVolumeIndex(AUDIO_STREAM_MUSIC, musicVolume_, device) == 0) volume_ = musicVolume_;
  }
  fmSpeakerApply(on && fmSpeaker_);
  pthread_mutex_lock(&mu_);
  publish();
  pthread_mutex_unlock(&mu_);
}

// FmRadio.setSpeaker(): the radio out of the speaker (1.0.x's FM force-use; Rondo's radio is music's
// output, so media's)
void Engine::fmSpeakerApply(bool on) {
  // the policy's own setting, not a copy: rondo-core clears it when Bluetooth audio connects
  speakerForced_ = android::AudioSystem::getForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA) == AUDIO_POLICY_FORCE_SPEAKER;
  if (on == speakerForced_) return;
  android::AudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA, on ? AUDIO_POLICY_FORCE_SPEAKER : AUDIO_POLICY_FORCE_NONE);
  speakerForced_ = on;
}

// ---- decoding ----

bool Engine::write(const int16_t* pcm, int frames, uint32_t gen) {
  // under mu_ (released while waiting for room)
  while (frames > 0) {
    if (gen != gen_) return false;
    uint64_t room = kRingFrames - (writeAt_ - readAt_);
    if (room == 0) {
      waitUntil(&cv_, &mu_, now_ns() + 50 * kMs);
      continue;
    }
    int n = int(std::min<uint64_t>(room, uint64_t(frames)));
    for (int k = 0; k < n; k++) {
      size_t at = size_t((writeAt_ + uint64_t(k)) % kRingFrames) * 2;
      ring_[at] = pcm[k * 2];
      ring_[at + 1] = pcm[k * 2 + 1];
    }
    writeAt_ += uint64_t(n);
    pcm += n * 2;
    frames -= n;
  }
  // Filling the whole ring at once, right as a song starts (and Now Playing slides in), took the CPU
  // from the UI for a refresh. With a second buffered the decoder steps back below the UI's priority,
  // and comes forward again under half a second.
  uint64_t buffered = writeAt_ - readAt_;
  if (eager_ && buffered >= uint64_t(kRate)) {
    setpriority(PRIO_PROCESS, 0, 10);
    eager_ = false;
  } else if (!eager_ && buffered < uint64_t(kRate / 2)) {
    setpriority(PRIO_PROCESS, 0, -4);
    eager_ = true;
  }
  return true;
}

// Decodes one track into the ring; false when a restart interrupted it.
bool Engine::decodeTrack(int pos, int64_t seekMs, uint32_t gen) {
  std::string path = items_[size_t(order_[size_t(pos)])];
  bool video = videoOn_;
  pthread_mutex_unlock(&mu_);
  AVFormatContext* fmt = nullptr;
  AVCodecContext* dec = nullptr;
  SwrContext* swr = nullptr;
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool ok = false;
  int stream = -1;
  const AVCodec* codec = nullptr;
  // a song the server streams: read through HttpSource, from the first of its streams that opens; a file
  // on the card: FFmpeg's own file input
  bool streamed = path.compare(0, 3, "/s/") == 0;
  std::vector<std::string> tries{path};
  if (streamed) tries = streamCandidates(path, video, opusMisses_ < 3);
  std::unique_ptr<HttpSource> source;
  for (size_t t = 0; t < tries.size() && !ok && gen == gen_; t++) {
    if (t > 0) {  // the one before didn't open: closed, the next tried
      swr_free(&swr);
      avcodec_free_context(&dec);
      avformat_close_input(&fmt);
      source.reset();
      stream = -1, codec = nullptr;
    }
    // A song's cover is a video stream FFmpeg can't decode (no picture decoders are built), and finding
    // its size made avformat_find_stream_info read up to 5 MB: half a megabyte is enough for every audio
    // format.
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "524288", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);
    if (streamed) {
      if (!curl_) curl_ = curl_easy_init();  // one handle for every song: its connection stays open
      pthread_mutex_lock(&mu_);
      std::string url = base_ + tries[t], token = token_;
      pthread_mutex_unlock(&mu_);
      source.reset(new HttpSource(curl_, url, token, [this, gen] { return gen != gen_; }));
      fmt = avformat_alloc_context();
      fmt->pb = source->avio();
      fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    int opened = avformat_open_input(&fmt, streamed ? "" : path.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (opened == 0)
      for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) fmt->streams[i]->discard = AVDISCARD_ALL;
    if (opened == 0 && avformat_find_stream_info(fmt, nullptr) >= 0 &&
        (stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0)) >= 0 && codec) {
      dec = avcodec_alloc_context3(codec);
      AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
      if (avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) >= 0 && avcodec_open2(dec, codec, nullptr) == 0 &&
          knownLayout(dec->ch_layout) && swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, kRate, &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0,
                              nullptr) == 0 &&
          swr_init(swr) == 0)
        ok = true;
    }
    if (streamed && tries.size() > 1 && t == 0) opusMisses_ = ok ? 0 : opusMisses_ + 1;
    if (!ok && t + 1 < tries.size()) fprintf(stderr, "rondo-player: %s didn't open: %s next\n", tries[t].c_str(), tries[t + 1].c_str());
  }
  const char* codecName = !ok ? "" : codec->id == AV_CODEC_ID_OPUS ? "Opus" : codec->id == AV_CODEC_ID_AAC ? "AAC" : "";
  pthread_mutex_lock(&mu_);
  if (!ok) {
    error_ = streamed ? "Can't play this song" : "Can't play " + path.substr(path.rfind('/') + 1);
    fprintf(stderr, "rondo-player: %s\n", error_.c_str());
  }
  if (ok) error_.clear();
  if (ok && gen == gen_) {
    int64_t duration = fmt->duration > 0 ? fmt->duration / 1000 : 0;
    // ReplayGain: FFmpeg's reading of the tags (ID3 TXXX, Vorbis comments, APE), else the tag itself
    float rg = NAN;
    const AVCodecParameters* cp = fmt->streams[stream]->codecpar;
    const AVPacketSideData* sd = av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data, AV_PKT_DATA_REPLAYGAIN);
    if (sd && sd->size >= sizeof(AVReplayGain)) {
      const AVReplayGain* r = reinterpret_cast<const AVReplayGain*>(sd->data);
      if (r->track_gain != INT32_MIN) rg = float(r->track_gain) / 100000.f;
      else if (r->album_gain != INT32_MIN) rg = float(r->album_gain) / 100000.f;
    }
    if (std::isnan(rg))
      for (AVDictionary* d : {fmt->metadata, fmt->streams[stream]->metadata})
        if (const AVDictionaryEntry* e = av_dict_get(d, "REPLAYGAIN_TRACK_GAIN", nullptr, 0)) {
          rg = float(atof(e->value));
          break;
        }
    // no tags: the loudness measured when it played before (ReplayGain's reference is -18 LUFS)
    float lufs;
    if (std::isnan(rg) && loudness_.find(path, lufs)) rg = -18.f - lufs;
    if (getenv("RONDO_PLAYER_LOG")) printf("player: track %d, ReplayGain %s%.2f dB\n", pos, std::isnan(rg) ? "none " : "", std::isnan(rg) ? 0.f : rg);
    uint64_t at = writeAt_;
    markers_.push_back({at, pos, duration, seekMs, false, rg});
    snprintf(markers_.back().codec, sizeof(markers_.back().codec), "%s", codecName);
    trackAt_ = writeAt_;
    if (heardPos_ == pos && readAt_ == markers_.back().at) {
      heardDuration_ = duration;
      memcpy(heardCodec_, markers_.back().codec, sizeof(heardCodec_));
    }
    // a video's sound again (Repeat): the picture's next pass starts where this one does on the clock
    if (videoOn_ && videoPasses_++ > 0) video_.loop(restartMs_ * 1000 + int64_t(writeAt_) * 1000000 / kRate);
  }
  pthread_mutex_unlock(&mu_);
  if (ok && seekMs > 0) {
    av_seek_frame(fmt, -1, seekMs * 1000, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec);
  }
  AVStream* st = ok ? fmt->streams[stream] : nullptr;
  std::vector<int16_t> out(size_t(8192) * 2);
  bool interrupted = false;
  bool measuring = ok && seekMs <= 0, reachedEnd = false;  // a song played from its start is measured
  if (measuring) meter_.reset();
  auto emit = [&](AVFrame* f) {
    // after a seek, drop what comes before the target (the seek lands on the previous key frame)
    if (seekMs > 0 && f && f->pts != AV_NOPTS_VALUE) {
      int64_t ms = av_rescale_q(f->pts, st->time_base, {1, 1000});
      if (ms + f->nb_samples * 1000LL / std::max(1, f->sample_rate) < seekMs) return true;
    }
    int max = swr_get_out_samples(swr, f ? f->nb_samples : 0);
    if (max > int(out.size() / 2)) out.resize(size_t(max) * 2);
    uint8_t* o = reinterpret_cast<uint8_t*>(out.data());
    int n = swr_convert(swr, &o, max, f ? (const uint8_t**)f->extended_data : nullptr, f ? f->nb_samples : 0);
    if (n <= 0) return true;
    if (measuring) meter_.add(out.data(), n);
    pthread_mutex_lock(&mu_);
    bool kept = write(out.data(), n, gen);
    pthread_mutex_unlock(&mu_);
    return kept;
  };
  bool readFailed = false;
  while (ok && !interrupted) {
    int r = av_read_frame(fmt, pkt);
    if (r < 0 && streamed && r != AVERROR_EOF) {  // the stream broke off (HttpSource tried for 20 s)
      readFailed = true;
      break;
    }
    if (r < 0) {  // the end: drain the decoder and the resampler
      avcodec_send_packet(dec, nullptr);
      while (avcodec_receive_frame(dec, frame) == 0)
        if (!emit(frame)) interrupted = true;
      if (!interrupted && !emit(nullptr)) interrupted = true;
      reachedEnd = !interrupted;
      break;
    }
    if (pkt->stream_index == stream && avcodec_send_packet(dec, pkt) == 0) {
      while (avcodec_receive_frame(dec, frame) == 0) {
        if (!emit(frame)) {
          interrupted = true;
          break;
        }
      }
    }
    av_packet_unref(pkt);
  }
  av_frame_free(&frame);
  av_packet_free(&pkt);
  swr_free(&swr);
  avcodec_free_context(&dec);
  avformat_close_input(&fmt);
  pthread_mutex_lock(&mu_);
  if (measuring && reachedEnd && !readFailed && gen == gen_) {
    float l = meter_.integrated();
    if (!std::isnan(l)) {
      bool listed = std::find(analyzeList_.begin(), analyzeList_.end(), path) != analyzeList_.end();
      float had;
      if (listed && !loudness_.find(path, had)) analyzeKnown_++;
      loudness_.put(path, l);
      if (getenv("RONDO_PLAYER_LOG")) printf("player: measured %.2f LUFS: %s\n", l, path.c_str());
    }
  }
  if (streamed && gen == gen_ && !interrupted && (!ok || readFailed)) {
    streamFailed_ = true;
    if (readFailed) {
      error_ = "Can't play this song";
      fprintf(stderr, "rondo-player: the stream broke off\n");
    }
  }
  return !interrupted && gen == gen_;
}

// The library measured in the background (Volume leveling, while the Y1 charges): a song at a time,
// those without a measurement, stopping as soon as it isn't allowed any more.
void Engine::analyzeLoop() {
  pthread_mutex_lock(&mu_);
  for (;;) {
    float l;
    while (analyzeAllowed_ && analyzeAt_ < analyzeList_.size() && loudness_.find(analyzeList_[analyzeAt_], l)) analyzeAt_++;
    if (!analyzeAllowed_ || analyzeAt_ >= analyzeList_.size()) {
      pthread_cond_wait(&analyzeCv_, &mu_);
      continue;
    }
    std::string path = analyzeList_[analyzeAt_];
    pthread_mutex_unlock(&mu_);
    int64_t t0 = monoNs();
    float lufs = measureFile(path);
    pthread_mutex_lock(&mu_);
    if (analyzeAt_ < analyzeList_.size() && analyzeList_[analyzeAt_] == path) analyzeAt_++;
    if (!std::isnan(lufs) && !loudness_.find(path, l)) {
      loudness_.put(path, lufs);
      analyzeKnown_++;
      publish();
    }
    if (getenv("RONDO_PLAYER_LOG"))
      printf("player: measured %.2f LUFS in %.1f s (%d of %zu): %s\n", lufs, (monoNs() - t0) / 1e9, analyzeKnown_,
             analyzeList_.size(), path.c_str());
  }
}

// A file decoded only to be measured: NAN when it can't be read or the measuring was stopped
float Engine::measureFile(const std::string& path) {
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return NAN;
  float lufs = NAN;
  const AVCodec* codec = nullptr;
  int si = avformat_find_stream_info(fmt, nullptr) >= 0 ? av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0) : -1;
  for (unsigned i = 0; i < fmt->nb_streams; i++)
    if (int(i) != si) fmt->streams[i]->discard = AVDISCARD_ALL;
  AVCodecContext* ctx = si >= 0 ? avcodec_alloc_context3(codec) : nullptr;
  SwrContext* swr = nullptr;
  AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  if (ctx && avcodec_parameters_to_context(ctx, fmt->streams[si]->codecpar) >= 0 && avcodec_open2(ctx, codec, nullptr) >= 0 &&
      knownLayout(ctx->ch_layout) && swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, kRate, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, 0,
                          nullptr) >= 0 &&
      swr_init(swr) >= 0) {
    LoudnessMeter meter;
    std::vector<int16_t> out(8192 * 2);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool stopped = false;
    auto drain = [&] {
      while (avcodec_receive_frame(ctx, frame) >= 0) {
        int max = swr_get_out_samples(swr, frame->nb_samples);
        if (max > int(out.size() / 2)) out.resize(size_t(max) * 2);
        uint8_t* o = reinterpret_cast<uint8_t*>(out.data());
        int n = swr_convert(swr, &o, max, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        if (n > 0) meter.add(out.data(), n);
      }
    };
    while (!stopped && av_read_frame(fmt, pkt) >= 0) {
      if (pkt->stream_index == si && avcodec_send_packet(ctx, pkt) >= 0) drain();
      av_packet_unref(pkt);
      pthread_mutex_lock(&mu_);
      stopped = !analyzeAllowed_;
      pthread_mutex_unlock(&mu_);
    }
    if (!stopped) {
      avcodec_send_packet(ctx, nullptr);
      drain();
      lufs = meter.integrated();
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
  }
  swr_free(&swr);
  avcodec_free_context(&ctx);
  avformat_close_input(&fmt);
  return lufs;
}

void Engine::decodeLoop() {
  pthread_mutex_lock(&mu_);
  for (;;) {
    while (startPos_ < 0) {
      if (!playing_ && applied_) {
        pthread_mutex_unlock(&mu_);
        applyPlayState();
        pthread_mutex_lock(&mu_);
        continue;
      }
      pthread_cond_wait(&cv_, &mu_);
    }
    uint32_t gen = gen_;
    int pos = startPos_;
    int64_t seekMs = startMs_;
    startPos_ = -1;
    // one track after another into the same stream: gapless
    for (int tries = 0; pos >= 0 && gen == gen_;) {
      uint64_t before = writeAt_;
      bool opened = false;
      trackAt_ = UINT64_MAX;
      bool done = decodeTrack(pos, seekMs, gen);
      if (gen != gen_) break;
      if (streamFailed_) {  // stop where it failed (heard once what was read before plays out)
        streamFailed_ = false;
        opened = trackAt_ != UINT64_MAX;
        int64_t at = opened ? seekMs + int64_t(writeAt_ - trackAt_) * 1000 / kRate : seekMs;
        markers_.push_back({opened ? writeAt_ : before, pos, 0, at, true});
        break;
      }
      seekMs = 0;
      if (!done && !error_.empty()) {  // can't be played: skip it (skipBroken)
        if (++tries > int(order_.size())) break;
      }
      pos = nextAfter(pos);
      if (pos < 0) markers_.push_back({writeAt_, -1, 0, 0});  // the queue ends here
    }
  }
}

// ---- output ----

bool Engine::openOutput() {
  SLEngineItf engine;
  if (slCreateEngine(&engineObj_, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return false;
  (*engineObj_)->Realize(engineObj_, SL_BOOLEAN_FALSE);
  (*engineObj_)->GetInterface(engineObj_, SL_IID_ENGINE, &engine);
  (*engine)->CreateOutputMix(engine, &mixObj_, 0, nullptr, nullptr);
  (*mixObj_)->Realize(mixObj_, SL_BOOLEAN_FALSE);
  SLDataLocator_AndroidSimpleBufferQueue locQ = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, kBuffers};
  SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
  SLDataSource src = {&locQ, &pcm};
  SLDataLocator_OutputMix locMix = {SL_DATALOCATOR_OUTPUTMIX, mixObj_};
  SLDataSink sink = {&locMix, nullptr};
  const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
  const SLboolean req[] = {SL_BOOLEAN_TRUE};
  if ((*engine)->CreateAudioPlayer(engine, &playerObj_, &src, &sink, 1, ids, req) != SL_RESULT_SUCCESS) return false;
  (*playerObj_)->Realize(playerObj_, SL_BOOLEAN_FALSE);
  (*playerObj_)->GetInterface(playerObj_, SL_IID_PLAY, &play_);
  (*playerObj_)->GetInterface(playerObj_, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue_);
  (*queue_)->RegisterCallback(queue_, &Engine::onBuffer, this);
  // the queue is kept full with sound or silence; pausing pauses the player
  memset(buffers_, 0, sizeof(buffers_));
  for (int b = 0; b < kBuffers; b++) {
    inFlight_.push_back({0, 0});  // they hold nothing of the stream
    (*queue_)->Enqueue(queue_, buffers_[b], sizeof(buffers_[b]));
  }
  (*play_)->SetPlayState(play_, SL_PLAYSTATE_PAUSED);
  click_.open(engine, mixObj_);
  return true;
}

void Engine::fill(SLAndroidSimpleBufferQueueItf q) {
  pthread_mutex_lock(&mu_);
  lastCallbackNs_ = monoNs();
  // the oldest buffer just finished playing: its frames have been heard
  if (!inFlight_.empty()) {
    playedAt_ = inFlight_.front().to;
    inFlight_.pop_front();
  }
  // tracks that began in what was heard
  while (!markers_.empty() && markers_.front().at <= playedAt_) {
    Marker m = markers_.front();
    markers_.pop_front();
    if (m.failed) {  // a stream failed here: stopped at that place of that song, Play goes on from it
      failed_ = true;
      playing_ = false;
      if (heardPos_ != m.pos) heardDuration_ = 0;
      heardPos_ = pos_ = m.pos;
      heardFrom_ = m.at;
      heardStartMs_ = m.startMs;
      pthread_cond_broadcast(&cv_);
      break;
    }
    if (m.pos < 0) {  // the queue ended
      ended_ = true;
      playing_ = false;
      heardStartMs_ = heardDuration_;
      heardFrom_ = playedAt_;
      pthread_cond_broadcast(&cv_);  // the decode thread pauses OpenSL (not from inside its callback)
      break;
    }
    heardPos_ = m.pos;
    heardFrom_ = m.at;
    heardStartMs_ = m.startMs;
    heardDuration_ = m.durationMs;
    heardGainDb_ = m.gainDb;
    memcpy(heardCodec_, m.codec, sizeof(heardCodec_));
    pos_ = m.pos;
  }
  int16_t* b = buffers_[next_];
  next_ = (next_ + 1) % kBuffers;
  if (fmSounding_ && !audioLost_) {  // the radio plays: its ring, music's left where it is
    int n = int(std::min<uint64_t>(fmWrite_ - fmRead_, kBufferFrames));
    for (int k = 0; k < n; k++) {
      size_t at = size_t((fmRead_ + uint64_t(k)) % kFmRingFrames) * 2;
      b[k * 2] = fmRing_[at];
      b[k * 2 + 1] = fmRing_[at + 1];
    }
    if (n < kBufferFrames) memset(b + n * 2, 0, size_t(kBufferFrames - n) * 4);
    if (dsp_.active()) dsp_.setTrackGain(NAN), dsp_.process(b, kBufferFrames);
    fmRead_ += uint64_t(n);
    inFlight_.push_back({readAt_, readAt_});
    (*q)->Enqueue(q, b, sizeof(buffers_[0]));
    pthread_mutex_unlock(&mu_);
    return;
  }
  // a video's sound waits (silence, nothing taken from the ring) until its picture is there; all of it
  // waits while mediaserver comes back (audioLost)
  int n = audioLost_ || waitingForPicture() ? 0 : int(std::min<uint64_t>(writeAt_ - readAt_, kBufferFrames));
  for (int k = 0; k < n; k++) {
    size_t at = size_t((readAt_ + uint64_t(k)) % kRingFrames) * 2;
    b[k * 2] = ring_[at];
    b[k * 2 + 1] = ring_[at + 1];
  }
  if (n < kBufferFrames) memset(b + n * 2, 0, size_t(kBufferFrames - n) * 4);  // not decoded yet: silence
  if (dsp_.active()) {
    // the track this buffer is from: the last one begun by readAt_ (the heard one lags two buffers)
    float g = heardGainDb_;
    for (const Marker& m : markers_) {
      if (m.at > readAt_) break;
      if (m.pos >= 0 && !m.failed) g = m.gainDb;
    }
    dsp_.setTrackGain(g);
    dsp_.process(b, kBufferFrames);
  }
  inFlight_.push_back({readAt_, readAt_ + uint64_t(n)});
  readAt_ += uint64_t(n);
  pthread_cond_broadcast(&cv_);
  publish();
  (*q)->Enqueue(q, b, sizeof(buffers_[0]));  // under the lock: a restart silences buffers in between
  pthread_mutex_unlock(&mu_);
}

void Engine::readVolume() {
  using android::AudioSystem;
  audio_devices_t device = AudioSystem::getDevicesForStream(AUDIO_STREAM_MUSIC);
  int index = 0;
  if (AudioSystem::getStreamVolumeIndex(AUDIO_STREAM_MUSIC, &index, device) == 0) volume_ = index;
}

void Engine::changeVolume(int delta) {
  using android::AudioSystem;
  audio_devices_t device = AudioSystem::getDevicesForStream(AUDIO_STREAM_MUSIC);
  readVolume();
  int index = std::max(0, std::min(std::min(volumeMax_, volumeLimit_), volume_ + delta));
  if (AudioSystem::setStreamVolumeIndex(AUDIO_STREAM_MUSIC, index, device) == 0) {
    volume_ = index;
    if (!fmOnOutput_) saveVolume(device, index);  // the radio's own step is the UI's (prefs fmVolume)
  }
}

// MediaTek's index maximum of each stream (dumpsys media.audio_policy with Android's AudioService, Nov 8)
constexpr int kStreamMax[] = {6, 7, 7, 13, 7, 7, 15, 7, 15, 15, 13, 13, 1};

void Engine::restoreVolumes() {
  using android::AudioSystem;
  // the volume limit first: nothing set below may pass it
  if (FILE* lf = fopen((volumesFile_.substr(0, volumesFile_.rfind('/') + 1) + "volume-limit").c_str(), "r")) {
    int v = volumeLimit_;
    if (fscanf(lf, "%d", &v) == 1) volumeLimit_ = std::max(0, std::min(volumeMax_, v));
    fclose(lf);
  }
  // Without Android (the 2.0 firmware) nobody gives the policy its ranges: every stream would have one
  // step. AudioService's initStreamVolume, with this firmware's maxima.
  if (!development_)
    for (int s = 0; s < int(sizeof(kStreamMax) / sizeof(kStreamMax[0])); s++)
      AudioSystem::initStreamVolume(audio_stream_type_t(s), 0, kStreamMax[s]);
  FILE* f = fopen(volumesFile_.c_str(), "r");
  if (!f) {
    // the first start without the file: Android's own (settings.db's volume_music_<device>) carry over
    // through the policy as Android left it; on a fresh 2.0 firmware, speaker and headphones start at 5
    if (!development_) {
      for (audio_devices_t d : {AUDIO_DEVICE_OUT_SPEAKER, AUDIO_DEVICE_OUT_WIRED_HEADSET, AUDIO_DEVICE_OUT_WIRED_HEADPHONE,
                                AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES})
        AudioSystem::setStreamVolumeIndex(AUDIO_STREAM_MUSIC, std::min(5, volumeLimit_), d);
    }
    return;
  }
  unsigned device;
  int index;
  while (fscanf(f, "%x %d", &device, &index) == 2)
    if (index >= 0 && index <= volumeMax_)
      AudioSystem::setStreamVolumeIndex(AUDIO_STREAM_MUSIC, std::min(index, volumeLimit_), audio_devices_t(device));
  fclose(f);
}

void Engine::setVolumeLimit(int limit) {
  using android::AudioSystem;
  volumeLimit_ = std::max(0, std::min(volumeMax_, limit));
  // every output above it comes down now (the one in use and the ones remembered)
  for (audio_devices_t d : {AUDIO_DEVICE_OUT_SPEAKER, AUDIO_DEVICE_OUT_WIRED_HEADSET, AUDIO_DEVICE_OUT_WIRED_HEADPHONE,
                            AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES,
                            AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER}) {
    int index = 0;
    if (AudioSystem::getStreamVolumeIndex(AUDIO_STREAM_MUSIC, &index, d) == 0 && index > volumeLimit_) {
      AudioSystem::setStreamVolumeIndex(AUDIO_STREAM_MUSIC, volumeLimit_, d);
      if (!fmOnOutput_) saveVolume(d, volumeLimit_);
    }
  }
  if (fmVolume_ > volumeLimit_) fmVolume_ = volumeLimit_;
  readVolume();
  std::string tmp = volumesFile_.substr(0, volumesFile_.rfind('/') + 1) + "volume-limit";
  if (FILE* f = fopen((tmp + ".part").c_str(), "w")) {
    fprintf(f, "%d\n", volumeLimit_);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (ok) rename((tmp + ".part").c_str(), tmp.c_str());
  }
}

void Engine::saveVolume(audio_devices_t device, int index) {
  // the file holds a line per output: read, change one, write (a few lines, on /data)
  std::vector<std::pair<unsigned, int>> lines;
  if (FILE* f = fopen(volumesFile_.c_str(), "r")) {
    unsigned d;
    int i;
    while (fscanf(f, "%x %d", &d, &i) == 2) lines.push_back({d, i});
    fclose(f);
  }
  bool found = false;
  for (auto& l : lines)
    if (l.first == unsigned(device)) l.second = index, found = true;
  if (!found) lines.push_back({unsigned(device), index});
  std::string tmp = volumesFile_ + ".part";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) return;
  for (auto& l : lines) fprintf(f, "%x %d\n", l.first, l.second);
  bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;  // on the flash before it counts: a power cut keeps it
  fclose(f);
  if (ok) rename(tmp.c_str(), volumesFile_.c_str());
}

namespace {
Engine* g_audioEngine = nullptr;
void onAudioError(android::status_t err) {
  if (err == android::DEAD_OBJECT && g_audioEngine) g_audioEngine->audioLost();
}
}  // namespace

void Engine::audioLost() {
  audioLost_ = true;  // silence from the next buffer on
  if (recovering_.exchange(true)) return;
  fprintf(stderr, "rondo-player: mediaserver is gone: silent until the volumes are back\n");
  std::thread([this] { recoverAudio(); }).detach();
}

void Engine::recoverAudio() {
  using android::AudioSystem;
  // the new mediaserver's services (init starts it again within a second or two)
  auto sm = android::defaultServiceManager();
  for (int i = 0; i < 300; i++) {
    if (sm->checkService(android::String16("media.audio_policy")) != nullptr && sm->checkService(android::String16("media.audio_flinger")) != nullptr)
      break;
    usleep(100000);
  }
  usleep(300000);  // its policy set up its outputs
  restoreVolumes();
  if (fmOnOutput_ && fmVolume_ >= 0)  // the radio was sounding: its own step on the output, not music's
    AudioSystem::setStreamVolumeIndex(AUDIO_STREAM_MUSIC, fmVolume_, AudioSystem::getDevicesForStream(AUDIO_STREAM_MUSIC));
  fmSpeakerApply(fmOnOutput_ && fmSpeaker_);
  readVolume();
  recovering_ = false;
  audioLost_ = false;
  pthread_mutex_lock(&mu_);
  publish();
  pthread_mutex_unlock(&mu_);
  fprintf(stderr, "rondo-player: mediaserver started again: the volume ranges and levels are back\n");
}

void Engine::main() {
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  prctl(PR_SET_NAME, "rondo-player");
  curl_global_init(CURL_GLOBAL_DEFAULT);  // before any thread: it isn't thread-safe
  setpriority(PRIO_PROCESS, 0, -8);
  av_log_set_level(AV_LOG_ERROR);  // FFmpeg's warnings about covers and probing are not errors
  android::ProcessState::self()->startThreadPool();
  // AudioFlinger and the audio policy check MODIFY_AUDIO_SETTINGS with the "permission" service, which
  // system_server hosts; without it, rondo does (N0 log)
  if (android::defaultServiceManager()->checkService(android::String16("permission")) == nullptr)
    rondo::PermissionService::publish();
  g_audioEngine = this;
  android::AudioSystem::setErrorCallback(onAudioError);  // mediaserver dying: audioLost()
  restoreVolumes();
  // the radio's speaker left forced by a player that ended mid-broadcast: media goes where it should
  fmSpeakerApply(false);
  readVolume();
  if (!openOutput()) fprintf(stderr, "rondo-player: no audio output\n");
  {  // the amplifier as NVRAM has it
    std::vector<uint8_t> t;
    if (amp::read(t)) {
      ampState_ = amp::level(t);
      if (const char* dump = getenv("RONDO_AMP_DUMP")) {  // development: the table as the HAL gives it
        if (FILE* f = fopen(dump, "wb")) fwrite(t.data(), 1, t.size(), f), fclose(f);
      }
      if (getenv("RONDO_PLAYER_LOG"))
        printf("player: amplifier table: headphones %d, speaker %d (level %d)\n", t[amp::kHeadset], t[amp::kSpeaker], ampState_);
    }
  }
  // A buffer handed to OpenSL is heard two output latencies later: measured on the Y1, the AudioTrack
  // holds 92 ms (OpenSL's GetPosition) and the mixer's output latency is 92 ms. A picture waits that
  // long after the clock reaches it.
  uint32_t latency = 0;
  android::AudioSystem::getOutputLatency(&latency, AUDIO_STREAM_MUSIC);
  int64_t syncMs = getenv("RONDO_AV_SYNC_MS") ? atoll(getenv("RONDO_AV_SYNC_MS")) : 2 * int64_t(latency);
  video_.setLatency(syncMs * 1000);
  if (getenv("RONDO_VIDEO_LOG")) printf("video: output latency %u ms, pictures %lld ms after the clock\n", latency, (long long)syncMs);
  pthread_t t;
  pthread_create(&t, nullptr, &Engine::decodeThread, this);
  pthread_t a;
  pthread_create(&a, nullptr, &Engine::analyzeThread, this);
  pthread_mutex_lock(&mu_);
  publish();
  pthread_mutex_unlock(&mu_);
  std::vector<uint8_t> m;
  for (;;) {
    uint32_t len;
    if (!readFully(sock_, &len, 4)) _exit(0);  // the UI is gone
    m.resize(len);
    if (len && !readFully(sock_, m.data(), len)) _exit(0);
    command(m);
  }
}

}  // namespace

[[noreturn]] void runPlayer(int sock, PlayerState* shm, const std::string& dataDir, bool development) {
  Engine* e = new Engine(sock, shm);
  e->setData(dataDir, development);
  e->main();
}

}  // namespace rondo
