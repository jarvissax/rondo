#include "fold.h"

#include <stdint.h>

namespace rondo {
namespace {

uint32_t next(const char*& p, const char* end) {
  uint8_t c = uint8_t(*p++);
  if (c < 0x80) return c;
  int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
  uint32_t cp = c & (0x3f >> extra);
  for (int i = 0; i < extra && p < end && (uint8_t(*p) & 0xc0) == 0x80; i++) cp = (cp << 6) | (uint8_t(*p++) & 0x3f);
  return cp;
}

void put(std::string& s, uint32_t cp) {
  if (cp < 0x80) s += char(cp);
  else if (cp < 0x800) s += char(0xc0 | (cp >> 6)), s += char(0x80 | (cp & 0x3f));
  else if (cp < 0x10000) s += char(0xe0 | (cp >> 12)), s += char(0x80 | ((cp >> 6) & 0x3f)), s += char(0x80 | (cp & 0x3f));
  else {
    s += char(0xf0 | (cp >> 18)), s += char(0x80 | ((cp >> 12) & 0x3f));
    s += char(0x80 | ((cp >> 6) & 0x3f)), s += char(0x80 | (cp & 0x3f));
  }
}

uint32_t lowerCp(uint32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c < 0x80) return c;
  if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 32;
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177)) return c | 1;
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return (c & 1) ? c + 1 : c;
  if (c == 0x178) return 0xff;
  if (c == 0x18f) return 0x259;  // Ə
  if (c >= 0x218 && c <= 0x21b) return c | 1;
  if (c >= 0x391 && c <= 0x3a9 && c != 0x3a2) return c + 32;
  if (c == 0x386) return 0x3ac;
  if (c >= 0x388 && c <= 0x38a) return c + 37;
  if (c == 0x38c) return 0x3cc;
  if (c == 0x38e || c == 0x38f) return c + 63;
  if (c >= 0x410 && c <= 0x42f) return c + 32;
  if (c >= 0x400 && c <= 0x40f) return c + 80;
  return c;
}

uint32_t upperCp(uint32_t c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c < 0x80) return c;
  if (c == 0xb5) return 0x39c;  // micro sign
  if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return c - 32;
  if (c == 0xff) return 0x178;
  if (c == 0x131) return 'I';  // dotless ı
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177)) return c & ~1u;
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return (c & 1) ? c : c - 1;
  if (c == 0x17f) return 'S';
  if (c == 0x259) return 0x18f;  // ə
  if (c >= 0x218 && c <= 0x21b) return c & ~1u;
  if (c >= 0x3b1 && c <= 0x3c9) return c == 0x3c2 ? 0x3a3 : c - 32;
  if (c == 0x3ac) return 0x386;
  if (c >= 0x3ad && c <= 0x3af) return c - 37;
  if (c == 0x3cc) return 0x38c;
  if (c == 0x3cd || c == 0x3ce) return c - 63;
  if (c >= 0x430 && c <= 0x44f) return c - 32;
  if (c >= 0x450 && c <= 0x45f) return c - 80;
  return c;
}

