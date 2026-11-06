// compare: a 1.0.x reference against the native screen (private/native-plan.md, Parity method, step 4).
//
//   compare <1.0.png> <2.0.png> <out.png> [threshold [x y w h [scale]]]   (with a box: a zoomed crop)
//
// Writes out.png: the reference, the native screen, and the difference (differing pixels in red over a
// dimmed reference). Prints the share of pixels that differ by more than `threshold` (default 24 of
// 255) in any channel, and the boxes around them (8 px cells joined), so a 1 px shift reads as a line.
// Built by tools/parity/compare.sh with the Mac's libpng.
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

struct Image {
  int w = 0, h = 0;
  std::vector<unsigned char> px;  // RGBA
};

static bool load(const char* path, Image& img) {
  png_image p;
  memset(&p, 0, sizeof(p));
  p.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&p, path)) return false;
  p.format = PNG_FORMAT_RGBA;
  img.w = int(p.width), img.h = int(p.height);
  img.px.resize(PNG_IMAGE_SIZE(p));
  return png_image_finish_read(&p, nullptr, img.px.data(), 0, nullptr) != 0;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: compare <1.0.png> <2.0.png> <out.png> [threshold]\n");
    return 2;
  }
  Image a, b;
  if (!load(argv[1], a) || !load(argv[2], b)) {
    fprintf(stderr, "compare: cannot read the images\n");
    return 1;
  }
  int t = argc > 4 ? atoi(argv[4]) : 24;
  int w = std::min(a.w, b.w), h = std::min(a.h, b.h);
  Image out;
  out.w = w * 3 + 8, out.h = h;
  out.px.assign(size_t(out.w) * out.h * 4, 255);
  const int cell = 8;
  int cw = (w + cell - 1) / cell, ch = (h + cell - 1) / cell;
  std::vector<int> cells(size_t(cw) * ch, 0);
  long differing = 0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const unsigned char* pa = &a.px[(size_t(y) * a.w + x) * 4];
      const unsigned char* pb = &b.px[(size_t(y) * b.w + x) * 4];
      int d = std::max({abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])});
      bool diff = d > t;
      if (diff) differing++, cells[size_t(y / cell) * cw + x / cell]++;
      unsigned char* o = &out.px[(size_t(y) * out.w) * 4];
      memcpy(o + size_t(x) * 4, pa, 4);
      memcpy(o + size_t(w + 4 + x) * 4, pb, 4);
      unsigned char* od = o + size_t(2 * w + 8 + x) * 4;
      if (diff) {
        od[0] = 255, od[1] = 40, od[2] = 40;
      } else {
        int g = (pa[0] + pa[1] + pa[2]) / 3 / 3 + 40;
        od[0] = od[1] = od[2] = (unsigned char)g;
      }
      od[3] = 255;
    }
  }
  // join neighboring differing cells into boxes
  std::vector<int> label(cells.size(), -1);
  struct Box { int x0, y0, x1, y1; long n; };
  std::vector<Box> boxes;
  for (int cy = 0; cy < ch; cy++) {
    for (int cx = 0; cx < cw; cx++) {
      size_t i = size_t(cy) * cw + cx;
      if (!cells[i] || label[i] >= 0) continue;
      Box bx = {cx, cy, cx, cy, 0};
      std::vector<size_t> stack = {i};
      label[i] = int(boxes.size());
      while (!stack.empty()) {
        size_t k = stack.back();
        stack.pop_back();
        int kx = int(k % cw), ky = int(k / cw);
        bx.x0 = std::min(bx.x0, kx), bx.y0 = std::min(bx.y0, ky), bx.x1 = std::max(bx.x1, kx), bx.y1 = std::max(bx.y1, ky);
        bx.n += cells[k];
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++) {
            int nx = kx + dx, ny = ky + dy;
            if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) continue;
            size_t j = size_t(ny) * cw + nx;
            if (cells[j] && label[j] < 0) label[j] = int(boxes.size()), stack.push_back(j);
          }
      }
      boxes.push_back(bx);
    }
  }
  std::sort(boxes.begin(), boxes.end(), [](const Box& p, const Box& q) { return p.n > q.n; });
  printf("%s: %.2f %% of pixels differ (threshold %d)", argv[2], 100.0 * differing / (double(w) * h), t);
  printf(a.w != b.w || a.h != b.h ? "; sizes differ: %dx%d vs %dx%d\n" : "\n", a.w, a.h, b.w, b.h);
  for (size_t i = 0; i < boxes.size() && i < 12; i++)
    printf("  box x %d-%d, y %d-%d: %ld px\n", boxes[i].x0 * cell, std::min(w, (boxes[i].x1 + 1) * cell) - 1,
           boxes[i].y0 * cell, std::min(h, (boxes[i].y1 + 1) * cell) - 1, boxes[i].n);
  if (argc > 8) {  // a zoomed crop instead: x y w h [scale], nearest neighbor, reference above native
    int zx = atoi(argv[5]), zy = atoi(argv[6]), zw = atoi(argv[7]), zh = atoi(argv[8]), k = argc > 9 ? atoi(argv[9]) : 8;
    Image z;
    z.w = zw * k, z.h = (zh * 2 + 1) * k;
    z.px.assign(size_t(z.w) * z.h * 4, 255);
    for (int y = 0; y < z.h; y++)
      for (int x = 0; x < z.w; x++) {
        int sy = y / k, sx = zx + x / k;
        const Image& src = sy < zh ? a : b;
        int yy = zy + (sy < zh ? sy : sy - zh - 1);
        if (sy == zh || sx >= src.w || yy >= src.h) continue;
        memcpy(&z.px[(size_t(y) * z.w + x) * 4], &src.px[(size_t(yy) * src.w + sx) * 4], 4);
      }
    out = z;
  }
  png_image p;
  memset(&p, 0, sizeof(p));
  p.version = PNG_IMAGE_VERSION;
  p.width = unsigned(out.w), p.height = unsigned(out.h);
  p.format = PNG_FORMAT_RGBA;
  return png_image_write_to_file(&p, argv[3], 0, out.px.data(), 0, nullptr) ? 0 : 1;
}
