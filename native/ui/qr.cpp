// encodeQr (qr.h), ZXing's com.google.zxing.qrcode.encoder.{Encoder, MatrixUtil, MaskUtil} for byte
// mode and level L, versions 1-9.
#include "qr.h"

#include <stdlib.h>

#include <algorithm>

namespace rondo::ui {
namespace {

// level L: data codewords, EC codewords a block, blocks (one group up to version 9 except as listed)
struct VersionL {
  int total, ecPerBlock, blocks1, data1, blocks2, data2;
};
// ISO 18004 table 9, error correction level L
const VersionL kL[10] = {{0, 0, 0, 0, 0, 0},       {26, 7, 1, 19, 0, 0},    {44, 10, 1, 34, 0, 0},
                         {70, 15, 1, 55, 0, 0},     {100, 20, 1, 80, 0, 0},  {134, 26, 1, 108, 0, 0},
                         {172, 18, 2, 68, 0, 0},    {196, 20, 2, 78, 0, 0},  {242, 24, 2, 97, 0, 0},
                         {292, 30, 2, 116, 0, 0}};
// alignment pattern centers (versions 2-9)
const int kAlign[10][3] = {{0}, {0}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}};

int dataBytes(int v) { return kL[v].blocks1 * kL[v].data1 + kL[v].blocks2 * kL[v].data2; }

// GF(256) with 0x11D, generator base 0 (GenericGF.QR_CODE_FIELD_256)
struct Gf {
  uint8_t exp[512], log[256];
  Gf() {
    int x = 1;
    for (int i = 0; i < 255; i++) {
      exp[i] = uint8_t(x);
      log[x] = uint8_t(i);
      x <<= 1;
      if (x & 0x100) x ^= 0x11d;
    }
    for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
  }
  uint8_t mul(uint8_t a, uint8_t b) const { return a && b ? exp[log[a] + log[b]] : 0; }
};
const Gf& gf() {
  static Gf g;
  return g;
}

// ReedSolomonEncoder: the remainder of data * x^n by the generator (x - a^0)(x - a^1)…(x - a^(n-1))
std::vector<uint8_t> ecBytes(const std::vector<uint8_t>& data, int n) {
  const Gf& g = gf();
  std::vector<uint8_t> gen(1, 1);
  for (int i = 0; i < n; i++) {
    std::vector<uint8_t> next(gen.size() + 1, 0);
    for (size_t j = 0; j < gen.size(); j++) {
      next[j] ^= gen[j];
      next[j + 1] ^= g.mul(gen[j], g.exp[i]);
    }
    gen = next;
  }
  std::vector<uint8_t> rem(n, 0);
  for (uint8_t d : data) {
    uint8_t f = d ^ rem[0];
    rem.erase(rem.begin());
    rem.push_back(0);
    for (int j = 0; j < n; j++) rem[j] ^= g.mul(gen[j + 1], f);
  }
  return rem;
}

struct Bits {
  std::vector<bool> b;
  void add(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) b.push_back((v >> i) & 1);
  }
};

bool maskBit(int mask, int x, int y) {  // MaskUtil.getDataMaskBit
  int t;
  switch (mask) {
    case 0: return ((y + x) & 1) == 0;
    case 1: return (y & 1) == 0;
    case 2: return x % 3 == 0;
    case 3: return (y + x) % 3 == 0;
    case 4: return (((y / 2) + (x / 3)) & 1) == 0;
    case 5: t = y * x; return (t & 1) + (t % 3) == 0;
    case 6: t = y * x; return (((t & 1) + (t % 3)) & 1) == 0;
    default: t = y * x; return (((t % 3) + ((y + x) & 1)) & 1) == 0;
  }
}

