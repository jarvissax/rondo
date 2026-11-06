// The frame log: what every drawn frame cost, and every refresh missed while something moved. It is
// what tools/perf reads to check a run against the budgets in private/native-plan.md.
#pragma once
#include <stdint.h>
#include <stdio.h>

#include <vector>

namespace rondo::ui {

class FrameLog {
 public:
  struct Frame {
    int64_t vsync;     // the refresh this frame started at
    int32_t buildNs;   // scene to vertices (CPU)
    int32_t submitNs;  // GL calls
    int32_t swapNs;    // present
    int32_t inputNs;   // oldest input event handled in this frame: its age at the vsync (-1: none)
    int16_t draws, glyphs;
    int16_t uploads;   // textures created this frame
    int32_t quads;
    bool continued;    // the frame before it also moved (a gap since it counts as missed)
  };

  explicit FrameLog(int64_t period) : period_(period) { frames_.reserve(1 << 16); }
  void add(const Frame& f) {
    if (frames_.size() < frames_.capacity()) frames_.push_back(f);
  }
  void clear() { frames_.clear(); }
  // Prints the summary; also writes every frame as CSV when path is set.
  void report(FILE* out, const char* csvPath) const;

 private:
  int64_t period_;
  std::vector<Frame> frames_;
};

}  // namespace rondo::ui
