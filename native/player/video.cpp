// VideoDecoder (video.h): FFmpeg reads the H.264 stream ahead, MediaCodec decodes it on MediaTek's
// hardware, and each picture is rendered into "rondo.video" when the sound's clock reaches it. As in s2,
// with two differences: the samples come from FFmpeg (their NAL lengths replaced by start codes, the
// avcC turned into csd-0 and csd-1), and the time a picture is shown is the sound's, not a wall clock.
#include "video.h"

#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <set>

extern "C" {
#include <libavformat/avformat.h>
}

#include <curl/curl.h>

#include "../platform/aosp_begin.h"
#include <binder/IServiceManager.h>
#include <gui/ISurfaceTexture.h>
#include <gui/SurfaceTextureClient.h>
#include <media/ICrypto.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <utils/String16.h>
#include "../platform/aosp_end.h"

#include "../platform/video_surface.h"
#include "http_source.h"

// The firmware's AMessage::setString takes the length as a long (bionic's ssize_t when it was built),
// where the 4.2 header says ssize_t: bound by its own name.
extern "C" void rondoSetString(android::AMessage* m, const char* name, const char* s, long len)
    __asm__("_ZN7android8AMessage9setStringEPKcS2_l");

namespace rondo {
namespace {

constexpr size_t kMaxBytes = 12u << 20;   // read ahead at most this much of the stream
constexpr int64_t kAheadUs = 20000000;    // or this far
constexpr int64_t kLateUs = 120000;       // a picture this late is dropped (but one is shown every 250 ms)

int64_t monoNs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

template <typename T, typename... A>
T* roomy(A&&... a) {  // room to spare for MediaTek's larger classes (as in s2)
  return new (::operator new(sizeof(T) + 256)) T(static_cast<A&&>(a)...);
}

const uint8_t kStart[4] = {0, 0, 0, 1};

// The MP4's avcC as MediaCodec's csd-0 (the SPS) and csd-1 (the PPS), with start codes.
bool parseAvcc(const uint8_t* p, int n, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps, int& lengthSize) {
  if (!p || n < 7 || p[0] != 1) return false;
  lengthSize = (p[4] & 3) + 1;
  int at = 5;
  for (int set = 0; set < 2; set++) {
    std::vector<uint8_t>& out = set ? pps : sps;
    if (at >= n) return false;
    int count = set ? p[at] : p[at] & 0x1f;
    at++;
    for (int i = 0; i < count; i++) {
      if (at + 2 > n) return false;
      int len = p[at] << 8 | p[at + 1];
      at += 2;
      if (at + len > n) return false;
      out.insert(out.end(), kStart, kStart + 4);
      out.insert(out.end(), p + at, p + at + len);
      at += len;
    }
  }
  return !sps.empty() && !pps.empty();
}

// A sample's NAL units, each with a start code in place of its length.
void toAnnexB(const uint8_t* p, int n, int lengthSize, std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(size_t(n) + 64);
  int at = 0;
  while (at + lengthSize <= n) {
    uint32_t len = 0;
    for (int i = 0; i < lengthSize; i++) len = len << 8 | p[at + i];
    at += lengthSize;
    if (len > uint32_t(n - at)) break;
    out.insert(out.end(), kStart, kStart + 4);
    out.insert(out.end(), p + at, p + at + len);
    at += int(len);
  }
}

}  // namespace

void VideoDecoder::startThreads() {
  if (started_) return;
  started_ = true;
  pthread_t t;
  pthread_create(&t, nullptr, &VideoDecoder::demuxThread, this);
  pthread_create(&t, nullptr, &VideoDecoder::renderThread, this);
}

void VideoDecoder::play(const std::vector<std::string>& sources, const std::string& token, int64_t atMs, bool gate,
                        int64_t baseUs) {
  pthread_mutex_lock(&mu_);
  startThreads();
  want_ = true;
  job_++;
  sources_ = sources, token_ = token;
  seekGen_++;
  seekMs_ = atMs;
  baseUs_ = baseUs;
  loops_.clear();
  latestSeek_ = seekGen_;
  if (!gate) shownSeek_ = seekGen_;
  catchUp_ = !gate;
  failed_ = false;
  packets_.clear();
  queuedBytes_ = 0;
  height_ = 0;
  readMs_ = atMs;
  pthread_cond_broadcast(&cv_);
  pthread_mutex_unlock(&mu_);
}

void VideoDecoder::seek(int64_t atMs) {
  pthread_mutex_lock(&mu_);
  if (want_) {
    seekGen_++;
    seekMs_ = atMs;
    baseUs_ = 0;  // a seek starts the clock again at the stream's own time
    loops_.clear();
    latestSeek_ = seekGen_;
    pthread_cond_broadcast(&cv_);
  }
  pthread_mutex_unlock(&mu_);
}

void VideoDecoder::setLooping(bool on) {
  pthread_mutex_lock(&mu_);
  looping_ = on;
  pthread_cond_broadcast(&cv_);
  pthread_mutex_unlock(&mu_);
}

void VideoDecoder::loop(int64_t baseUs) {
  pthread_mutex_lock(&mu_);
  if (want_) loops_.push_back(baseUs);
  pthread_cond_broadcast(&cv_);
  pthread_mutex_unlock(&mu_);
}

void VideoDecoder::stop() {
  pthread_mutex_lock(&mu_);
  if (want_) {
    want_ = false;
    job_++;
    packets_.clear();
    queuedBytes_ = 0;
    height_ = 0;
    readMs_ = 0;
    pthread_cond_broadcast(&cv_);
  }
  pthread_mutex_unlock(&mu_);
}

void* VideoDecoder::demuxThread(void* self) {
  prctl(PR_SET_NAME, "rondo-vread");
  setpriority(PRIO_PROCESS, 0, 0);
  static_cast<VideoDecoder*>(self)->demuxLoop();
  return nullptr;
}

void* VideoDecoder::renderThread(void* self) {
  prctl(PR_SET_NAME, "rondo-vshow");
  setpriority(PRIO_PROCESS, 0, -6);  // light work, but on time
  static_cast<VideoDecoder*>(self)->renderLoop();
  return nullptr;
}

void VideoDecoder::demuxLoop() {
  CURL* curl = curl_easy_init();  // its own connection: the sound has the decoder thread's
  pthread_mutex_lock(&mu_);
  uint32_t done = 0;
  for (;;) {
    while (!want_ || job_ == done) pthread_cond_wait(&cv_, &mu_);
    uint32_t job = done = job_;
    std::vector<std::string> sources = sources_;
    std::string token = token_;
    pthread_mutex_unlock(&mu_);

    auto gone = [this, job] {
      pthread_mutex_lock(&mu_);
      bool g = job_ != job;
      pthread_mutex_unlock(&mu_);
      return g;
    };
    std::unique_ptr<HttpSource> http;
    AVFormatContext* fmt = nullptr;
    int stream = -1, lengthSize = 4;
    Format f;
    bool ok = false;
    // the first that opens (a quality the video doesn't have: the server answers 404, and the next is Auto's)
    for (size_t k = 0; k < sources.size() && !ok && !gone(); k++) {
      const std::string& source = sources[k];
      bool url = source.compare(0, 4, "http") == 0;
      http.reset();
      fmt = nullptr;
      if (url) {
        http.reset(new HttpSource(curl, source, token, gone));
        fmt = avformat_alloc_context();
        fmt->pb = http->avio();
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
      }
      stream = -1;
      // the stream's header says all MediaCodec needs: no probing (it would read ahead over the network)
      if (avformat_open_input(&fmt, url ? "" : source.c_str(), nullptr, nullptr) == 0) {
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
          AVCodecParameters* cp = fmt->streams[i]->codecpar;
          if (stream < 0 && cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->codec_id == AV_CODEC_ID_H264) stream = int(i);
          else fmt->streams[i]->discard = AVDISCARD_ALL;
        }
        if (stream >= 0) {
          AVCodecParameters* cp = fmt->streams[stream]->codecpar;
          f = Format();
          f.width = cp->width, f.height = cp->height;
          ok = f.width > 0 && f.height > 0 && parseAvcc(cp->extradata, cp->extradata_size, f.csd0, f.csd1, lengthSize);
        }
        if (!ok) avformat_close_input(&fmt);
      } else {
        fmt = nullptr;  // freed by avformat_open_input
      }
      if (!ok) fprintf(stderr, "rondo-player: no picture from %s\n", url ? source.substr(source.find("/s/")).c_str() : source.c_str());
    }
    pthread_mutex_lock(&mu_);
    if (!ok) {
      if (job_ == job) failed_ = true;  // nothing to wait for: the sound plays on
      pthread_mutex_unlock(&mu_);
      http.reset();
      pthread_mutex_lock(&mu_);
      continue;  // the sound plays on
    }
    if (job_ == job) {
      format_ = f;
      formatJob_ = job;
      height_ = f.height;
      pthread_cond_broadcast(&cv_);
    }
    AVStream* st = fmt->streams[stream];
    AVPacket* pkt = av_packet_alloc();
    uint32_t seek = 0;  // none yet: the first pass puts the stream at seekMs_
    bool eos = false, readAny = false, rewind = false;
    int64_t base = 0;
    while (job_ == job) {
      if (seek != seekGen_) {
        seek = seekGen_;
        int64_t at = seekMs_;
        base = baseUs_;
        rewind = false;
        packets_.clear();
        queuedBytes_ = 0;
        queueSeek_ = seek;
        eos = false;
        readMs_ = at;
        pthread_cond_broadcast(&cv_);
        pthread_mutex_unlock(&mu_);
        // to the key frame before the target; the renderer drops the pictures before it
        if (at > 0 || readAny) {
          int r = av_seek_frame(fmt, stream, av_rescale_q(at * 1000, AVRational{1, 1000000}, st->time_base), AVSEEK_FLAG_BACKWARD);
          if (getenv("RONDO_VIDEO_LOG")) printf("video: stream to %lld ms: %d\n", (long long)at, r);
        }
        pthread_mutex_lock(&mu_);
        continue;
      }
      if (rewind && !loops_.empty()) {  // the next pass of a repeat: the stream from its start, later on the clock
        base = loops_.front();
        loops_.pop_front();
        rewind = false;
        pthread_mutex_unlock(&mu_);
        av_seek_frame(fmt, stream, 0, AVSEEK_FLAG_BACKWARD);
        pthread_mutex_lock(&mu_);
        continue;
      }
      if (rewind && !looping_) {  // Repeat went off at the end: the decoder gets its end after all
        rewind = false;
        eos = true;
        packets_.push_back(Packet{{}, 0, true});
        pthread_cond_broadcast(&cv_);
        continue;
      }
      bool full = queuedBytes_ > kMaxBytes || (packets_.size() > 1 && packets_.back().ptsUs - packets_.front().ptsUs > kAheadUs);
      if (eos || full || rewind) {
        timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 100000000;
        if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&cv_, &mu_, &until);
        continue;
      }
      pthread_mutex_unlock(&mu_);
      int r = av_read_frame(fmt, pkt);
      readAny = true;
      Packet p{};
      bool have = false;
      if (r < 0) {
        p.eos = have = true;  // the end (or the stream gave out after HttpSource's retries)
      } else if (pkt->stream_index == stream && pkt->size > 0) {
        toAnnexB(pkt->data, pkt->size, lengthSize, p.data);
        int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        p.ptsUs = ts == AV_NOPTS_VALUE ? 0 : av_rescale_q(ts, st->time_base, AVRational{1, 1000000});
        have = !p.data.empty();
      }
      if (r >= 0) av_packet_unref(pkt);
      pthread_mutex_lock(&mu_);
      if (have && p.eos && r == AVERROR_EOF && looping_ && job_ == job && seek == seekGen_) {
        rewind = true;  // a repeat: wait for the next pass's time instead of ending the decoder
        continue;
      }
      if (have && job_ == job && seek == seekGen_) {
        if (p.eos) eos = true;
        else readMs_ = std::max<int64_t>(readMs_.load(), p.ptsUs / 1000), p.ptsUs += base;
        queuedBytes_ += p.data.size();
        packets_.push_back(std::move(p));
        pthread_cond_broadcast(&cv_);
      }
    }
    pthread_mutex_unlock(&mu_);
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    http.reset();
    pthread_mutex_lock(&mu_);
  }
}

