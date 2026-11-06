#include "frame_log.h"

#include <algorithm>

namespace rondo::ui {
namespace {

double pct(std::vector<int64_t> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t i = std::min(v.size() - 1, size_t(p / 100.0 * double(v.size() - 1) + 0.5));
  return double(v[i]) / 1e6;
}

}  // namespace

void FrameLog::report(FILE* out, const char* csvPath) const {
  std::vector<int64_t> cpu, total, input;
  int64_t missed = 0, moving = 0;
  for (size_t i = 0; i < frames_.size(); i++) {
    const Frame& f = frames_[i];
    cpu.push_back(int64_t(f.buildNs) + f.submitNs);
    total.push_back(int64_t(f.buildNs) + f.submitNs + f.swapNs);
    if (f.inputNs >= 0) input.push_back(f.inputNs);
    if (i > 0 && f.continued) {
      moving++;
      int64_t gap = f.vsync - frames_[i - 1].vsync;
      if (gap > period_ * 3 / 2) missed += (gap + period_ / 2) / period_ - 1;
    }
  }
  fprintf(out, "frames %zu (%lld while moving), missed refreshes while moving: %lld\n", frames_.size(),
          (long long)moving, (long long)missed);
  fprintf(out, "cpu per frame p50 %.2f ms, p99 %.2f, max %.2f; with present p50 %.2f, p99 %.2f, max %.2f\n",
          pct(cpu, 50), pct(cpu, 99), pct(cpu, 100), pct(total, 50), pct(total, 99), pct(total, 100));
  if (!input.empty())
    fprintf(out, "input event to its frame's start: p50 %.2f ms, p99 %.2f, max %.2f (%zu frames)\n", pct(input, 50),
            pct(input, 99), pct(input, 100), input.size());
  if (!csvPath) return;
  FILE* csv = fopen(csvPath, "w");
  if (!csv) return;
  fprintf(csv, "vsync_ms,build_ms,submit_ms,swap_ms,input_ms,draws,quads,glyphs,continued,uploads\n");
  int64_t t0 = frames_.empty() ? 0 : frames_[0].vsync;
  for (const Frame& f : frames_)
    fprintf(csv, "%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d\n", (f.vsync - t0) / 1e6, f.buildNs / 1e6, f.submitNs / 1e6,
            f.swapNs / 1e6, f.inputNs / 1e6, f.draws, f.quads, f.glyphs, f.continued ? 1 : 0, f.uploads);
  fclose(csv);
}

}  // namespace rondo::ui
