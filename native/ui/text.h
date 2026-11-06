// Text: HarfBuzz shapes, FreeType rasterizes each glyph once at its exact size into an A8 atlas, and a
// shaped line is cached, so a list row costs only its quads after its first frame.
//
// Each style has a primary face (Roboto) and the Y1's own /system/fonts as fallbacks, picked per
// character: DroidSansFallback for CJK, DroidNaskh for Arabic, and the rest. Lines are laid out
// left to right by runs; full bidirectional reordering comes later (ICU's ubidi is on the Y1).
#pragma once
#include <stdint.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gfx.h"

typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;
typedef struct FT_SizeRec_* FT_Size;
typedef struct hb_face_t hb_face_t;
typedef struct hb_font_t hb_font_t;
typedef struct hb_buffer_t hb_buffer_t;

namespace rondo::ui {

enum class Weight : uint8_t { Regular, Bold };

struct TextRun {
  float width = 0;
  struct Glyph {
    int16_t x, y;   // the glyph's pen position from the run's origin (baseline), whole pixels
    uint32_t slot;  // into the glyph table
  };
  std::vector<Glyph> glyphs;
};

class Text {
 public:
  // fontDir holds the Y1's system fonts; regular is the bundled Roboto Regular (null: the system one).
  bool init(const char* fontDir, const char* regular);
  // A style at a pixel size. The same arguments always return the same id.
  int font(Weight weight, float px);
  float ascent(int font) const { return sizes_[font].ascent; }
  float descent(int font) const { return sizes_[font].descent; }
  // The line, shaped and cut with an ellipsis to fit maxWidth (0: no limit). Cached; the reference
  // stays valid until the next call that misses the cache.
  const TextRun& run(int font, std::string_view utf8, float maxWidth = 0);
  float measure(int font, std::string_view utf8) { return run(font, utf8).width; }
  // Draws a run with its origin (left end of the baseline) at x, baseline. New glyphs are rasterized
  // here and reach the GPU in flush().
  void draw(Canvas& c, const TextRun& run, float x, float baseline, Color color);
  // Sends this frame's new glyphs to the atlas texture; call before Canvas::end().
  void flush();

  // Rasterizes the glyphs of `chars` for each font in `fonts`, for at most budgetNs; true when all are
  // done. Run while idle, so a screen's first frame does not pay for its glyphs.
  bool warm(const std::vector<int>& fonts, std::string_view chars, int64_t budgetNs);
  int glyphsRasterizedLastFrame() const { return rasterizedFrame_; }
  void frameDone() { rasterizedFrame_ = 0; }

 private:
  struct Face {
    std::string path;
    const uint8_t* data = nullptr;
    size_t size = 0;
    hb_face_t* hb = nullptr;
    FT_Face ft = nullptr;
  };
  struct Size {
    int face;  // primary face for this style
    Weight weight;
    float px;
    float ascent, descent;
    // per face: a HarfBuzz font and a FreeType size at px, created on first use
    std::vector<hb_font_t*> hb;
    std::vector<FT_Size> ft;
  };
  struct Slot {
    uint16_t size, face;
    uint32_t glyph;
    bool ready = false;
    int16_t left = 0, top = 0;  // bitmap offset from the pen position
    uint16_t w = 0, h = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  };
  struct CachedRun {
    std::string text;
    int font;
    float maxWidth;
    uint32_t used;
    TextRun run;
  };

  int openFace(const std::string& path);
  hb_font_t* hbFont(int size, int face);
  FT_Size ftSize(int size, int face);
  uint32_t slotFor(int size, int face, uint32_t glyph);
  bool rasterize(Slot& s);
  void shapeInto(TextRun& out, int font, std::string_view utf8);
  void resetAtlas();

  FT_Library ft_ = nullptr;
  std::vector<Face> faces_;
  int regular_ = -1, bold_ = -1;
  std::vector<int> fallbacks_;
  std::vector<Size> sizes_;
  std::vector<Slot> slots_;
  std::unordered_map<uint64_t, uint32_t> slotIndex_;
  std::unordered_map<uint64_t, CachedRun> runs_;
  uint32_t useClock_ = 0;
  hb_buffer_t* buffer_ = nullptr;

  static constexpr int kAtlas = 1024;
  Texture atlas_;
  std::vector<uint8_t> pixels_;  // the atlas, kept on the CPU: new glyphs go up as one update
  int shelfX_ = 0, shelfY_ = 0, shelfH_ = 0;
  int dirtyY0_ = kAtlas, dirtyY1_ = 0;
  int rasterizedFrame_ = 0;
  size_t warmFont_ = 0, warmChar_ = 0;
};

}  // namespace rondo::ui
