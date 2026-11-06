// N0 test S5: sound without Java.
//
// Run with the Java layer stopped (`adb shell stop zygote`; mediaserver keeps running). It plays a
// quiet 440 Hz tone through OpenSL ES (the NDK's public audio API since Android 2.3) for a few
// seconds, from a buffer queue fed by its own callback, and reports whether every buffer came back in
// time: the callback period, its jitter, and underruns (buffers asked for later than they play).
// The tone is kept quiet on purpose: about -26 dBFS, and the player's own volume at -30 dB.
//
//   s5 [seconds] [buffer ms]     defaults 3 s and 20 ms

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace {

constexpr int kRate = 44100;
int64_t now_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

struct Tone {
  std::vector<int16_t> buf[2];
  int next = 0;
  double phase = 0;
  std::vector<int64_t> callbackAt;
  std::atomic<bool> stop{false};

  void fill(std::vector<int16_t>& b) {
    for (size_t i = 0; i < b.size(); i += 2) {
      int16_t s = int16_t(sin(phase) * 1600);  // ~ -26 dBFS
      b[i] = b[i + 1] = s;
      phase += 2 * M_PI * 440 / kRate;
      if (phase > 2 * M_PI) phase -= 2 * M_PI;
    }
  }
};

void onBuffer(SLAndroidSimpleBufferQueueItf q, void* ctx) {
  Tone* t = static_cast<Tone*>(ctx);
  t->callbackAt.push_back(now_ns());
  if (t->stop) return;
  std::vector<int16_t>& b = t->buf[t->next];
  t->next ^= 1;
  t->fill(b);
  (*q)->Enqueue(q, b.data(), b.size() * sizeof(int16_t));
}

bool ok(SLresult r, const char* what) {
  if (r == SL_RESULT_SUCCESS) return true;
  fprintf(stderr, "s5: %s failed (%u)\n", what, (unsigned)r);
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 3;
  int bufferMs = argc > 2 ? atoi(argv[2]) : 20;
  SLObjectItf engineObj, mixObj, playerObj;
  SLEngineItf engine;
  int64_t t0 = now_ns();
  if (!ok(slCreateEngine(&engineObj, 0, nullptr, 0, nullptr, nullptr), "slCreateEngine") ||
      !ok((*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE), "engine Realize") ||
      !ok((*engineObj)->GetInterface(engineObj, SL_IID_ENGINE, &engine), "engine interface") ||
      !ok((*engine)->CreateOutputMix(engine, &mixObj, 0, nullptr, nullptr), "CreateOutputMix") ||
      !ok((*mixObj)->Realize(mixObj, SL_BOOLEAN_FALSE), "mix Realize"))
    return 1;

  SLDataLocator_AndroidSimpleBufferQueue locQ = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
  SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
  SLDataSource src = {&locQ, &pcm};
  SLDataLocator_OutputMix locMix = {SL_DATALOCATOR_OUTPUTMIX, mixObj};
  SLDataSink sink = {&locMix, nullptr};
  const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
  const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  SLPlayItf play;
  SLAndroidSimpleBufferQueueItf queue;
  SLVolumeItf volume;
  if (!ok((*engine)->CreateAudioPlayer(engine, &playerObj, &src, &sink, 2, ids, req), "CreateAudioPlayer") ||
      !ok((*playerObj)->Realize(playerObj, SL_BOOLEAN_FALSE), "player Realize") ||
      !ok((*playerObj)->GetInterface(playerObj, SL_IID_PLAY, &play), "play interface") ||
      !ok((*playerObj)->GetInterface(playerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue), "queue") ||
      !ok((*playerObj)->GetInterface(playerObj, SL_IID_VOLUME, &volume), "volume interface"))
    return 1;
  (*volume)->SetVolumeLevel(volume, -3000);  // millibels: -30 dB
  int64_t t1 = now_ns();

  Tone tone;
  size_t frames = size_t(kRate) * bufferMs / 1000;
  tone.buf[0].assign(frames * 2, 0);
  tone.buf[1].assign(frames * 2, 0);
  tone.callbackAt.reserve(size_t(seconds) * 1000 / bufferMs + 64);
  (*queue)->RegisterCallback(queue, onBuffer, &tone);
  for (int i = 0; i < 2; i++) {
    tone.fill(tone.buf[i]);
    (*queue)->Enqueue(queue, tone.buf[i].data(), tone.buf[i].size() * sizeof(int16_t));
  }
  if (!ok((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING), "play")) return 1;
  int64_t started = now_ns();
  usleep(useconds_t(seconds) * 1000000);
  tone.stop = true;
  (*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED);
  SLmillisecond pos = 0;
  (*play)->GetPosition(play, &pos);

  std::vector<int64_t> gaps;
  for (size_t i = 1; i < tone.callbackAt.size(); i++) gaps.push_back(tone.callbackAt[i] - tone.callbackAt[i - 1]);
  std::sort(gaps.begin(), gaps.end());
  auto at = [&](double p) { return gaps.empty() ? 0.0 : gaps[std::min(gaps.size() - 1, size_t(p * (gaps.size() - 1)))] / 1e6; };
  // a buffer holds bufferMs of sound and two are queued, so a callback later than ~2 buffers means
  // the queue ran dry: an underrun you'd hear as a click
  long late = std::count_if(gaps.begin(), gaps.end(), [&](int64_t g) { return g > int64_t(bufferMs) * 2000000LL; });
  printf("s5: engine and player ready in %.1f ms; %zu callbacks in %d s (expected %d)\n", (t1 - t0) / 1e6,
         tone.callbackAt.size(), seconds, seconds * 1000 / bufferMs);
  printf("s5: callback period p1 %.2f ms, p50 %.2f, p99 %.2f, max %.2f; over two buffers apart: %ld\n", at(0.01),
         at(0.5), at(0.99), at(1.0), late);
  printf("s5: first callback %.1f ms after start; play position at stop %u ms\n",
         tone.callbackAt.empty() ? -1.0 : (tone.callbackAt[0] - started) / 1e6, (unsigned)pos);
  (*playerObj)->Destroy(playerObj);
  (*mixObj)->Destroy(mixObj);
  (*engineObj)->Destroy(engineObj);
  return 0;
}
