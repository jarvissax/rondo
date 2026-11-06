#include "text.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SIZES_H
#include FT_OUTLINE_H
#include <hb.h>

namespace rondo::ui {
namespace {

uint32_t decodeUtf8(const char*& p, const char* end) {
  uint8_t c = uint8_t(*p++);
  if (c < 0x80) return c;
  int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
  uint32_t cp = c & (0x3f >> extra);
  for (int i = 0; i < extra && p < end && (uint8_t(*p) & 0xc0) == 0x80; i++) cp = (cp << 6) | (uint8_t(*p++) & 0x3f);
  return cp;
}

// Characters that belong to whatever script surrounds them: they stay in the current run's font.
bool isCommon(uint32_t cp) {
  if (cp < 0x80) return !((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'));
  return (cp >= 0x300 && cp <= 0x36f) || (cp >= 0x2000 && cp <= 0x206f) || cp == 0xa0;
}

bool isRtl(uint32_t cp) {
  return (cp >= 0x590 && cp <= 0x8ff) || (cp >= 0xfb1d && cp <= 0xfdff) || (cp >= 0xfe70 && cp <= 0xfeff);
}

bool isStrong(uint32_t cp) { return !isCommon(cp) && cp > 0x40; }

uint64_t fnv(std::string_view s, uint64_t seed) {
  uint64_t h = 1469598103934665603ULL ^ seed;
  for (char c : s) h = (h ^ uint8_t(c)) * 1099511628211ULL;
  return h;
}

// The fallback order: what Android 4.2's own fallback list puts first, for the scripts it covers.
const char* kFallbacks[] = {
    "DroidSans.ttf",          "DroidNaskh-Regular.ttf",      "DroidSansHebrew-Regular.ttf",
    "DroidSansThai.ttf",      "DroidSansArmenian.ttf",       "DroidSansGeorgian.ttf",
    "DroidSansEthiopic-Regular.ttf", "DroidSansDevanagari-Regular.ttf", "DroidSansTamil-Regular.ttf",
    "Lohit-Bengali.ttf",      "Lohit-Kannada.ttf",           "Lohit-Telugu.ttf",
    "AnjaliNewLipi-light.ttf", "AndroidEmoji.ttf",           "MTLmr3m.ttf",
    "NanumGothic.ttf",        "DroidSansFallback.ttf",
};

}  // namespace

int Text::openFace(const std::string& path) {
  for (size_t i = 0; i < faces_.size(); i++)
    if (faces_[i].path == path) return int(i);
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  fstat(fd, &st);
  void* data = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (data == MAP_FAILED) return -1;
  Face f;
  f.path = path;
  f.data = static_cast<const uint8_t*>(data);
  f.size = size_t(st.st_size);
  if (FT_New_Memory_Face(ft_, f.data, FT_Long(f.size), 0, &f.ft) != 0) {
    munmap(data, f.size);
    return -1;
  }
  hb_blob_t* blob = hb_blob_create((const char*)f.data, unsigned(f.size), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  f.hb = hb_face_create(blob, 0);
  hb_blob_destroy(blob);
  faces_.push_back(f);
  return int(faces_.size() - 1);
}

bool Text::init(const char* fontDir, const char* regular) {
  if (FT_Init_FreeType(&ft_) != 0) return false;
  std::string dir = fontDir;
  regular_ = regular ? openFace(regular) : -1;
  if (regular_ < 0) regular_ = openFace(dir + "/Roboto-Regular.ttf");
  bold_ = openFace(dir + "/Roboto-Bold.ttf");
  if (regular_ < 0) {
    fprintf(stderr, "text: no Roboto Regular in %s\n", fontDir);
    return false;
  }
  if (bold_ < 0) bold_ = regular_;
  for (const char* name : kFallbacks) {
    int f = openFace(dir + "/" + name);
    if (f >= 0) fallbacks_.push_back(f);
  }
  buffer_ = hb_buffer_create();
  pixels_.assign(size_t(kAtlas) * kAtlas, 0);
  atlas_ = createTexture(kAtlas, kAtlas, GL_ALPHA, GL_UNSIGNED_BYTE, pixels_.data(), true);
  return true;
}

int Text::font(Weight weight, float px) {
  for (size_t i = 0; i < sizes_.size(); i++)
    if (sizes_[i].weight == weight && sizes_[i].px == px) return int(i);
  Size s;
  s.weight = weight;
  s.px = px;
  s.face = regular_;
  s.bold = weight == Weight::Bold;
  s.hb.assign(faces_.size(), nullptr);
  s.ft.assign(faces_.size(), nullptr);
  sizes_.push_back(s);
  int id = int(sizes_.size() - 1);
  hb_font_extents_t ext;
  hb_font_get_h_extents(hbFont(id, s.face), &ext);
  sizes_[id].ascent = ext.ascender / 64.0f;
  sizes_[id].descent = -ext.descender / 64.0f;
  {  // Skia: face->ascender/descender scaled, then SkScalarRound in getFontMetricsInt()
    FT_Face f = faces_[s.face].ft;
    float k = px / float(f->units_per_EM);
    sizes_[id].ascentInt = int(floorf(f->ascender * k + 0.5f));
    sizes_[id].descentInt = int(floorf(-f->descender * k + 0.5f));
  }
  return id;
}

hb_font_t* Text::hbFont(int size, int face) {
  Size& s = sizes_[size];
  if (!s.hb[face]) {
    s.hb[face] = hb_font_create(faces_[face].hb);
    int scale = int(lroundf(s.px * 64));
    hb_font_set_scale(s.hb[face], scale, scale);
  }
  return s.hb[face];
}

FT_Size Text::ftSize(int size, int face) {
  Size& s = sizes_[size];
  if (!s.ft[face]) {
    FT_Face f = faces_[face].ft;
    FT_New_Size(f, &s.ft[face]);
    FT_Activate_Size(s.ft[face]);
    FT_Set_Char_Size(f, 0, FT_F26Dot6(lroundf(s.px * 64)), 72, 72);
  }
  return s.ft[face];
}

uint32_t Text::slotFor(int size, int face, uint32_t glyph) {
  uint64_t key = (uint64_t(size) << 48) | (uint64_t(face) << 32) | glyph;
  auto it = slotIndex_.find(key);
  if (it != slotIndex_.end()) return it->second;
  Slot s;
  s.size = uint16_t(size);
  s.face = uint16_t(face);
  s.glyph = glyph;
  slots_.push_back(s);
  uint32_t index = uint32_t(slots_.size() - 1);
  slotIndex_.emplace(key, index);
  return index;
}

void Text::resetAtlas() {
  for (Slot& s : slots_) s.ready = false;
  std::fill(pixels_.begin(), pixels_.end(), 0);
  shelfX_ = shelfY_ = shelfH_ = 0;
  dirtyY0_ = 0;
  dirtyY1_ = kAtlas;
}

bool Text::rasterize(Slot& s) {
  FT_Face f = faces_[s.face].ft;
  FT_Activate_Size(ftSize(s.size, s.face));
  // light hinting: crisp horizontal stems, advances untouched (they come from HarfBuzz anyway)
  const Size& sz = sizes_[s.size];
  bool ok = FT_Load_Glyph(f, s.glyph, FT_LOAD_TARGET_LIGHT | (sz.bold ? FT_LOAD_NO_BITMAP : FT_LOAD_RENDER)) == 0;
  if (ok && sz.bold) {
    if (f->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
      static float factor = getenv("RONDO_BOLD") ? float(atof(getenv("RONDO_BOLD"))) : 0.07f;  // matched to 1.0.x on the Y1 (N1 log)
      FT_Pos w = FT_Pos(lroundf(sz.px * factor * 64));  // grows by w/2 on each side, as a stroke of w does
      FT_Outline_EmboldenXY(&f->glyph->outline, w, w);
    }
    ok = FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LIGHT) == 0;
  }
  if (!ok) {
    s.ready = true;
    s.w = s.h = 0;
    return false;
  }
  const FT_Bitmap& bm = f->glyph->bitmap;
  s.left = int16_t(f->glyph->bitmap_left);
  s.top = int16_t(f->glyph->bitmap_top);
  s.w = uint16_t(bm.width);
  s.h = uint16_t(bm.rows);
  s.ready = true;
  rasterizedFrame_++;
  if (!s.w || !s.h || bm.pixel_mode != FT_PIXEL_MODE_GRAY) {
    s.w = s.h = 0;
    return true;
  }
  // shelf packing, one pixel of empty border around each glyph for the linear filter
  int w = s.w + 2, h = s.h + 2;
  if (shelfX_ + w > kAtlas) {
    shelfY_ += shelfH_;
    shelfX_ = shelfH_ = 0;
  }
  if (shelfY_ + h > kAtlas) {
    resetAtlas();
    return rasterize(s);
  }
  int x = shelfX_ + 1, y = shelfY_ + 1;
  for (unsigned r = 0; r < bm.rows; r++) memcpy(&pixels_[size_t(y + r) * kAtlas + x], bm.buffer + r * bm.pitch, bm.width);
  shelfX_ += w;
  shelfH_ = std::max(shelfH_, h);
  dirtyY0_ = std::min(dirtyY0_, y);
  dirtyY1_ = std::max(dirtyY1_, y + int(s.h));
  s.u0 = float(x) / kAtlas;
  s.v0 = float(y) / kAtlas;
  s.u1 = float(x + s.w) / kAtlas;
  s.v1 = float(y + s.h) / kAtlas;
  return true;
}

void Text::flush() {
  if (dirtyY1_ <= dirtyY0_) return;
  glBindTexture(GL_TEXTURE_2D, atlas_.id);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirtyY0_, kAtlas, dirtyY1_ - dirtyY0_, GL_ALPHA, GL_UNSIGNED_BYTE,
                  &pixels_[size_t(dirtyY0_) * kAtlas]);
  dirtyY0_ = kAtlas;
  dirtyY1_ = 0;
}

void Text::shapeInto(TextRun& out, int font, std::string_view s) {
  out.glyphs.clear();
  out.width = 0;
  Size& sz = sizes_[font];
  struct Item {
    int face;
    unsigned start, len;
  };
  Item items[64];
  int count = 0;
  bool rtl = false, haveDirection = false;
  const char* p = s.data();
  const char* end = p + s.size();
  int cur = -1;
  unsigned curStart = 0;
  while (p < end) {
    unsigned at = unsigned(p - s.data());
    uint32_t cp = decodeUtf8(p, end);
    if (!haveDirection && isStrong(cp)) {
      rtl = isRtl(cp);
      haveDirection = true;
    }
    int face = -1;
    if (cur >= 0 && isCommon(cp) && FT_Get_Char_Index(faces_[cur].ft, cp)) face = cur;
    if (face < 0 && FT_Get_Char_Index(faces_[sz.face].ft, cp)) face = sz.face;
    for (size_t i = 0; face < 0 && i < fallbacks_.size(); i++)
      if (FT_Get_Char_Index(faces_[fallbacks_[i]].ft, cp)) face = fallbacks_[i];
    if (face < 0) face = sz.face;
    if (face != cur) {
      if (cur >= 0 && count < 64) items[count++] = {cur, curStart, at - curStart};
      cur = face;
      curStart = at;
    }
  }
  if (cur >= 0 && count < 64) items[count++] = {cur, curStart, unsigned(s.size()) - curStart};

  // a right-to-left line shows its runs from the right: lay them out in reverse
  int32_t pen = 0;  // 26.6
  for (int k = 0; k < count; k++) {
    const Item& it = items[rtl ? count - 1 - k : k];
    hb_buffer_clear_contents(buffer_);
    hb_buffer_add_utf8(buffer_, s.data(), int(s.size()), it.start, int(it.len));
    hb_buffer_guess_segment_properties(buffer_);
    // Android 4.2 forms no ligatures (fi, ff measure as their two letters there)
    static const hb_feature_t kNoLigatures[] = {{HB_TAG('l', 'i', 'g', 'a'), 0, 0, ~0u}, {HB_TAG('c', 'l', 'i', 'g'), 0, 0, ~0u}};
    hb_shape(hbFont(font, it.face), buffer_, kNoLigatures, 2);
    const float* table = it.face == regular_ ? advancesFor(sz.px) : nullptr;
    unsigned n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer_, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer_, &n);
    for (unsigned i = 0; i < n; i++) {
      TextRun::Glyph g;
      g.x = int16_t((pen + pos[i].x_offset + 32) >> 6);
      g.y = int16_t(-((pos[i].y_offset + 32) >> 6));
      g.slot = slotFor(font, it.face, info[i].codepoint);
      out.glyphs.push_back(g);
      // Whole pixels, as Android 4.2 lays text out: each character's own hinted advance (measured on the
      // Y1, tools/parity/advances) plus the pair kerning rounded to the nearest pixel.
      hb_position_t plain = hb_font_get_glyph_h_advance(hbFont(font, it.face), info[i].codepoint);
      hb_position_t kern = pos[i].x_advance - plain;
      int32_t base = ((plain + 32) >> 6) << 6;
      if (table) {
        const char* cp = s.data() + info[i].cluster;
        float a = lookup(table, decodeUtf8(cp, end));
        if (a >= 0) base = int32_t(lroundf(a)) << 6;
      }
      int32_t k = kern >= 0 ? ((kern + 32) >> 6) << 6 : -(((-kern + 32) >> 6) << 6);
      pen += base + k;
    }
  }
  out.width = pen / 64.0f;
}

const TextRun& Text::run(int font, std::string_view utf8, float maxWidth) {
  uint64_t key = fnv(utf8, uint64_t(font) * 0x9e3779b97f4a7c15ULL ^ uint64_t(lroundf(maxWidth * 4)));
  auto it = runs_.find(key);
  useClock_++;
  if (it != runs_.end() && it->second.font == font && it->second.maxWidth == maxWidth && it->second.text == utf8) {
    it->second.used = useClock_;
    return it->second.run;
  }
  if (runs_.size() > 4096) {  // drop the older half
    std::vector<uint32_t> ages;
    ages.reserve(runs_.size());
    for (auto& e : runs_) ages.push_back(e.second.used);
    std::nth_element(ages.begin(), ages.begin() + ages.size() / 2, ages.end());
    uint32_t cut = ages[ages.size() / 2];
    for (auto e = runs_.begin(); e != runs_.end();) e = e->second.used < cut ? runs_.erase(e) : std::next(e);
  }
  CachedRun& c = runs_[key];
  c.text.assign(utf8.data(), utf8.size());
  c.font = font;
  c.maxWidth = maxWidth;
  c.used = useClock_;
  shapeInto(c.run, font, utf8);
  if (maxWidth > 0 && c.run.width > maxWidth && !c.run.glyphs.empty()) {
    TextRun dots;
    shapeInto(dots, font, "\xe2\x80\xa6");
    float room = maxWidth - dots.width;
    bool rtl = c.run.glyphs.size() > 1 && c.run.glyphs.front().x > c.run.glyphs.back().x;
    auto& g = c.run.glyphs;
    if (!rtl) {
      // keep each glyph whose advance ends inside the room (the next glyph's pen position)
      size_t keep = 0;
      while (keep < g.size() && (keep + 1 < g.size() ? float(g[keep + 1].x) : c.run.width) <= room) keep++;
      int16_t at = keep < g.size() ? g[keep].x : int16_t(c.run.width);
      g.resize(keep);
      for (auto d : dots.glyphs) g.push_back({int16_t(at + d.x), d.y, d.slot});
      c.run.width = at + dots.width;
    } else {
      // visual order runs right to left: keep the right part, ellipsis at the left
      float cut = c.run.width - room;
      std::vector<TextRun::Glyph> kept;
      for (auto& x : g)
        if (x.x >= cut) kept.push_back({int16_t(x.x - cut + dots.width), x.y, x.slot});
      g.clear();
      for (auto d : dots.glyphs) g.push_back(d);
      g.insert(g.end(), kept.begin(), kept.end());
      c.run.width = maxWidth;
    }
  }
  return c.run;
}

void Text::draw(Canvas& c, const TextRun& run, float x, float baseline, Color color) {
  for (const TextRun::Glyph& g : run.glyphs) {
    Slot& s = slots_[g.slot];
    if (!s.ready) rasterize(s);
    if (!s.w) continue;
    float gx = x + g.x + s.left, gy = baseline + g.y - s.top;
    c.glyph(atlas_, {gx, gy, float(s.w), float(s.h)}, {s.u0, s.v0, s.u1 - s.u0, s.v1 - s.v0}, color);
  }
}

}  // namespace rondo::ui

