// Click (click.h).
#include "click.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace rondo {
namespace {

const char* kTick = "/system/media/audio/ui/Effect_Tick.ogg";

// the whole file as 44.1 kHz stereo 16-bit
bool decode(const char* path, std::vector<int16_t>& out) {
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) return false;
  bool ok = false;
  const AVCodec* codec = nullptr;
  int si = avformat_find_stream_info(fmt, nullptr) >= 0 ? av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0) : -1;
  AVCodecContext* ctx = si >= 0 ? avcodec_alloc_context3(codec) : nullptr;
  SwrContext* swr = nullptr;
  AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  if (ctx && avcodec_parameters_to_context(ctx, fmt->streams[si]->codecpar) >= 0 && avcodec_open2(ctx, codec, nullptr) >= 0 &&
      swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 44100, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, 0,
                          nullptr) >= 0 &&
      swr_init(swr) >= 0) {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto drain = [&] {
      while (avcodec_receive_frame(ctx, frame) >= 0) {
        int cap = swr_get_out_samples(swr, frame->nb_samples);
        size_t at = out.size();
        out.resize(at + size_t(cap) * 2);
        uint8_t* dst = reinterpret_cast<uint8_t*>(out.data() + at);
        int n = swr_convert(swr, &dst, cap, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        out.resize(at + size_t(n > 0 ? n : 0) * 2);
      }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
      if (pkt->stream_index == si && avcodec_send_packet(ctx, pkt) >= 0) drain();
      av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, nullptr);
    drain();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    ok = !out.empty();
  }
  swr_free(&swr);
  avcodec_free_context(&ctx);
  avformat_close_input(&fmt);
  return ok;
}

}  // namespace

bool Click::open(SLEngineItf engine, SLObjectItf mix) {
  if (!decode(kTick, pcm_)) {
    fprintf(stderr, "rondo-player: no click sound (%s)\n", kTick);
    return false;
  }
  SLDataLocator_AndroidSimpleBufferQueue locQ = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1};
  SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
  SLDataSource src = {&locQ, &pcm};
  SLDataLocator_OutputMix locMix = {SL_DATALOCATOR_OUTPUTMIX, mix};
  SLDataSink sink = {&locMix, nullptr};
  const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION, SL_IID_VOLUME};
  const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  if ((*engine)->CreateAudioPlayer(engine, &obj_, &src, &sink, 3, ids, req) != SL_RESULT_SUCCESS) return false;
  SLAndroidConfigurationItf config;
  if ((*obj_)->GetInterface(obj_, SL_IID_ANDROIDCONFIGURATION, &config) == SL_RESULT_SUCCESS) {
    SLint32 stream = SL_ANDROID_STREAM_SYSTEM;
    (*config)->SetConfiguration(config, SL_ANDROID_KEY_STREAM_TYPE, &stream, sizeof(stream));
  }
  if ((*obj_)->Realize(obj_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return false;
  (*obj_)->GetInterface(obj_, SL_IID_PLAY, &play_);
  (*obj_)->GetInterface(obj_, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue_);
  SLVolumeItf volume;
  if ((*obj_)->GetInterface(obj_, SL_IID_VOLUME, &volume) == SL_RESULT_SUCCESS) {
    // -6 dB (Android's sound effects); development: RONDO_CLICK_SILENT=1 plays it at no volume
    (*volume)->SetVolumeLevel(volume, getenv("RONDO_CLICK_SILENT") ? SL_MILLIBEL_MIN : -600);
  }
  (*play_)->SetPlayState(play_, SL_PLAYSTATE_PAUSED);
  durationNs_ = int64_t(pcm_.size() / 2) * 1000000000LL / 44100;
  pthread_t t;
  if (pthread_create(&t, nullptr, &Click::idle, this) == 0) pthread_detach(t);
  return play_ && queue_;
}

namespace {
int64_t monoNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
}  // namespace

// A player left playing with an empty queue keeps its AudioTrack asking for sound every few
// milliseconds; paused, it costs nothing.
void* Click::idle(void* self) {
  Click* c = static_cast<Click*>(self);
  pthread_mutex_lock(&c->mu_);
  for (;;) {
    while (!c->pauseAt_) pthread_cond_wait(&c->cv_, &c->mu_);
    int64_t left = c->pauseAt_ - monoNs();
    if (left > 0) {
      pthread_mutex_unlock(&c->mu_);
      usleep(useconds_t(std::min<int64_t>(left, 50000000LL) / 1000));
      pthread_mutex_lock(&c->mu_);
      continue;
    }
    (*c->play_)->SetPlayState(c->play_, SL_PLAYSTATE_PAUSED);
    c->pauseAt_ = 0;
  }
  return nullptr;
}

void Click::play() {
  if (!queue_) return;
  pthread_mutex_lock(&mu_);
  (*queue_)->Clear(queue_);
  (*queue_)->Enqueue(queue_, pcm_.data(), SLuint32(pcm_.size() * sizeof(int16_t)));
  if (!pauseAt_) (*play_)->SetPlayState(play_, SL_PLAYSTATE_PLAYING);
  pauseAt_ = monoNs() + durationNs_ + 150000000LL;  // its sound, then the output's latency
  pthread_cond_signal(&cv_);
  pthread_mutex_unlock(&mu_);
  if (getenv("RONDO_PLAYER_LOG")) printf("player: click (%zu frames)\n", pcm_.size() / 2);
}

}  // namespace rondo
