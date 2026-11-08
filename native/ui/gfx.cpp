#include "gfx.h"

#include <GLES2/gl2ext.h>
#include <png.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace rondo::ui {
namespace {

// Mali-400's fragment shaders only have mediump (fp16): every value here stays well inside its range
// at 480x360 (length() only ever sees the corner's few pixels).
const char* kVertex = R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;
attribute vec2 a_local;
attribute vec3 a_shape;
uniform vec2 u_scale;
varying vec2 v_uv;
varying vec4 v_color;
varying vec2 v_local;
varying vec3 v_shape;
void main() {
  gl_Position = vec4(a_pos.x * u_scale.x - 1.0, 1.0 - a_pos.y * u_scale.y, 0.0, 1.0);
  v_uv = a_uv;
  v_color = a_color;
  v_local = a_local;
  v_shape = a_shape;
}
)";

const char* kFillFs = R"(
precision mediump float;
varying vec4 v_color;
void main() { gl_FragColor = v_color; }
)";

// A rounded rectangle's signed distance; coverage is the half pixel around the edge.
const char* kShapeFs = R"(
precision mediump float;
varying vec4 v_color;
varying vec2 v_local;
varying vec3 v_shape;
void main() {
  vec2 q = abs(v_local) - v_shape.xy + v_shape.z;
  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_shape.z;
  gl_FragColor = v_color * clamp(0.5 - d, 0.0, 1.0);
}
)";

const char* kImageFs = R"(
precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
varying vec4 v_color;
varying vec2 v_local;
varying vec3 v_shape;
void main() {
  vec2 q = abs(v_local) - v_shape.xy + v_shape.z;
  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_shape.z;
  gl_FragColor = texture2D(u_tex, v_uv) * v_color * clamp(0.5 - d, 0.0, 1.0);
}
)";

const char* kTextFs = R"(
precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
varying vec4 v_color;
void main() { gl_FragColor = v_color * texture2D(u_tex, v_uv).a; }
)";

// A square image: no corner mask, so a full-screen backdrop costs one texture read per pixel.
const char* kImagePlainFs = R"(
precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
varying vec4 v_color;
void main() { gl_FragColor = texture2D(u_tex, v_uv) * v_color; }
)";

// A video frame from a SurfaceTexture: MediaTek's decoder format, sampled by the GPU as it is (S2)
const char* kVideoFs = R"(#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES u_tex;
varying vec2 v_uv;
varying vec4 v_color;
void main() { gl_FragColor = texture2D(u_tex, v_uv) * v_color; }
)";

GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    fprintf(stderr, "gfx: shader: %s\n", log);
    return 0;
  }
  return s;
}

GLuint program(GLuint vs, const char* fragment, int w, int h) {
  GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
  if (!fs) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  const char* names[] = {"a_pos", "a_uv", "a_color", "a_local", "a_shape"};
  for (int i = 0; i < 5; i++) glBindAttribLocation(p, i, names[i]);
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetProgramInfoLog(p, sizeof(log), nullptr, log);
    fprintf(stderr, "gfx: link: %s\n", log);
    return 0;
  }
  glUseProgram(p);
  glUniform2f(glGetUniformLocation(p, "u_scale"), 2.0f / w, 2.0f / h);
  GLint tex = glGetUniformLocation(p, "u_tex");
  if (tex >= 0) glUniform1i(tex, 0);
  return p;
}

constexpr uint32_t kMaxQuadsPerDraw = 16383;  // 16-bit indices

}  // namespace

Color mix(Color a, Color b, float t) {
  auto ch = [&](int s) {
    float x = float((a >> s) & 0xff), y = float((b >> s) & 0xff);
    return uint32_t(x + (y - x) * t + 0.5f) << s;
  };
  return ch(24) | ch(16) | ch(8) | ch(0);
}

Texture createTexture(int w, int h, GLenum format, GLenum type, const void* pixels, bool linear) {
  Texture t;
  t.w = w;
  t.h = h;
  glGenTextures(1, &t.id);
  glBindTexture(GL_TEXTURE_2D, t.id);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, type, pixels);
  return t;
}

void destroyTexture(Texture& t) {
  if (t.id) glDeleteTextures(1, &t.id);
  t = Texture();
}