// MatrixUtil.buildMatrix: -1 empty, 0 light, 1 dark
std::vector<int8_t> build(const Bits& data, int version, int mask) {
  int n = 17 + 4 * version;
  std::vector<int8_t> m(size_t(n) * n, -1);
  auto at = [&](int x, int y) -> int8_t& { return m[size_t(y) * n + x]; };
  auto finder = [&](int x0, int y0) {
    for (int y = -1; y <= 7; y++)
      for (int x = -1; x <= 7; x++) {
        int xx = x0 + x, yy = y0 + y;
        if (xx < 0 || yy < 0 || xx >= n || yy >= n) continue;
        bool ring = x >= 0 && x <= 6 && y >= 0 && y <= 6 && (x == 0 || x == 6 || y == 0 || y == 6);
        bool core = x >= 2 && x <= 4 && y >= 2 && y <= 4;
        at(xx, yy) = ring || core ? 1 : 0;  // the separator around it light
      }
  };
  finder(0, 0), finder(n - 7, 0), finder(0, n - 7);
  at(8, n - 8) = 1;  // the dark module
  if (version >= 2) {
    const int* c = kAlign[version];
    int count = c[2] ? 3 : 2;
    for (int i = 0; i < count; i++)
      for (int j = 0; j < count; j++) {
        int cx = c[i], cy = c[j];
        if (at(cx, cy) != -1) continue;  // on a finder
        for (int y = -2; y <= 2; y++)
          for (int x = -2; x <= 2; x++) at(cx + x, cy + y) = std::max(abs(x), abs(y)) != 1 ? 1 : 0;
      }
  }
  for (int i = 8; i < n - 8; i++) {  // timing
    int8_t bit = (i + 1) % 2;
    if (at(i, 6) == -1) at(i, 6) = bit;
    if (at(6, i) == -1) at(6, i) = bit;
  }
  // type info: level L (01) and the mask, BCH(15,5) with 0x537, XOR 0x5412
  int type = (1 << 3) | mask, v = type << 10;
  for (int d = 14; d >= 10; d--)
    if (v & (1 << d)) v ^= 0x537 << (d - 10);
  int info = ((type << 10) | v) ^ 0x5412;
  static const int coords[15][2] = {{8, 0}, {8, 1}, {8, 2}, {8, 3}, {8, 4}, {8, 5}, {8, 7}, {8, 8},
                                    {7, 8}, {5, 8}, {4, 8}, {3, 8}, {2, 8}, {1, 8}, {0, 8}};
  for (int i = 0; i < 15; i++) {
    int8_t bit = (info >> i) & 1;  // typeInfoBits.get(size - 1 - i): the lowest bit first
    at(coords[i][0], coords[i][1]) = bit;
    if (i < 8) at(n - i - 1, 8) = bit;
    else at(8, n - 7 + (i - 8)) = bit;
  }
  // the data, two columns at a time from the bottom right, up then down, around the timing column
  size_t bit = 0;
  int dir = -1, y = n - 1;
  for (int x = n - 1; x > 0; x -= 2) {
    if (x == 6) x -= 1;
    while (y >= 0 && y < n) {
      for (int i = 0; i < 2; i++) {
        int xx = x - i;
        if (at(xx, y) != -1) continue;
        bool b = bit < data.b.size() ? data.b[bit++] : false;
        if (maskBit(mask, xx, y)) b = !b;
        at(xx, y) = b;
      }
      y += dir;
    }
    dir = -dir;
    y += dir;
  }
  return m;
}

// MaskUtil's four rules
int penalty(const std::vector<int8_t>& m, int n) {
  auto at = [&](int x, int y) { return m[size_t(y) * n + x]; };
  int p = 0;
  for (int pass = 0; pass < 2; pass++)  // rule 1: runs of five or more
    for (int i = 0; i < n; i++) {
      int run = 0, prev = -1;
      for (int j = 0; j < n; j++) {
        int c = pass ? at(i, j) : at(j, i);
        if (c == prev) run++;
        else {
          if (run >= 5) p += 3 + (run - 5);
          run = 1, prev = c;
        }
      }
      if (run >= 5) p += 3 + (run - 5);
    }
  for (int y = 0; y < n - 1; y++)  // rule 2: 2 x 2 blocks
    for (int x = 0; x < n - 1; x++) {
      int c = at(x, y);
      if (c == at(x + 1, y) && c == at(x, y + 1) && c == at(x + 1, y + 1)) p += 3;
    }
  // rule 3: 1:1:3:1:1 with four light modules on one side (off the edge counts as light, as ZXing's
  // isWhite clamps its range)
  int found = 0;
  auto lightH = [&](int y, int from, int to) {
    from = std::max(from, 0), to = std::min(to, n);
    for (int i = from; i < to; i++)
      if (at(i, y) == 1) return false;
    return true;
  };
  auto lightV = [&](int x, int from, int to) {
    from = std::max(from, 0), to = std::min(to, n);
    for (int i = from; i < to; i++)
      if (at(x, i) == 1) return false;
    return true;
  };
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      if (x + 6 < n && at(x, y) == 1 && at(x + 1, y) == 0 && at(x + 2, y) == 1 && at(x + 3, y) == 1 && at(x + 4, y) == 1 &&
          at(x + 5, y) == 0 && at(x + 6, y) == 1 && (lightH(y, x - 4, x) || lightH(y, x + 7, x + 11)))
        found++;
      if (y + 6 < n && at(x, y) == 1 && at(x, y + 1) == 0 && at(x, y + 2) == 1 && at(x, y + 3) == 1 && at(x, y + 4) == 1 &&
          at(x, y + 5) == 0 && at(x, y + 6) == 1 && (lightV(x, y - 4, y) || lightV(x, y + 7, y + 11)))
        found++;
    }
  p += found * 40;
  int darkCount = 0;  // rule 4: how far from half dark, in 5 % steps
  for (int8_t c : m) darkCount += c == 1;
  int total = n * n;
  p += abs(darkCount * 2 - total) * 10 / total * 10;
  return p;
}

}  // namespace

