// The renderer: everything on screen is quads in one vertex buffer per frame, drawn with four small
// GLES2 programs (flat fill, rounded shape, image, text) in about a dozen draw calls.
//
// Drawing goes into layers (0 is lowest). Inside a layer, quads keep their order and neighbors with the
// same program, texture and clip merge into one draw. A list draws its row backgrounds, thumbnails and
// text in separate layers, so all its text is one draw however many rows there are.
//
// Colors are 0xAARRGGBB and blending is premultiplied, so fading anything (text, images, whole screens)
// is a multiply.
#pragma once
#include <GLES2/gl2.h>
#include <stdint.h>

#include <vector>

namespace rondo::ui {

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  float right() const { return x + w; }
  float bottom() const { return y + h; }
};

using Color = uint32_t;  // 0xAARRGGBB

constexpr Color rgb(uint32_t v) { return 0xff000000u | v; }
inline Color withAlpha(Color c, float a) {
  uint32_t al = uint32_t(((c >> 24) & 0xff) * a + 0.5f);
  return (c & 0xffffff) | (al << 24);
}
Color mix(Color a, Color b, float t);

struct Texture {
  GLuint id = 0;
  int w = 0, h = 0;
};

// format: GL_RGB, GL_RGBA or GL_ALPHA; type GL_UNSIGNED_BYTE or GL_UNSIGNED_SHORT_5_6_5.
Texture createTexture(int w, int h, GLenum format, GLenum type, const void* pixels, bool linear = true);
void destroyTexture(Texture& t);

struct Vertex {
  float x, y;    // screen pixels
  float u, v;    // texture coordinates
  uint32_t rgba;  // premultiplied, in memory order R G B A
  float lx, ly;  // position from the shape's center, pixels
  float hw, hh;  // the shape's half size
  float radius;  // corner radius
};

class Canvas {
 public:
  bool init(int width, int height);
  int width() const { return w_; }
  int height() const { return h_; }

  void begin(Color clear);
  void end();  // uploads and draws everything recorded since begin()

  void layer(int n) { layer_ = n < kLayers ? n : kLayers - 1; }
  int currentLayer() const { return layer_; }

  // An offset, an opacity and a clip, each applying to what is drawn until the matching pop.
  void pushOffset(float dx, float dy);
  void popOffset();
  void pushAlpha(float a);
  void popAlpha();
  void pushClip(Rect r);  // in current coordinates; intersects the current clip
  void popClip();
  float alpha() const { return alpha_.back(); }

  // A flat rectangle with hard edges: backgrounds and pixel-aligned lines.
  void fill(Rect r, Color c);
  void fillGradient(Rect r, Color top, Color bottom);
  // A rectangle with antialiased edges and rounded corners (radius 0: square), top-to-bottom gradient.
  void shape(Rect r, float radius, Color top, Color bottom);
  void shape(Rect r, float radius, Color c) { shape(r, radius, c, c); }
  // An image (uv in 0..1 of the texture) with optional rounded corners, tinted (premultiplied white =
  // as is).
  void image(const Texture& t, Rect dst, Rect uv, float radius = 0, Color tint = 0xffffffff);
  // A glyph from an A8 atlas; uv in 0..1.
  void glyph(const Texture& atlas, Rect dst, Rect uv, Color c);

  // Draw statistics of the last end().
  int drawCalls() const { return draws_; }
  int quads() const { return quads_; }

 private:
  static constexpr int kLayers = 8;
  enum Pipe : uint8_t { kFill, kShape, kImage, kText, kImagePlain, kPipes };
  struct Clip {
    int16_t x0, y0, x1, y1;
    bool operator==(const Clip& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
  };
  struct Cmd {
    Pipe pipe;
    GLuint tex;
    Clip clip;
    uint32_t first, count;  // quads, in the layer's vertices
  };
  struct Layer {
    std::vector<Vertex> verts;
    std::vector<Cmd> cmds;
  };

  Vertex* quad(Pipe pipe, GLuint tex);
  uint32_t color(Color c) const;

  int w_ = 0, h_ = 0;
  int layer_ = 0;
  Layer layers_[kLayers];
  std::vector<float> offX_, offY_, alpha_;
  std::vector<Clip> clip_;
  GLuint programs_[kPipes] = {};
  GLuint vbo_[3] = {};
  int vboNext_ = 0;
  GLuint ibo_ = 0;
  std::vector<Vertex> upload_;
  int draws_ = 0, quads_ = 0;
};

// Writes what is on screen now (the frame just drawn, before present) to a PNG file.
bool saveScreenshot(const char* path, int w, int h);

}  // namespace rondo::ui
