// A song's loudness as EBU R128 / ITU-R BS.1770 measure it (integrated, LUFS): K-weighting, 400 ms
// blocks every 100 ms, the absolute gate at -70 LUFS and the relative one 10 LU under. For Settings ›
// Sound › Volume leveling, since most files carry no ReplayGain: measured as a song plays (and, while
// the Y1 charges, through the library), kept in a small file.
#pragma once
#include <stdint.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace rondo {

class LoudnessMeter {
 public:
  LoudnessMeter();
  void reset();
  // 44.1 kHz stereo
  void add(const int16_t* stereo, int frames);
  // LUFS; NAN under a second of sound, or all of it silence
  float integrated() const;

 private:
  struct Stage {
    double b0, b1, b2, a1, a2;
    double z1[2], z2[2];
  };
  Stage shelf_, highpass_;
  double sub_ = 0;  // this 100 ms's sum of squares
  int subFrames_ = 0;
  std::vector<double> subs_;    // every 100 ms's mean square
  std::vector<double> blocks_;  // every 400 ms block's mean square, 100 ms apart
};

// path → LUFS, a line a song ("-9.84\t/storage/sdcard0/Music/…"), appended as songs are measured, the
// last line of a path winning
class LoudnessCache {
 public:
  void open(const std::string& file);
  bool find(const std::string& path, float& lufs) const;
  void put(const std::string& path, float lufs);
  size_t size() const { return map_.size(); }

 private:
  std::string file_;
  std::unordered_map<std::string, float> map_;
};

}  // namespace rondo