bool Canvas::init(int width, int height) {
  w_ = width;
  h_ = height;
  GLuint vs = compile(GL_VERTEX_SHADER, kVertex);
  if (!vs) return false;
  const char* fragments[kPipes] = {kFillFs, kShapeFs, kImageFs, kTextFs, kImagePlainFs, kVideoFs};
  for (int i = 0; i < kPipes; i++) {
    programs_[i] = program(vs, fragments[i], width, height);
    if (!programs_[i]) return false;
  }
  std::vector<uint16_t> idx(kMaxQuadsPerDraw * 6);
  for (uint32_t q = 0; q < kMaxQuadsPerDraw; q++) {
    uint16_t v = uint16_t(q * 4);
    uint16_t* i = &idx[q * 6];
    i[0] = v, i[1] = v + 1, i[2] = v + 2, i[3] = v + 2, i[4] = v + 1, i[5] = v + 3;
  }
  glGenBuffers(1, &ibo_);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 2, idx.data(), GL_STATIC_DRAW);
  glGenBuffers(3, vbo_);
  // Each buffer is allocated once, big enough for any screen (a buffer that grows makes the driver
  // allocate while the GPU still draws the last frame: the next frame waited a whole refresh, N3 log).
  vboCap_ = 16384;
  for (GLuint b : vbo_) {
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, vboCap_ * sizeof(Vertex), nullptr, GL_STREAM_DRAW);
  }
  for (auto& l : layers_) {
    l.verts.reserve(4096);
    l.cmds.reserve(64);
  }
  upload_.reserve(16384);
  for (int i = 0; i < 5; i++) glEnableVertexAttribArray(i);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_DITHER);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_SCISSOR_TEST);
  glActiveTexture(GL_TEXTURE0);
  return true;
}

void Canvas::begin(Color clear) {
  for (auto& l : layers_) {
    l.verts.clear();
    l.cmds.clear();
  }
  layer_ = 0;
  offX_.assign(1, 0);
  offY_.assign(1, 0);
  alpha_.assign(1, 1);
  clip_.assign(1, Clip{0, 0, int16_t(w_), int16_t(h_)});
  glViewport(0, 0, w_, h_);
  glScissor(0, 0, w_, h_);
  glClearColor(((clear >> 16) & 0xff) / 255.f, ((clear >> 8) & 0xff) / 255.f, (clear & 0xff) / 255.f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}

void Canvas::end() {
  size_t total = 0;
  for (auto& l : layers_) total += l.verts.size();
  draws_ = 0;
  quads_ = int(total / 4);
  if (!total) return;
  upload_.resize(total);
  size_t base[kLayers];
  size_t at = 0;
  for (int i = 0; i < kLayers; i++) {
    base[i] = at;
    if (!layers_[i].verts.empty()) memcpy(&upload_[at], layers_[i].verts.data(), layers_[i].verts.size() * sizeof(Vertex));
    at += layers_[i].verts.size();
  }
  // three buffers in turn, so this frame's upload never waits for the GPU to finish the last one's
  if (total > vboCap_) {  // more than ever planned for: grow all three, once
    while (vboCap_ < total) vboCap_ *= 2;
    for (GLuint b : vbo_) {
      glBindBuffer(GL_ARRAY_BUFFER, b);
      glBufferData(GL_ARRAY_BUFFER, vboCap_ * sizeof(Vertex), nullptr, GL_STREAM_DRAW);
    }
  }
  glBindBuffer(GL_ARRAY_BUFFER, vbo_[vboNext_]);
  vboNext_ = (vboNext_ + 1) % 3;
  glBufferSubData(GL_ARRAY_BUFFER, 0, total * sizeof(Vertex), upload_.data());
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
  int pipe = -1;
  GLuint tex = 0;
  Clip clip = {0, 0, int16_t(w_), int16_t(h_)};
  for (int li = 0; li < kLayers; li++) {
    for (const Cmd& c : layers_[li].cmds) {
      if (c.pipe != pipe) {
        pipe = c.pipe;
        glUseProgram(programs_[pipe]);
      }
      if (c.tex && c.tex != tex) {
        tex = c.tex;
        glBindTexture(c.pipe == kVideo ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, tex);
      }
      if (!(c.clip == clip)) {
        clip = c.clip;
        glScissor(clip.x0, h_ - clip.y1, std::max(0, clip.x1 - clip.x0), std::max(0, clip.y1 - clip.y0));
      }
      for (uint32_t done = 0; done < c.count; done += kMaxQuadsPerDraw) {
        uint32_t n = std::min(kMaxQuadsPerDraw, c.count - done);
        const char* v0 = (const char*)((base[li] + (c.first + done) * 4) * sizeof(Vertex));
        constexpr GLsizei s = sizeof(Vertex);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, s, v0 + offsetof(Vertex, x));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, s, v0 + offsetof(Vertex, u));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, s, v0 + offsetof(Vertex, rgba));
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, s, v0 + offsetof(Vertex, lx));
        glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, s, v0 + offsetof(Vertex, hw));
        glDrawElements(GL_TRIANGLES, GLsizei(n * 6), GL_UNSIGNED_SHORT, nullptr);
        draws_++;
      }
    }
  }
}