namespace rondo::ui {

bool Text::warm(const std::vector<int>& fonts, std::string_view chars, int64_t budgetNs) {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  int64_t until = int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec + budgetNs;
  int done = rasterizedFrame_;
  while (warmFont_ < fonts.size()) {
    int font = fonts[warmFont_];
    const char* p = chars.data() + warmChar_;
    const char* end = chars.data() + chars.size();
    while (p < end) {
      uint32_t cp = decodeUtf8(p, end);
      FT_UInt g = FT_Get_Char_Index(faces_[sizes_[font].face].ft, cp);
      if (g) {
        Slot& s = slots_[slotFor(font, sizes_[font].face, g)];
        if (!s.ready) rasterize(s);
      }
      warmChar_ = size_t(p - chars.data());
      clock_gettime(CLOCK_MONOTONIC, &t);
      if (int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec > until) {
        rasterizedFrame_ = done;  // idle work: not a frame's cost
        return false;
      }
    }
    run(font, chars);  // HarfBuzz builds its shaping plans on first use
    warmFont_++;
    warmChar_ = 0;
  }
  rasterizedFrame_ = done;
  return true;
}

}  // namespace rondo::ui

namespace rondo::ui {

namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
float beFloat(const uint8_t* p) {
  uint32_t u = be32(p);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
}  // namespace

bool Text::loadAdvances(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  std::vector<uint8_t> b;
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) b.insert(b.end(), chunk, chunk + n);
  fclose(f);
  if (b.size() < 16 || memcmp(b.data(), "RADV", 4) != 0) return false;
  uint32_t sizes = be32(&b[8]), ranges = be32(&b[12]);
  size_t at = 16;
  Advances a;
  for (uint32_t i = 0; i < ranges && at + 8 <= b.size(); i++, at += 8) {
    a.ranges.push_back({be32(&b[at]), be32(&b[at + 4])});
    a.per += be32(&b[at + 4]) - be32(&b[at]) + 1;
  }
  for (uint32_t i = 0; i < sizes && at + 4 <= b.size(); i++, at += 4) a.sizes.push_back(beFloat(&b[at]));
  if (at + a.sizes.size() * a.per * 4 > b.size()) return false;
  a.adv.resize(a.sizes.size() * a.per);
  for (size_t i = 0; i < a.adv.size(); i++, at += 4) a.adv[i] = beFloat(&b[at]);
  advances_ = std::move(a);
  runs_.clear();  // lines shaped before were laid out without it
  return true;
}

const float* Text::advancesFor(float px) const {
  for (size_t i = 0; i < advances_.sizes.size(); i++)
    if (fabsf(advances_.sizes[i] - px) < 0.01f) return &advances_.adv[i * advances_.per];
  return nullptr;
}

float Text::lookup(const float* row, uint32_t cp) const {
  size_t base = 0;
  for (const auto& r : advances_.ranges) {
    if (cp >= r.first && cp <= r.second) return row[base + (cp - r.first)];
    base += r.second - r.first + 1;
  }
  return -1;
}

}  // namespace rondo::ui
