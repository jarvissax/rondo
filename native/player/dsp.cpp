// Dsp (dsp.h).
#include "dsp.h"

#include <string.h>

#include <algorithm>

namespace rondo {
namespace {
constexpr float kCeiling = 0.8913f;  // -1 dBFS: what the limiter lets through
}

// Robert Bristow-Johnson's Audio EQ Cookbook, the filters graphic equalizers and AutoEq describe
Dsp::Biquad Dsp::design(const Filter& f) {
  double freq = std::max(10.0, std::min(double(f.freq), 0.45 * kRate));
  double q = std::max(0.1, double(f.q));
  double A = pow(10.0, f.gainDb / 40.0), w0 = 2 * M_PI * freq / kRate, cs = cos(w0), sn = sin(w0), alpha = sn / (2 * q);
  double sa = 2 * sqrt(A) * alpha;
  double b0, b1, b2, a0, a1, a2;
  switch (f.type) {
    case FilterType::LowShelf:
      b0 = A * ((A + 1) - (A - 1) * cs + sa), b1 = 2 * A * ((A - 1) - (A + 1) * cs), b2 = A * ((A + 1) - (A - 1) * cs - sa);
      a0 = (A + 1) + (A - 1) * cs + sa, a1 = -2 * ((A - 1) + (A + 1) * cs), a2 = (A + 1) + (A - 1) * cs - sa;
      break;
    case FilterType::HighShelf:
      b0 = A * ((A + 1) + (A - 1) * cs + sa), b1 = -2 * A * ((A - 1) + (A + 1) * cs), b2 = A * ((A + 1) + (A - 1) * cs - sa);
      a0 = (A + 1) - (A - 1) * cs + sa, a1 = 2 * ((A - 1) - (A + 1) * cs), a2 = (A + 1) - (A - 1) * cs - sa;
      break;
    case FilterType::HighPass:
      b0 = (1 + cs) / 2, b1 = -(1 + cs), b2 = (1 + cs) / 2, a0 = 1 + alpha, a1 = -2 * cs, a2 = 1 - alpha;
      break;
    case FilterType::LowPass:
      b0 = (1 - cs) / 2, b1 = 1 - cs, b2 = (1 - cs) / 2, a0 = 1 + alpha, a1 = -2 * cs, a2 = 1 - alpha;
      break;
    default:  // peaking
      b0 = 1 + alpha * A, b1 = -2 * cs, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * cs, a2 = 1 - alpha / A;
      break;
  }
  Biquad b;
  b.b0 = b0 / a0, b.b1 = b1 / a0, b.b2 = b2 / a0, b.a1 = a1 / a0, b.a2 = a2 / a0;
  return b;
}

void Dsp::configure(const SoundSettings& s) {
  bool was = active_;
  // a filter that stays keeps its state: changing one band doesn't click the others
  std::vector<Biquad> eq;
  for (const Filter& f : s.filters) {
    if (f.type != FilterType::HighPass && f.type != FilterType::LowPass && f.gainDb == 0) continue;
    Biquad b = design(f);
    size_t i = eq.size();
    if (i < eq_.size()) memcpy(b.z1, eq_[i].z1, sizeof(b.z1)), memcpy(b.z2, eq_[i].z2, sizeof(b.z2));
    eq.push_back(b);
  }
  eq_ = std::move(eq);
  s_ = s;
  active_ = s.active();
  if (active_ && !was) {  // starting: the limiter empty, the gain from where it is
    memset(delay_, 0, sizeof(delay_));
    for (int i = 0; i < kLook; i++) required_[i] = 1, box_[i] = 1;
    boxSum_ = kLook, held_ = 1, delayAt_ = 0, minVal_ = 1, minAt_ = 0;
    gain_ = targetGain();
    for (Biquad& b : eq_) b.z1[0] = b.z1[1] = b.z2[0] = b.z2[1] = 0;
  }
}

float Dsp::targetGain() const {
  float db = s_.gainDb;
  if (s_.leveling && !std::isnan(trackDb_)) db += trackDb_ + s_.levelingTargetDb;
  return powf(10.f, db / 20.f);
}

void Dsp::process(int16_t* io, int frames) {
  if (!active_) return;
  const float target = targetGain(), smooth = 1.f - expf(-1.f / (0.01f * kRate));  // gain changes glide over 10 ms
  const float release = 1.f - expf(-1.f / (0.1f * kRate));         // the limiter lets go over 100 ms
  float least = 1;
  for (int i = 0; i < frames; i++) {
    if (gain_ != target) {
      gain_ += (target - gain_) * smooth;
      if (fabsf(target - gain_) < 1e-5f) gain_ = target;
    }
    float x[2] = {io[2 * i] * (1.f / 32768.f) * gain_, io[2 * i + 1] * (1.f / 32768.f) * gain_};
    for (Biquad& b : eq_)
      for (int c = 0; c < 2; c++) {  // transposed direct form II, in double for the low bands
        double in = x[c], y = b.b0 * in + b.z1[c];
        b.z1[c] = b.b1 * in - b.a1 * y + b.z2[c];
        b.z2[c] = b.b2 * in - b.a2 * y;
        x[c] = float(y);
      }
    // The limiter: each sample's needed gain, its minimum over the look-ahead, released slowly, then
    // averaged over the look-ahead and applied to the sample that look-ahead ago, which so never
    // passes the ceiling.
    float peak = std::max(fabsf(x[0]), fabsf(x[1]));
    // the sliding minimum: looked for again only when the one leaving the window was it
    float need = peak > kCeiling ? kCeiling / peak : 1.f;
    bool left = minAt_ == delayAt_ && need > minVal_;
    required_[delayAt_] = need;
    if (need <= minVal_) {
      minVal_ = need, minAt_ = delayAt_;
    } else if (left) {
      minVal_ = 1, minAt_ = delayAt_;
      for (int k = 0; k < kLook; k++)
        if (required_[k] < minVal_) minVal_ = required_[k], minAt_ = k;
    }
    float m = minVal_;
    held_ = m < held_ ? m : held_ + (m - held_) * release;
    boxSum_ += held_ - box_[delayAt_];
    box_[delayAt_] = held_;
    float g = std::min(1.f, boxSum_ * (1.f / kLook));
    least = std::min(least, g);
    delay_[delayAt_][0] = x[0], delay_[delayAt_][1] = x[1];
    delayAt_ = (delayAt_ + 1) % kLook;
    const float* d = delay_[delayAt_];  // the oldest: look-ahead minus one samples ago
    for (int c = 0; c < 2; c++) {
      float y = d[c] * g * 32767.f;
      if (y != 0) {  // TPDF dither, one step wide; digital silence stays silent
        rng_ = rng_ * 1664525u + 1013904223u;
        float r1 = float(rng_ >> 8) * (1.f / 16777216.f);
        rng_ = rng_ * 1664525u + 1013904223u;
        float r2 = float(rng_ >> 8) * (1.f / 16777216.f);
        y += r1 - r2;
      }
      int v = int(lrintf(y));
      io[2 * i + c] = int16_t(std::max(-32768, std::min(32767, v)));
    }
  }
  reductionDb_ = 20.f * log10f(std::max(least, 1e-4f));
}

}  // namespace rondo

namespace rondo {

float Dsp::responseDb(const Filter& f, float hz) {
  if (f.type != FilterType::HighPass && f.type != FilterType::LowPass && f.gainDb == 0) return 0;
  Biquad b = design(f);
  double w = 2 * M_PI * hz / kRate;
  // |H(e^jw)| = |b0 + b1 z^-1 + b2 z^-2| / |1 + a1 z^-1 + a2 z^-2|
  double nr = b.b0 + b.b1 * cos(w) + b.b2 * cos(2 * w), ni = -(b.b1 * sin(w) + b.b2 * sin(2 * w));
  double dr = 1 + b.a1 * cos(w) + b.a2 * cos(2 * w), di = -(b.a1 * sin(w) + b.a2 * sin(2 * w));
  return float(10 * log10((nr * nr + ni * ni) / (dr * dr + di * di)));
}

}  // namespace rondo