bool encodeQr(const std::string& text, QrCode& out) {
  int version = 0;
  for (int v = 1; v <= 6; v++)  // 7 and up would need the version blocks
    if (4 + 8 + 8 * int(text.size()) <= dataBytes(v) * 8) {
      version = v;
      break;
    }
  if (!version) return false;
  int capacity = dataBytes(version);
  Bits head;
  head.add(0x4, 4);  // byte mode
  head.add(uint32_t(text.size()), 8);
  for (unsigned char c : text) head.add(c, 8);
  // terminateBits: up to four zeros, to a byte, then 0xEC 0x11 …
  for (int i = 0; i < 4 && int(head.b.size()) < capacity * 8; i++) head.b.push_back(false);
  while (head.b.size() % 8) head.b.push_back(false);
  for (int i = 0; int(head.b.size()) < capacity * 8; i++) head.add(i % 2 ? 0x11 : 0xec, 8);
  std::vector<uint8_t> bytes(capacity);
  for (int i = 0; i < capacity; i++) {
    uint8_t v = 0;
    for (int b = 0; b < 8; b++) v = uint8_t(v << 1 | head.b[size_t(i) * 8 + b]);
    bytes[i] = v;
  }
  // interleaveWithECBytes: the blocks' data bytes in turn, then their EC bytes in turn
  const VersionL& L = kL[version];
  std::vector<std::vector<uint8_t>> data, ec;
  int at = 0;
  for (int g = 0; g < 2; g++)
    for (int b = 0; b < (g ? L.blocks2 : L.blocks1); b++) {
      int len = g ? L.data2 : L.data1;
      data.emplace_back(bytes.begin() + at, bytes.begin() + at + len);
      at += len;
      ec.push_back(ecBytes(data.back(), L.ecPerBlock));
    }
  Bits all;
  size_t longest = 0;
  for (auto& d : data) longest = std::max(longest, d.size());
  for (size_t i = 0; i < longest; i++)
    for (auto& d : data)
      if (i < d.size()) all.add(d[i], 8);
  for (int i = 0; i < L.ecPerBlock; i++)
    for (auto& e : ec) all.add(e[i], 8);
  int n = 17 + 4 * version, best = -1, bestPenalty = 0;
  std::vector<int8_t> chosen;
  for (int mask = 0; mask < 8; mask++) {
    std::vector<int8_t> m = build(all, version, mask);
    int p = penalty(m, n);
    if (best < 0 || p < bestPenalty) best = mask, bestPenalty = p, chosen = std::move(m);
  }
  out.modules = n;
  out.dark.assign(chosen.begin(), chosen.end());
  return true;
}

std::vector<uint8_t> renderQr(const QrCode& qr, int size) {
  std::vector<uint8_t> px(size_t(size) * size, 255);
  int n = qr.modules;
  if (!n || size < n) return px;
  int multiple = size / n, pad = (size - n * multiple) / 2;
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      if (!qr.dark[size_t(y) * n + x]) continue;
      for (int dy = 0; dy < multiple; dy++)
        for (int dx = 0; dx < multiple; dx++) px[size_t(pad + y * multiple + dy) * size + pad + x * multiple + dx] = 0;
    }
  return px;
}

}  // namespace rondo::ui
