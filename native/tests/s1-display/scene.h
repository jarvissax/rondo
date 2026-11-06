// The S1 test scene and its GL helpers, shared by s1 (through SurfaceFlinger) and s1fb (straight to
// the framebuffer).
#pragma once
#include <GLES2/gl2.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <algorithm>
#include <vector>

namespace {


int64_t now_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

double ms(int64_t ns) { return ns / 1e6; }

// Percentile of a copy of the samples (nanoseconds), in milliseconds.
double pct(std::vector<int64_t> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t i = std::min(v.size() - 1, size_t(p / 100.0 * (v.size() - 1) + 0.5));
  return ms(v[i]);
}

const char* kVertex =
    "attribute vec2 a_pos;\n"
    "attribute vec4 a_color;\n"
    "uniform vec2 u_scale;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos.x * u_scale.x - 1.0, 1.0 - a_pos.y * u_scale.y, 0.0, 1.0);\n"
    "  v_color = a_color;\n"
    "}\n";

const char* kFragment =
    "precision mediump float;\n"
    "varying vec4 v_color;\n"
    "void main() { gl_FragColor = v_color; }\n";

GLuint shader(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    fprintf(stderr, "s1: shader: %s\n", log);
    exit(1);
  }
  return s;
}

struct Vertex {
  float x, y;
  float r, g, b, a;
};

void rect(std::vector<Vertex>& v, float x, float y, float w, float h, float r, float g, float b,
          float a = 1) {
  Vertex q[6] = {{x, y, r, g, b, a},     {x + w, y, r, g, b, a},     {x, y + h, r, g, b, a},
                 {x + w, y, r, g, b, a}, {x + w, y + h, r, g, b, a}, {x, y + h, r, g, b, a}};
  v.insert(v.end(), q, q + 6);
}

// The test scene: 821 rows (the size of the card library here) scrolling under a selection bar that
// follows the wheel with a critically damped spring, a square moving 2 px per refresh along the top
// (a stutter shows as a jump), and a strip that changes color every frame (a dropped frame shows).
struct Scene {
  static constexpr int kRows = 821;
  static constexpr float kRowH = 50;
  int selected = 0;
  float bar = 0, barVel = 0;  // selection bar position (rows) and velocity
  int64_t frames = 0;

  void step(double dt) {
    // critically damped spring toward the selected row, ~110 ms to settle
    const double w = 2 * M_PI / 0.22;
    double x = bar - selected;
    double a = -w * w * x - 2 * w * barVel;
    barVel += a * dt;
    bar += barVel * dt;
  }

  void build(std::vector<Vertex>& v, float W, float H) {
    v.clear();
    // keep the bar in the middle third: scroll so the bar's row sits there
    float scroll = bar * kRowH - H / 2 + kRowH / 2;
    int first = std::max(0, int(scroll / kRowH));
    for (int i = first; i < kRows && i * kRowH - scroll < H; i++) {
      float y = i * kRowH - scroll;
      float shade = (i % 2) ? 0.10f : 0.14f;
      rect(v, 0, y, W, kRowH - 1, shade, shade, shade + 0.02f);
      // a "thumbnail" and two "text" bars per row
      rect(v, 8, y + 6, 38, 38, 0.2f + (i * 37 % 100) / 200.0f, 0.3f, 0.5f);
      rect(v, 56, y + 12, 180 + (i * 53 % 160), 10, 0.85f, 0.85f, 0.85f);
      rect(v, 56, y + 30, 120 + (i * 29 % 120), 7, 0.55f, 0.55f, 0.6f);
    }
    float by = bar * kRowH - scroll;
    rect(v, 0, by, W, kRowH - 1, 0.20f, 0.45f, 0.95f, 0.55f);
    float x = float((frames * 2) % int(W - 20));
    rect(v, x, 0, 20, 6, 1, 0.8f, 0.1f);
    float c = (frames % 2) ? 1.0f : 0.0f;
    rect(v, W - 12, H - 12, 12, 12, c, 1 - c, 0);
  }
};

}  // namespace
