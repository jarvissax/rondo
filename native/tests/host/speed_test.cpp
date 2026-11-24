// Audiobook speed (player/engine.cpp's Sonic stage) on the Mac: a voice-like sound (a 150 Hz tone with
// its harmonics, in syllables) through Sonic as the decoder feeds it (44.1 kHz stereo, a frame of 1152
// at a time, flushed at the end), at each speed Speed offers. It must come out 1/speed as long, its
// pitch kept: 150 Hz still (sped up as a tape would, it would be 150 x speed).
//   clang++ -std=c++17 -I native/third_party/src/sonic-<commit> <that>/sonic.c native/tests/host/speed_test.cpp
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "sonic.h"

static int failures = 0;

// the strongest period between 2.5 and 12.5 ms (80-400 Hz) in the middle of a voiced stretch, as Hz
static double pitchOf(const std::vector<short>& st, size_t frames, int rate, size_t at) {
  const int n = 2048, lo = rate / 400, hi = rate / 80;
  if (at + n + hi >= frames) return 0;
  double best = -1;
  int bestLag = 0;
  for (int lag = lo; lag <= hi; lag++) {
    double sum = 0, e0 = 0, e1 = 0;
    for (int i = 0; i < n; i++) {
      double a = st[(at + i) * 2], b = st[(at + i + lag) * 2];
      sum += a * b, e0 += a * a, e1 += b * b;
    }
    double r = sum / sqrt(e0 * e1 + 1e-9);
    if (r > best) best = r, bestLag = lag;
  }
  return bestLag ? double(rate) / bestLag : 0;
}

int main() {
  const int rate = 44100, seconds = 6;
  const size_t frames = size_t(rate) * seconds;
  std::vector<short> in(frames * 2);
  for (size_t i = 0; i < frames; i++) {
    double t = double(i) / rate;
    // syllables: 0.25 s of voice, 0.08 s of near silence
    double env = fmod(t, 0.33) < 0.25 ? sin(M_PI * fmod(t, 0.33) / 0.25) : 0.02;
    double v = 0;
    for (int h = 1; h <= 6; h++) v += sin(2 * M_PI * 150 * h * t) / h;
    short s = short(9000 * env * v / 2.45);
    in[i * 2] = in[i * 2 + 1] = s;
  }
  for (int percent : {75, 125, 150, 200, 250, 300}) {
    float speed = percent / 100.f;
    sonicStream st = sonicCreateStream(rate, 2);
    sonicSetSpeed(st, speed);
    std::vector<short> out, buf(8192 * 2);
    auto drain = [&] {
      for (;;) {
        int avail = sonicSamplesAvailable(st);
        if (avail <= 0) return;
        int got = sonicReadShortFromStream(st, buf.data(), std::min(avail, 8192));
        if (got <= 0) return;
        out.insert(out.end(), buf.begin(), buf.begin() + got * 2);
      }
    };
    for (size_t at = 0; at < frames; at += 1152) {
      int n = int(std::min<size_t>(1152, frames - at));
      sonicWriteShortToStream(st, &in[at * 2], n);
      drain();
    }
    sonicFlushStream(st);
    drain();
    sonicDestroyStream(st);
    size_t outFrames = out.size() / 2;
    double ratio = double(frames) / double(outFrames);
    // the pitch in a syllable's middle, early and late in the output
    double p1 = pitchOf(out, outFrames, rate, size_t(0.12 * rate / speed)), p2 = pitchOf(out, outFrames, rate, size_t(3.42 * rate / speed));
    bool ok = fabs(ratio - speed) < 0.03 * speed && fabs(p1 - 150) < 6 && fabs(p2 - 150) < 6;
    printf("%s %.2fx: %zu frames in, %zu out (%.3fx as short), pitch %.1f Hz and %.1f Hz\n", ok ? "ok  " : "FAIL", speed, frames, outFrames,
           ratio, p1, p2);
    if (!ok) failures++;
  }
  return failures ? 1 : 0;
}
