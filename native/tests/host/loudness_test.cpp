// Host test for the loudness meter (player/loudness.cpp), on the Mac:
//   clang++ -std=c++17 -O2 -I player tests/host/loudness_test.cpp player/loudness.cpp -o /tmp/lt && /tmp/lt
// EBU Tech 3341's first cases: a 1 kHz stereo sine at -23 dBFS reads -23.0 LUFS (±0.1), at -33 dBFS
// -33.0; the gates leave silence out.
#include <math.h>
#include <stdio.h>

#include <vector>

#include "loudness.h"
using namespace rondo;

static std::vector<int16_t> sine(double dbfs, double seconds, double hz = 1000) {
  int n = int(seconds * 44100);
  std::vector<int16_t> b(size_t(n) * 2);
  double a = pow(10.0, dbfs / 20.0) * 32767;
  for (int i = 0; i < n; i++) b[size_t(i) * 2] = b[size_t(i) * 2 + 1] = int16_t(lrint(a * sin(2 * M_PI * hz * i / 44100.0)));
  return b;
}

int main() {
  int fails = 0;
  auto check = [&](bool ok, const char* what, float v) {
    printf("%s %s: %.2f\n", ok ? "ok  " : "FAIL", what, v);
    fails += !ok;
  };
  LoudnessMeter m;
  std::vector<int16_t> s = sine(-23, 20);
  m.add(s.data(), int(s.size() / 2));
  check(fabsf(m.integrated() + 23) < 0.1f, "1 kHz at -23 dBFS reads -23 LUFS", m.integrated());
  m.reset();
  s = sine(-33, 20);
  m.add(s.data(), int(s.size() / 2));
  check(fabsf(m.integrated() + 33) < 0.1f, "1 kHz at -33 dBFS reads -33 LUFS", m.integrated());
  m.reset();  // EBU case 3: 10 s at -36, 60 s at -23, 10 s at -36: the relative gate gives -23
  for (auto [db, sec] : {std::pair<double, double>{-36, 10}, {-23, 60}, {-36, 10}}) {
    s = sine(db, sec);
    m.add(s.data(), int(s.size() / 2));
  }
  check(fabsf(m.integrated() + 23) < 0.1f, "quiet parts gated out (EBU case 3)", m.integrated());
  m.reset();
  std::vector<int16_t> silence(44100 * 2 * 5, 0);
  m.add(silence.data(), 44100 * 5);
  check(isnan(m.integrated()), "silence has no loudness", m.integrated());
  return fails;
}