void VideoDecoder::renderLoop() {
  using namespace android;
  sp<ALooper> looper = roomy<ALooper>();
  looper->setName("rondo-video");
  looper->start();
  pthread_mutex_lock(&mu_);
  uint32_t done = 0;
  for (;;) {
    while (!want_ || job_ == done || formatJob_ != job_) pthread_cond_wait(&cv_, &mu_);
    uint32_t job = done = job_;
    Format f = format_;
    uint32_t mySeek = queueSeek_;
    int64_t dropBefore = baseUs_ + seekMs_ * 1000;
    // Another quality starts where the sound was when it was asked for, and the sound goes on meanwhile:
    // the pictures it has passed are skipped (the last one stays on the screen), not shown late.
    bool catchUp = catchUp_;
    pthread_mutex_unlock(&mu_);

    // the decoder, rendering into the drawing process's surface
    sp<MediaCodec> codec;
    sp<SurfaceTextureClient> window;
    sp<IBinder> binder = defaultServiceManager()->checkService(String16(kVideoService));
    if (binder != nullptr) {
      window = roomy<SurfaceTextureClient>(interface_cast<ISurfaceTexture>(binder));
      sp<AMessage> format = roomy<AMessage>();
      rondoSetString(format.get(), "mime", "video/avc", -1);
      format->setInt32("width", f.width);
      format->setInt32("height", f.height);
      format->setInt32("max-input-size", std::max(f.width * f.height, 256 * 1024));
      sp<ABuffer> sps = roomy<ABuffer>(f.csd0.size()), pps = roomy<ABuffer>(f.csd1.size());
      memcpy(sps->data(), f.csd0.data(), f.csd0.size());
      memcpy(pps->data(), f.csd1.data(), f.csd1.size());
      format->setBuffer("csd-0", sps);
      format->setBuffer("csd-1", pps);
      int64_t t0 = monoNs();
      codec = MediaCodec::CreateByType(looper, "video/avc", false);
      if (codec == nullptr || codec->configure(format, window, nullptr, 0) != OK || codec->start() != OK) {
        fprintf(stderr, "rondo-player: can't start the video decoder (%dx%d)\n", f.width, f.height);
        if (codec != nullptr) codec->release();
        codec.clear();
      } else if (getenv("RONDO_VIDEO_LOG")) {
        printf("video: %dx%d decoder ready in %.0f ms\n", f.width, f.height, (monoNs() - t0) / 1e6);
      }
    } else {
      fprintf(stderr, "rondo-player: no %s to show the video on\n", kVideoService);
    }
    if (codec == nullptr) {
      failed_ = true;
      pthread_mutex_lock(&mu_);
      continue;
    }
    Vector<sp<ABuffer> > inputs;
    codec->getInputBuffers(&inputs);

    std::multiset<int64_t> queued;  // presentation times of the samples in the decoder, smallest first
    bool first = !catchUp, inputDone = false;
    ssize_t freeInput = -1, held = -1;
    int64_t heldPts = 0, lastShown = 0;
    int shown = 0, dropped = 0, zeroPts = 0;
    int64_t firstNs = 0;
    const bool log = getenv("RONDO_VIDEO_LOG") != nullptr;
    for (;;) {
      pthread_mutex_lock(&mu_);
      if (!want_ || job_ != job) {
        pthread_mutex_unlock(&mu_);
        break;
      }
      if (queueSeek_ != mySeek) {  // the stream was moved: everything in the decoder is the old place's
        mySeek = queueSeek_;
        dropBefore = baseUs_ + seekMs_ * 1000;
        pthread_mutex_unlock(&mu_);
        codec->flush();  // every buffer comes back, the held picture and the free input with them
        if (log) printf("video: flushed for %lld ms (%zu samples were in the decoder)\n", (long long)dropBefore / 1000, queued.size());
        held = freeInput = -1;
        queued.clear();
        first = true, inputDone = false;
        catchUp = false;
        continue;
      }
      Packet p;
      bool popped = false;
      if (freeInput >= 0 && !packets_.empty()) {
        p = std::move(packets_.front());
        packets_.pop_front();
        queuedBytes_ -= p.data.size();
        popped = true;
        pthread_cond_broadcast(&cv_);  // room for the reader
      }
      pthread_mutex_unlock(&mu_);

      if (popped) {
        if (p.eos) {
          codec->queueInputBuffer(size_t(freeInput), 0, 0, 0, MediaCodec::BUFFER_FLAG_EOS);
          inputDone = true;
        } else {
          const sp<ABuffer>& buf = inputs.itemAt(size_t(freeInput));
          size_t n = std::min(p.data.size(), buf->capacity());
          memcpy(buf->base(), p.data.data(), n);
          codec->queueInputBuffer(size_t(freeInput), 0, n, p.ptsUs, 0);
          queued.insert(p.ptsUs);
        }
        freeInput = -1;
      }
      if (freeInput < 0 && !inputDone) {
        size_t i;
        if (codec->dequeueInputBuffer(&i, 0) == OK) freeInput = ssize_t(i);
      }

      if (held < 0) {
        size_t i, offset, size;
        int64_t pts;
        uint32_t flags;
        status_t r = codec->dequeueOutputBuffer(&i, &offset, &size, &pts, &flags, popped ? 0 : 10000);
        if (r == OK) {
          if (flags & MediaCodec::BUFFER_FLAG_EOS) {
            codec->releaseOutputBuffer(i);
            shownSeek_ = mySeek;  // the end: no picture at the target to wait for
            continue;
          }
          // MediaTek's decoder gives every picture time 0 (s2): the samples' own times, smallest first
          int64_t t = pts;
          if (pts == 0 && !queued.empty()) {
            zeroPts++;
            t = *queued.begin();
            queued.erase(queued.begin());
          } else {
            auto it = queued.find(pts);
            if (it != queued.end()) queued.erase(it);
          }
          // before a seek's target, or the old place's while a seek is on its way to the decoder
          bool passed = false;
          if (catchUp && t >= dropBefore) {
            bool moving = false;
            passed = t < clock_(moving) - latencyUs_;
            if (!passed) catchUp = false, first = !moving;  // caught up: on time from here (at once, paused)
          }
          if (t < dropBefore || passed || latestSeek_.load() != mySeek) codec->releaseOutputBuffer(i);
          else held = ssize_t(i), heldPts = t;
        }
      }
      if (held >= 0 && latestSeek_.load() != mySeek) {  // a seek came: the held picture is the old place's
        codec->releaseOutputBuffer(size_t(held));
        held = -1;
      }
      if (held >= 0) {
        bool moving = false;
        int64_t now = clock_(moving) - latencyUs_;
        int64_t ns = monoNs();
        if (first || (moving && now >= heldPts)) {
          // the first picture after a start or a seek is shown at once (paused, it is what is seen)
          if (!first && now - heldPts > kLateUs && ns - lastShown < 250 * 1000000LL) {
            codec->releaseOutputBuffer(size_t(held));
            dropped++;
            if (log) printf("video: late %lld ms at %lld ms, %lld ms after the first picture\n", (long long)(now - heldPts) / 1000,
                            (long long)heldPts / 1000, (long long)(ns - firstNs) / 1000000);
          } else {
            codec->renderOutputBufferAndRelease(size_t(held));
            lastShown = ns;
            shown++;
          }
          held = -1;
          if (first && log) {
            bool m;
            printf("video: first picture %lld ms, the clock at %lld ms, %zu samples in the decoder\n", (long long)heldPts / 1000,
                   (long long)clock_(m) / 1000, queued.size());
          }
          if (first) firstNs = ns;
          if (first) shownSeek_ = mySeek;  // the sound may start
          first = false;
        } else if (!popped) {
          int64_t wait = moving ? std::max<int64_t>(1000, std::min<int64_t>(heldPts - now, 5000)) : 20000;
          usleep(useconds_t(wait));
        }
      }
    }
    if (getenv("RONDO_VIDEO_LOG"))
      printf("video: %d pictures shown, %d dropped late, %d with time 0\n", shown, dropped, zeroPts);
    if (held >= 0) codec->releaseOutputBuffer(size_t(held));
    codec->stop();
    codec->release();
    codec.clear();
    window.clear();
    pthread_mutex_lock(&mu_);
  }
}

}  // namespace rondo
