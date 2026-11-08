// LoudnessMeter and LoudnessCache (loudness.h).
#include "loudness.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

namespace rondo {
namespace {
constexpr double kRate = 44100;
constexpr int kSubFrames = 4410;  // 100 ms
}

// BS.1770's two stages for any rate, as libebur128 derives them: the head's high shelf, then the
// revised low-frequency B high-pass
LoudnessMeter::LoudnessMeter() {
  double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
  double K = tan(M_PI * f0 / kRate), Vh = pow(10.0, G / 20.0), Vb = pow(Vh, 0.4996667741545416);
  double a0 = 1.0 + K / Q + K * K;
  shelf_ = {(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0, 2.0 * (K * K - 1.0) / a0,
            (1.0 - K / Q + K * K) / a0, {0, 0}, {0, 0}};
  f0 = 38.13547087602444, Q = 0.5003270373238773, K = tan(M_PI * f0 / kRate);
  a0 = 1.0 + K / Q + K * K;
  // b: 1, -2, 1, unnormalized, as BS.1770 gives it
  highpass_ = {1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0, {0, 0}, {0, 0}};
  reset();
}

void LoudnessMeter::reset() {
  for (Stage* s : {&shelf_, &highpass_}) s->z1[0] = s->z1[1] = s->z2[0] = s->z2[1] = 0;
  sub_ = 0, subFrames_ = 0;
  subs_.clear(), blocks_.clear();
}

void LoudnessMeter::add(const int16_t* stereo, int frames) {
  for (int i = 0; i < frames; i++) {
    for (int c = 0; c < 2; c++) {
      double x = stereo[2 * i + c] / 32768.0;
      for (Stage* s : {&shelf_, &highpass_}) {
        double y = s->b0 * x + s->z1[c];
        s->z1[c] = s->b1 * x - s->a1 * y + s->z2[c];
        s->z2[c] = s->b2 * x - s->a2 * y;
        x = y;
      }
      sub_ += x * x;  // left and right weigh 1.0
    }
    if (++subFrames_ == kSubFrames) {
      subs_.push_back(sub_ / kSubFrames);
      sub_ = 0, subFrames_ = 0;
      size_t n = subs_.size();
      if (n >= 4) blocks_.push_back((subs_[n - 1] + subs_[n - 2] + subs_[n - 3] + subs_[n - 4]) / 4);
    }
  }
}

float LoudnessMeter::integrated() const {
  if (blocks_.size() < 7) return NAN;  // under a second
  auto lufs = [](double ms) { return -0.691 + 10.0 * log10(ms); };
  const double absolute = pow(10.0, (-70.0 + 0.691) / 10.0);
  double sum = 0;
  size_t n = 0;
  for (double b : blocks_)
    if (b > absolute) sum += b, n++;
  if (!n) return NAN;
  double relative = pow(10.0, (lufs(sum / double(n)) - 10.0 + 0.691) / 10.0);
  sum = 0, n = 0;
  for (double b : blocks_)
    if (b > absolute && b > relative) sum += b, n++;
  return n ? float(lufs(sum / double(n))) : NAN;
}

void LoudnessCache::open(const std::string& file) {
  file_ = file;
  map_.clear();
  FILE* f = fopen(file.c_str(), "r");
  if (!f) return;
  char line[1200];
  while (fgets(line, sizeof(line), f)) {
    char* tab = strchr(line, '\t');
    if (!tab) continue;
    std::string path = tab + 1;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    if (!path.empty()) map_[path] = float(atof(line));
  }
  fclose(f);
}

bool LoudnessCache::find(const std::string& path, float& lufs) const {
  auto it = map_.find(path);
  if (it == map_.end()) return false;
  lufs = it->second;
  return true;
}

void LoudnessCache::put(const std::string& path, float lufs) {
  if (isnan(lufs) || path.empty() || path.find('\n') != std::string::npos) return;
  map_[path] = lufs;
  if (file_.empty()) return;
  int fd = ::open(file_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) return;
  char head[32];
  int n = snprintf(head, sizeof(head), "%.2f\t", lufs);
  std::string line = std::string(head, size_t(n)) + path + "\n";
  (void)!write(fd, line.data(), line.size());
  close(fd);
}

}  // namespace rondo
