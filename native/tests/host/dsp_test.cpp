// Host tests for the sound processing (player/dsp.cpp), run on the Mac:
//   clang++ -std=c++17 -O2 -I player tests/host/dsp_test.cpp player/dsp.cpp -o /tmp/dsp_test && /tmp/dsp_test
// Each line says ok or FAIL; the exit code is the number of failures.
#include <math.h>
#include <stdio.h>
#include <time.h>

#include <algorithm>
#include <vector>
#include "dsp.h"
using namespace rondo;
// a sine through the DSP: the output's RMS over the last half second, and its peak
static void run(Dsp& d, double freq, double amp, double& rmsDb, int& peak, std::vector<int16_t>* keep = nullptr) {
  const int n = 44100 * 2;
  std::vector<int16_t> buf(n * 2);
  for (int i = 0; i < n; i++) buf[2 * i] = buf[2 * i + 1] = int16_t(lrint(amp * 32767 * sin(2 * M_PI * freq * i / 44100.0)));
  std::vector<int16_t> in = buf;
  for (int at = 0; at < n; at += 1024) d.process(buf.data() + at * 2, std::min(1024, n - at));
  double sum = 0, ref = 0;
  peak = 0;
  for (int i = n / 2; i < n; i++) {
    sum += double(buf[2 * i]) * buf[2 * i];
    ref += double(in[2 * i]) * in[2 * i];
    peak = std::max(peak, abs(int(buf[2 * i])));
  }
  rmsDb = 10 * log10(sum / ref);
  if (keep) *keep = buf;
}
int main() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; };
  {  // off: untouched
    Dsp d; d.configure(SoundSettings{});
    std::vector<int16_t> b(2048), c;
    for (int i = 0; i < 2048; i++) b[i] = int16_t(i * 37);
    c = b; d.process(b.data(), 1024);
    check(b == c && !d.active(), "off is bit for bit");
  }
  {  // +6 dB at 1 kHz
    Dsp d; SoundSettings s; s.filters.push_back({FilterType::Peak, 1000, 6, 1.41f}); d.configure(s);
    double db; int pk;
    run(d, 1000, 0.1, db, pk); check(fabs(db - 6) < 0.2, "peak +6 dB at its frequency"); printf("     %.2f dB\n", db);
    Dsp d2; d2.configure(s); run(d2, 100, 0.1, db, pk); check(fabs(db) < 0.3, "peak leaves 100 Hz alone"); printf("     %.2f dB\n", db);
  }
  {  // high-pass at 150 Hz
    Dsp d; SoundSettings s; s.filters.push_back({FilterType::HighPass, 150, 0, 0.707f}); d.configure(s);
    double db; int pk; run(d, 40, 0.3, db, pk); check(db < -20, "high-pass cuts 40 Hz"); printf("     %.2f dB\n", db);
    Dsp d2; d2.configure(s); run(d2, 2000, 0.3, db, pk); check(fabs(db) < 0.2, "high-pass passes 2 kHz"); printf("     %.2f dB\n", db);
  }
  {  // low shelf +8 dB at 100 Hz
    Dsp d; SoundSettings s; s.filters.push_back({FilterType::LowShelf, 100, 8, 0.7f}); d.configure(s);
    double db; int pk; run(d, 30, 0.05, db, pk); check(fabs(db - 8) < 1, "low shelf lifts 30 Hz by ~8 dB"); printf("     %.2f dB\n", db);
  }
  {  // a +12 dB boost on a loud sine: the limiter holds -1 dBFS
    Dsp d; SoundSettings s; s.gainDb = 12; d.configure(s);
    double db; int pk; run(d, 1000, 0.9, db, pk);
    check(pk <= int(0.8913 * 32767) + 2, "limiter: no sample past -1 dBFS"); printf("     peak %d (ceiling %d), reduction %.1f dB\n", pk, int(0.8913 * 32767), d.reductionDb());
    Dsp d2; d2.configure(s); run(d2, 1000, 0.05, db, pk); check(fabs(db - 12) < 0.2, "a quiet sine gets the full +12 dB"); printf("     %.2f dB\n", db);
  }
  {  // a sudden loud burst after quiet: no overshoot
    Dsp d; SoundSettings s; s.gainDb = 9; d.configure(s);
    const int n = 44100; std::vector<int16_t> b(n * 2);
    for (int i = 0; i < n; i++) { double a = i < n / 2 ? 0.01 : 0.95; b[2*i] = b[2*i+1] = int16_t(lrint(a * 32767 * sin(2 * M_PI * 3000 * i / 44100.0))); }
    for (int at = 0; at < n; at += 1024) d.process(b.data() + at * 2, std::min(1024, n - at));
    int pk = 0; for (int i = 0; i < n * 2; i++) pk = std::max(pk, abs(int(b[i])));
    check(pk <= int(0.8913 * 32767) + 2, "limiter: a burst after quiet doesn't overshoot"); printf("     peak %d\n", pk);
  }
  {  // leveling: a track gain of -6 dB with +2 target
    Dsp d; SoundSettings s; s.leveling = true; s.levelingTargetDb = 2; d.configure(s); d.setTrackGain(-6);
    double db; int pk; run(d, 1000, 0.1, db, pk); check(fabs(db + 4) < 0.2, "leveling applies track gain + target"); printf("     %.2f dB\n", db);
  }
  {  // speed: 10 bands over a minute of sound
    Dsp d; SoundSettings s; float f[10] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    for (float x : f) s.filters.push_back({FilterType::Peak, x, 3, 1.41f});
    s.gainDb = 6; d.configure(s);
    std::vector<int16_t> b(44100 * 60 * 2, 1000);
    clock_t t = clock(); for (size_t at = 0; at < b.size() / 2; at += 1024) d.process(b.data() + at * 2, 1024);
    printf("     a minute of 10 bands + limiter: %.0f ms on this Mac\n", (clock() - t) * 1000.0 / CLOCKS_PER_SEC);
  }
  return fails;
}
