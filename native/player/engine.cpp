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
#include <deque>
#include <random>
#include <string>
#include <vector>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include "../platform/aosp_begin.h"
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <media/AudioSystem.h>
#include "../platform/permission_service.h"
#include "../platform/aosp_end.h"

#include "player.h"
#include "protocol.h"

namespace rondo {
namespace {

constexpr int kRate = 44100;
constexpr int kBufferFrames = 2048;  // per OpenSL buffer: 46 ms
constexpr int kBuffers = 3;
constexpr uint32_t kRingFrames = 1u << 17;  // 3 s of PCM

int64_t monoNs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

struct Marker {  // where in the stream a track begins (pos -1: the queue's end)
  uint64_t at;
  int pos;
  int64_t durationMs, startMs;
};

class Engine {
 public:
  Engine(int sock, PlayerState* shm) : sock_(sock), shm_(shm) {}
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
  void decodeLoop();
  bool decodeTrack(int pos, int64_t seekMs, uint32_t gen);
  bool write(const int16_t* pcm, int frames, uint32_t gen);

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
  int16_t buffers_[kBuffers][kBufferFrames * 2];
  int next_ = 0;
  int volume_ = 0, volumeMax_ = 15;
};

void Engine::publish() {
  // under mu_
  PlayerState& s = *shm_;
  __atomic_add_fetch(&s.seq, 1, __ATOMIC_ACQ_REL);  // odd: being written
  s.playing = playing_;
  s.buffering = playing_ && readAt_ == writeAt_ && !ended_;
  s.pos = heardPos_;
  s.index = heardPos_ >= 0 && heardPos_ < int(order_.size()) ? order_[heardPos_] : -1;
  s.count = int(items_.size());
  s.shuffle = shuffle_;
  s.repeat = repeat_;
  int64_t played = int64_t(playedAt_ - heardFrom_) * 1000 / kRate + heardStartMs_;
  if (playing_ && lastCallbackNs_ && !ended_) played += std::min<int64_t>((monoNs() - lastCallbackNs_) / 1000000, 46);
  s.positionMs = std::max<int64_t>(0, std::min(played, heardDuration_ > 0 ? heardDuration_ : played));
  s.durationMs = heardDuration_;
  s.volume = volume_;
  s.volumeMax = volumeMax_;
  s.ended = ended_;
  int np = heardPos_ >= 0 ? nextAfter(heardPos_) : -1;
  s.nextIndex = np >= 0 && np < int(order_.size()) ? order_[np] : -1;
  if (s.index >= 0) snprintf(s.path, sizeof(s.path), "%s", items_[size_t(s.index)].c_str());
  else s.path[0] = 0;
  snprintf(s.error, sizeof(s.error), "%s", error_.c_str());
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
  error_.clear();
  pos_ = pos;
  pthread_cond_broadcast(&cv_);
  setPlaying(play && pos >= 0);
}

void Engine::setPlaying(bool on) { playing_ = on; }  // under mu_; OpenSL follows in applyPlayState()

// OpenSL's play state follows playing_, set outside mu_ (the buffer callback takes mu_)
void Engine::applyPlayState() {
  pthread_mutex_lock(&mu_);
  bool want = playing_;
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
  pthread_mutex_lock(&mu_);
  int64_t heard = int64_t(playedAt_ - heardFrom_) * 1000 / kRate + heardStartMs_;
  switch (type) {
    case kPlayQueue: {
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
    case kToggle:
      if (heardPos_ >= 0) {
        if (ended_) restart(heardPos_, 0, true);
        else setPlaying(!playing_);
      }
      break;
    case kPlay:
      if (heardPos_ >= 0 && !playing_) setPlaying(true);
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
    case kRepeat:
      repeat_ = len ? p[0] : 0;
      if (heardPos_ >= 0) restart(heardPos_, heard, playing_);
      break;
    case kVolume: {
      int32_t delta = int32_t(i64());
      pthread_mutex_unlock(&mu_);
      changeVolume(delta);
      pthread_mutex_lock(&mu_);
      break;
    }
  }
  publish();
  pthread_mutex_unlock(&mu_);
  applyPlayState();
}

// ---- decoding ----

bool Engine::write(const int16_t* pcm, int frames, uint32_t gen) {
  // under mu_ (released while waiting for room)
  while (frames > 0) {
    if (gen != gen_) return false;
    uint64_t room = kRingFrames - (writeAt_ - readAt_);
    if (room == 0) {
      timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 50000000;
      if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
      pthread_cond_timedwait(&cv_, &mu_, &until);
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
  return true;
}

// Decodes one track into the ring; false when a restart interrupted it.
bool Engine::decodeTrack(int pos, int64_t seekMs, uint32_t gen) {
  std::string path = items_[size_t(order_[size_t(pos)])];
  pthread_mutex_unlock(&mu_);
  AVFormatContext* fmt = nullptr;
  AVCodecContext* dec = nullptr;
  SwrContext* swr = nullptr;
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool ok = false;
  int stream = -1;
  const AVCodec* codec = nullptr;
  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) == 0 && avformat_find_stream_info(fmt, nullptr) >= 0 &&
      (stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0)) >= 0 && codec) {
    dec = avcodec_alloc_context3(codec);
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) >= 0 && avcodec_open2(dec, codec, nullptr) == 0 &&
        swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, kRate, &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0,
                            nullptr) == 0 &&
        swr_init(swr) == 0)
      ok = true;
  }
  pthread_mutex_lock(&mu_);
  if (!ok) {
    error_ = "Can't play " + path.substr(path.rfind('/') + 1);
    fprintf(stderr, "rondo-player: %s\n", error_.c_str());
  }
  if (ok) error_.clear();
  if (ok && gen == gen_) {
    int64_t duration = fmt->duration > 0 ? fmt->duration / 1000 : 0;
    markers_.push_back({writeAt_, pos, duration, seekMs});
    if (heardPos_ == pos && readAt_ == markers_.back().at) heardDuration_ = duration;
  }
  pthread_mutex_unlock(&mu_);
  if (ok && seekMs > 0) {
    av_seek_frame(fmt, -1, seekMs * 1000, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec);
  }
  AVStream* st = ok ? fmt->streams[stream] : nullptr;
  std::vector<int16_t> out(size_t(8192) * 2);
  bool interrupted = false;
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
    pthread_mutex_lock(&mu_);
    bool kept = write(out.data(), n, gen);
    pthread_mutex_unlock(&mu_);
    return kept;
  };
  while (ok && !interrupted) {
    int r = av_read_frame(fmt, pkt);
    if (r < 0) {  // the end: drain the decoder and the resampler
      avcodec_send_packet(dec, nullptr);
      while (avcodec_receive_frame(dec, frame) == 0)
        if (!emit(frame)) interrupted = true;
      if (!interrupted && !emit(nullptr)) interrupted = true;
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
  return !interrupted && gen == gen_;
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
      bool done = decodeTrack(pos, seekMs, gen);
      if (gen != gen_) break;
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
    pos_ = m.pos;
  }
  int16_t* b = buffers_[next_];
  next_ = (next_ + 1) % kBuffers;
  int n = int(std::min<uint64_t>(writeAt_ - readAt_, kBufferFrames));
  for (int k = 0; k < n; k++) {
    size_t at = size_t((readAt_ + uint64_t(k)) % kRingFrames) * 2;
    b[k * 2] = ring_[at];
    b[k * 2 + 1] = ring_[at + 1];
  }
  if (n < kBufferFrames) memset(b + n * 2, 0, size_t(kBufferFrames - n) * 4);  // not decoded yet: silence
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
  int index = std::max(0, std::min(volumeMax_, volume_ + delta));
  if (AudioSystem::setStreamVolumeIndex(AUDIO_STREAM_MUSIC, index, device) == 0) volume_ = index;
}

void Engine::main() {
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  prctl(PR_SET_NAME, "rondo-player");
  setpriority(PRIO_PROCESS, 0, -8);
  android::ProcessState::self()->startThreadPool();
  // AudioFlinger and the audio policy check MODIFY_AUDIO_SETTINGS with the "permission" service, which
  // system_server hosts; without it, rondo does (N0 log)
  if (android::defaultServiceManager()->checkService(android::String16("permission")) == nullptr)
    rondo::PermissionService::publish();
  readVolume();
  if (!openOutput()) fprintf(stderr, "rondo-player: no audio output\n");
  pthread_t t;
  pthread_create(&t, nullptr, &Engine::decodeThread, this);
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

[[noreturn]] void runPlayer(int sock, PlayerState* shm) {
  Engine* e = new Engine(sock, shm);
  e->main();
}

}  // namespace rondo