// The base letter of an accented lower-case letter (what NFD keeps), or the letter itself.
uint32_t base(uint32_t c) {
  static const char kLatin1[] = "aaaaaa\0ceeeeiiii\0nooooo\0\0uuuuy\0\0";  // 0xe0..0xff, \0 = unchanged
  if (c >= 0xe0 && c <= 0xff) {
    char b = c == 0xff ? 'y' : kLatin1[c - 0xe0];
    return b ? uint32_t(b) : c;
  }
  if (c >= 0x100 && c <= 0x17f) {
    struct R { uint16_t from, to; char b; };
    static const R kExtA[] = {{0x100, 0x105, 'a'}, {0x106, 0x10d, 'c'}, {0x10e, 0x10f, 'd'}, {0x112, 0x11b, 'e'},
                              {0x11c, 0x123, 'g'}, {0x124, 0x125, 'h'}, {0x128, 0x131, 'i'}, {0x134, 0x135, 'j'},
                              {0x136, 0x137, 'k'}, {0x139, 0x13e, 'l'}, {0x143, 0x148, 'n'}, {0x14c, 0x151, 'o'},
                              {0x154, 0x159, 'r'}, {0x15a, 0x161, 's'}, {0x162, 0x165, 't'}, {0x168, 0x173, 'u'},
                              {0x174, 0x175, 'w'}, {0x176, 0x178, 'y'}, {0x179, 0x17e, 'z'}};
    for (const R& r : kExtA)
      if (c >= r.from && c <= r.to) return uint32_t(r.b);
    return c;
  }
  if (c == 0x259) return 'e';  // ə
  if (c == 0x219) return 's';
  if (c == 0x21b) return 't';
  switch (c) {  // Greek tonos, Cyrillic breve and diaeresis
    case 0x3ac: return 0x3b1;
    case 0x3ad: return 0x3b5;
    case 0x3ae: return 0x3b7;
    case 0x3af: case 0x3ca: case 0x390: return 0x3b9;
    case 0x3cc: return 0x3bf;
    case 0x3cd: case 0x3cb: case 0x3b0: return 0x3c5;
    case 0x3ce: return 0x3c9;
    case 0x439: return 0x438;  // й
    case 0x451: case 0x450: return 0x435;  // ё ѐ
    case 0x453: return 0x433;
    case 0x45c: return 0x43a;
    case 0x457: return 0x456;
    case 0x45e: return 0x443;
    default: return c;
  }
}

}  // namespace

std::string lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  const char* p = s.data();
  const char* end = p + s.size();
  while (p < end) {
    uint32_t c = next(p, end);
    if (c == 0x130) {  // İ: i and a combining dot, as Java lower-cases it
      out += 'i';
      put(out, 0x307);
      continue;
    }
    put(out, lowerCp(c));
  }
  return out;
}

std::string upper(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  const char* p = s.data();
  const char* end = p + s.size();
  while (p < end) {
    uint32_t c = next(p, end);
    if (c == 0xdf) {  // ß
      out += "SS";
      continue;
    }
    put(out, upperCp(c));
  }
  return out;
}

uint32_t upperChar(uint32_t cp) { return upperCp(cp); }

bool isLetter(uint32_t c) {
  if (c < 0x80) return (c | 32) >= 'a' && (c | 32) <= 'z';
  if (c < 0xc0) return c == 0xaa || c == 0xb5 || c == 0xba;
  if (c == 0xd7 || c == 0xf7) return false;
  return c <= 0x24f || (c >= 0x370 && c <= 0x3ff && c != 0x37e && c != 0x387) || (c >= 0x400 && c <= 0x52f && (c < 0x482 || c > 0x489)) ||
         (c >= 0x531 && c <= 0x587) || (c >= 0x5d0 && c <= 0x5ea) || (c >= 0x620 && c <= 0x64a) || (c >= 0x904 && c <= 0x939) ||
         (c >= 0xe01 && c <= 0xe30) || (c >= 0x10a0 && c <= 0x10ff) || (c >= 0x1100 && c <= 0x11ff) ||
         (c >= 0x1e00 && c <= 0x1fff) || (c >= 0x3041 && c <= 0x30ff && c != 0x30fb) || (c >= 0x3400 && c <= 0x4dbf) ||
         (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7a3);
}

std::string fold(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  const char* p = s.data();
  const char* end = p + s.size();
  while (p < end) {
    uint32_t c = next(p, end);
    if (c >= 0x300 && c <= 0x36f) continue;  // combining marks
    if (c == 0x130 || c == 0x131) {
      out += 'i';
      continue;
    }
    put(out, base(lowerCp(c)));
  }
  return out;
}

}  // namespace rondo