void Canvas::pushOffset(float dx, float dy) {
  offX_.push_back(offX_.back() + dx);
  offY_.push_back(offY_.back() + dy);
}
void Canvas::popOffset() {
  if (offX_.size() > 1) offX_.pop_back(), offY_.pop_back();
}
void Canvas::pushAlpha(float a) { alpha_.push_back(alpha_.back() * a); }
void Canvas::popAlpha() {
  if (alpha_.size() > 1) alpha_.pop_back();
}
void Canvas::pushClip(Rect r) {
  const Clip& c = clip_.back();
  float x = r.x + offX_.back(), y = r.y + offY_.back();
  Clip n;
  n.x0 = int16_t(std::max<float>(c.x0, x + 0.5f));
  n.y0 = int16_t(std::max<float>(c.y0, y + 0.5f));
  n.x1 = int16_t(std::min<float>(c.x1, x + r.w + 0.5f));
  n.y1 = int16_t(std::min<float>(c.y1, y + r.h + 0.5f));
  if (n.x1 < n.x0) n.x1 = n.x0;
  if (n.y1 < n.y0) n.y1 = n.y0;
  clip_.push_back(n);
}
void Canvas::popClip() {
  if (clip_.size() > 1) clip_.pop_back();
}

uint32_t Canvas::color(Color c) const {
  float a = ((c >> 24) & 0xff) / 255.f * alpha_.back();
  uint32_t r = uint32_t(((c >> 16) & 0xff) * a + 0.5f);
  uint32_t g = uint32_t(((c >> 8) & 0xff) * a + 0.5f);
  uint32_t b = uint32_t((c & 0xff) * a + 0.5f);
  uint32_t al = uint32_t(255 * a + 0.5f);
  return r | (g << 8) | (b << 16) | (al << 24);
}

Vertex* Canvas::quad(Pipe pipe, GLuint tex) {
  Layer& l = layers_[layer_];
  const Clip& clip = clip_.back();
  uint32_t index = uint32_t(l.verts.size() / 4);
  if (l.cmds.empty() || l.cmds.back().pipe != pipe || l.cmds.back().tex != tex || !(l.cmds.back().clip == clip)) {
    l.cmds.push_back(Cmd{pipe, tex, clip, index, 0});
  }
  l.cmds.back().count++;
  l.verts.resize(l.verts.size() + 4);
  return &l.verts[index * 4];
}

namespace {
// The four corners in index order: top-left, top-right, bottom-left, bottom-right.
inline void corners(Vertex* v, float x0, float y0, float x1, float y1) {
  v[0].x = x0, v[0].y = y0;
  v[1].x = x1, v[1].y = y0;
  v[2].x = x0, v[2].y = y1;
  v[3].x = x1, v[3].y = y1;
}
inline void uvs(Vertex* v, float u0, float v0, float u1, float v1) {
  v[0].u = u0, v[0].v = v0;
  v[1].u = u1, v[1].v = v0;
  v[2].u = u0, v[2].v = v1;
  v[3].u = u1, v[3].v = v1;
}
inline void local(Vertex* v, float hw, float hh, float pad, float radius) {
  float ex = hw + pad, ey = hh + pad;
  v[0].lx = -ex, v[0].ly = -ey;
  v[1].lx = ex, v[1].ly = -ey;
  v[2].lx = -ex, v[2].ly = ey;
  v[3].lx = ex, v[3].ly = ey;
  for (int i = 0; i < 4; i++) v[i].hw = hw, v[i].hh = hh, v[i].radius = radius;
}
}  // namespace

