// Rondo's sound (Settings › Sound), applied to everything rondo-player sends out (music, a video's
// sound, the radio) just before OpenSL: an equalizer (up to 16 filters: peaking, shelves, high- and
// low-pass, as graphic presets and AutoEq's parametric ones use), a gain (the preset's preamp, the
// loudness boost, a song's leveling), and a look-ahead limiter that keeps the result under -1 dBFS, so
// a boost makes things louder without ever clipping. 44.1 kHz stereo, 16-bit in and out (TPDF
// dithered). Off (no filters, no gain), it isn't run at all: the sound is bit for bit 1.0.x's.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace rondo {

enum class FilterType : uint8_t { Peak, LowShelf, HighShelf, HighPass, LowPass };

struct Filter {
  FilterType type = FilterType::Peak;
  float freq = 1000, gainDb = 0, q = 1.41f;
};

struct SoundSettings {
  std::vector<Filter> filters;
  float gainDb = 0;          // the preamp plus the loudness boost
  bool leveling = false;     // songs' ReplayGain (or the server's loudness) applied
  float levelingTargetDb = 0;  // added to a song's ReplayGain gain (louder than its -18 LUFS reference)
  bool active() const {
    if (gainDb != 0 || leveling) return true;
    for (const Filter& f : filters)
      if (f.type == FilterType::HighPass || f.type == FilterType::LowPass || f.gainDb != 0) return true;
    return false;
  }
};

class Dsp {
 public:
  static constexpr int kRate = 44100;
  void configure(const SoundSettings& s);
  // the song now heard: its ReplayGain track gain in dB (NAN: none)
  void setTrackGain(float db) { trackDb_ = db; }
  bool leveling() const { return s_.leveling; }  // songs' gains applied (a crossfade matches the two)
  bool active() const { return active_; }
  // in place, frames of interleaved stereo
  void process(int16_t* io, int frames);
  // a filter's response at a frequency, dB (the equalizer screen draws the sum)
  static float responseDb(const Filter& f, float hz);
  // the limiter's gain reduction lately, dB (for the logs)
  float reductionDb() const { return reductionDb_; }

 private:
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};
  };
  static Biquad design(const Filter& f);
  float targetGain() const;

  bool active_ = false;
  SoundSettings s_;
  std::vector<Biquad> eq_;
  float trackDb_ = NAN;
  float gain_ = 1;  // linear, ramped toward targetGain() over 10 ms
  // the limiter: L samples of look-ahead
  static constexpr int kLook = 64;
  float delay_[kLook][2] = {};
  int delayAt_ = 0;
  float required_[kLook] = {};  // each sample's needed gain, for the sliding minimum
  float boxSum_ = kLook, box_[kLook];
  float held_ = 1;  // the released minimum
  float minVal_ = 1;  // the sliding minimum of required_, and where it is
  int minAt_ = 0;
  float reductionDb_ = 0;
  uint32_t rng_ = 22222;
};

}  // namespace rondo

namespace rondo {

// SoundSettings over the player's socket (kSound): f32 gain, u8 leveling, f32 leveling target, u8 n,
// then n times u8 type, f32 frequency, f32 gain, f32 Q
inline std::vector<uint8_t> encodeSound(const SoundSettings& s) {
  std::vector<uint8_t> b;
  auto f32 = [&](float v) {
    uint8_t x[4];
    memcpy(x, &v, 4);
    b.insert(b.end(), x, x + 4);
  };
  f32(s.gainDb);
  b.push_back(s.leveling);
  f32(s.levelingTargetDb);
  size_t n = std::min<size_t>(s.filters.size(), 16);
  b.push_back(uint8_t(n));
  for (size_t i = 0; i < n; i++) {
    b.push_back(uint8_t(s.filters[i].type));
    f32(s.filters[i].freq), f32(s.filters[i].gainDb), f32(s.filters[i].q);
  }
  return b;
}
inline bool decodeSound(const uint8_t* p, size_t len, SoundSettings& s) {
  size_t at = 0;
  auto f32 = [&](float& v) {
    if (at + 4 > len) return false;
    memcpy(&v, p + at, 4);
    at += 4;
    return true;
  };
  s = SoundSettings();
  if (!f32(s.gainDb) || at >= len) return false;
  s.leveling = p[at++] != 0;
  if (!f32(s.levelingTargetDb) || at >= len) return false;
  int n = p[at++];
  for (int i = 0; i < n; i++) {
    if (at >= len) return false;
    Filter f;
    f.type = FilterType(std::min<uint8_t>(p[at++], uint8_t(FilterType::LowPass)));
    if (!f32(f.freq) || !f32(f.gainDb) || !f32(f.q)) return false;
    s.filters.push_back(f);
  }
  return true;
}

}  // namespace rondo