void Canvas::fill(Rect r, Color c) { fillGradient(r, c, c); }

void Canvas::fillCorners(Rect r, Color tl, Color tr, Color bl, Color br) {
  if (r.w <= 0 || r.h <= 0) return;
  Vertex* v = quad(kFill, 0);
  float x = r.x + offX_.back(), y = r.y + offY_.back();
  corners(v, x, y, x + r.w, y + r.h);
  v[0].rgba = color(tl), v[1].rgba = color(tr), v[2].rgba = color(bl), v[3].rgba = color(br);
}

void Canvas::fillGradient(Rect r, Color top, Color bottom) {
  if (r.w <= 0 || r.h <= 0) return;
  Vertex* v = quad(kFill, 0);
  float x = r.x + offX_.back(), y = r.y + offY_.back();
  corners(v, x, y, x + r.w, y + r.h);
  uint32_t t = color(top), b = color(bottom);
  v[0].rgba = v[1].rgba = t;
  v[2].rgba = v[3].rgba = b;
}

void Canvas::shape(Rect r, float radius, Color top, Color bottom) {
  if (r.w <= 0 || r.h <= 0) return;
  Vertex* v = quad(kShape, 0);
  float x = r.x + offX_.back(), y = r.y + offY_.back();
  const float pad = 1;  // room for the antialiased edge
  corners(v, x - pad, y - pad, x + r.w + pad, y + r.h + pad);
  local(v, r.w / 2, r.h / 2, pad, std::min(radius, std::min(r.w, r.h) / 2));
  // the gradient spans the padded quad: over a 1 px pad the difference is invisible
  uint32_t t = color(top), b = color(bottom);
  v[0].rgba = v[1].rgba = t;
  v[2].rgba = v[3].rgba = b;
}

void Canvas::video(unsigned externalTex, Rect dst, const float* m) {
  if (!externalTex || dst.w <= 0 || dst.h <= 0) return;
  Vertex* v = quad(kVideo, externalTex);
  float x = dst.x + offX_.back(), y = dst.y + offY_.back();
  corners(v, x, y, x + dst.w, y + dst.h);
  // the corners top-left, top-right, bottom-left, bottom-right are (0,1) (1,1) (0,0) (1,0) in the
  // texture's own space (its v runs up), then through its transform
  const float tu[4] = {0, 1, 0, 1}, tv[4] = {1, 1, 0, 0};
  for (int i = 0; i < 4; i++) {
    v[i].u = m[0] * tu[i] + m[4] * tv[i] + m[12];
    v[i].v = m[1] * tu[i] + m[5] * tv[i] + m[13];
    v[i].rgba = color(0xffffffff);
  }
}

bool Canvas::captureVideo(unsigned externalTex, const float* m, int w, int h, Texture& into) {
  if (!externalTex || w <= 0 || h <= 0) return false;
  if (into.id && (into.w != w || into.h != h)) destroyTexture(into);
  if (!into.id) into = createTexture(w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, true);
  if (!fbo_) glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, into.id, 0);
  bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (ok) {
    glViewport(0, 0, w, h);
    glScissor(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // the whole screen's coordinates fill the viewport: one quad, the frame through its transform
    Vertex v[4] = {};
    const float x[4] = {0, float(w_), 0, float(w_)}, y[4] = {0, 0, float(h_), float(h_)};
    const float tu[4] = {0, 1, 0, 1}, tv[4] = {1, 1, 0, 0};
    for (int i = 0; i < 4; i++) {
      v[i].x = x[i], v[i].y = y[i];
      v[i].u = m[0] * tu[i] + m[4] * tv[i] + m[12];
      v[i].v = m[1] * tu[i] + m[5] * tv[i] + m[13];
      v[i].rgba = 0xffffffff;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);  // from memory: four vertices
    glUseProgram(programs_[kVideo]);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTex);
    constexpr GLsizei s = sizeof(Vertex);
    const char* v0 = reinterpret_cast<const char*>(v);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, s, v0 + offsetof(Vertex, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, s, v0 + offsetof(Vertex, u));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, s, v0 + offsetof(Vertex, rgba));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, s, v0 + offsetof(Vertex, lx));
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, s, v0 + offsetof(Vertex, hw));
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEnable(GL_BLEND);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, w_, h_);
  glScissor(0, 0, w_, h_);
  return ok;
}

void Canvas::image(const Texture& t, Rect dst, Rect uv, float radius, Color tint) {
  if (!t.id || dst.w <= 0 || dst.h <= 0) return;
  if (radius <= 0) {  // square: no mask, no padding
    Vertex* v = quad(kImagePlain, t.id);
    float x = dst.x + offX_.back(), y = dst.y + offY_.back();
    corners(v, x, y, x + dst.w, y + dst.h);
    uvs(v, uv.x, uv.y, uv.x + uv.w, uv.y + uv.h);
    uint32_t c = color(tint);
    for (int i = 0; i < 4; i++) v[i].rgba = c;
    return;
  }
  Vertex* v = quad(kImage, t.id);
  float x = dst.x + offX_.back(), y = dst.y + offY_.back();
  // one pixel of padding for the antialiased edge, with the texture coordinates carried along
  const float pad = 1;
  float du = uv.w / dst.w * pad, dv = uv.h / dst.h * pad;
  corners(v, x - pad, y - pad, x + dst.w + pad, y + dst.h + pad);
  uvs(v, uv.x - du, uv.y - dv, uv.x + uv.w + du, uv.y + uv.h + dv);
  local(v, dst.w / 2, dst.h / 2, pad, std::min(radius, std::min(dst.w, dst.h) / 2));
  uint32_t c = color(tint);
  for (int i = 0; i < 4; i++) v[i].rgba = c;
}

void Canvas::colorGlyph(const Texture& atlas, Rect dst, Rect uv, uint32_t alpha) {
  Vertex* v = quad(kImagePlain, atlas.id);
  float x = dst.x + offX_.back(), y = dst.y + offY_.back();
  corners(v, x, y, x + dst.w, y + dst.h);
  uvs(v, uv.x, uv.y, uv.x + uv.w, uv.y + uv.h);
  uint32_t col = color((alpha & 0xff) << 24 | 0xffffff);
  for (int i = 0; i < 4; i++) v[i].rgba = col;
  if (fade_) {
    for (int i = 0; i < 4; i++) {
      float f = std::min((v[i].y - fadeTop_) / fadeEdge_, (fadeBottom_ - v[i].y) / fadeEdge_);
      f = f < 0 ? 0 : f > 1 ? 1 : f;
      uint32_t o = 0;
      for (int k = 0; k < 32; k += 8) o |= uint32_t(((col >> k) & 0xff) * f + 0.5f) << k;
      v[i].rgba = o;
    }
  }
}

void Canvas::glyph(const Texture& atlas, Rect dst, Rect uv, Color c) {
  Vertex* v = quad(kText, atlas.id);
  float x = dst.x + offX_.back(), y = dst.y + offY_.back();
  corners(v, x, y, x + dst.w, y + dst.h);
  uvs(v, uv.x, uv.y, uv.x + uv.w, uv.y + uv.h);
  uint32_t col = color(c);
  for (int i = 0; i < 4; i++) v[i].rgba = col;
  if (fade_) {
    for (int i = 0; i < 4; i++) {
      float f = std::min((v[i].y - fadeTop_) / fadeEdge_, (fadeBottom_ - v[i].y) / fadeEdge_);
      f = f < 0 ? 0 : f > 1 ? 1 : f;
      uint32_t o = 0;
      for (int k = 0; k < 32; k += 8) o |= uint32_t(((col >> k) & 0xff) * f + 0.5f) << k;
      v[i].rgba = o;
    }
  }
}

bool saveScreenshot(const char* path, int w, int h) {
  std::vector<uint8_t> px(size_t(w) * h * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  png_image img;
  memset(&img, 0, sizeof(img));
  img.version = PNG_IMAGE_VERSION;
  img.width = w;
  img.height = h;
  img.format = PNG_FORMAT_RGBA;
  for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
  // GL's rows run bottom-up: a negative stride tells libpng (it starts from the last row itself)
  return png_image_write_to_file(&img, path, 0, px.data(), -w * 4, nullptr) != 0;
}

}  // namespace rondo::ui
