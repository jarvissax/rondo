#include "cover.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

namespace rondo {
namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
uint32_t syncsafe(const uint8_t* p) { return uint32_t(p[0]) << 21 | uint32_t(p[1]) << 14 | uint32_t(p[2]) << 7 | p[3]; }

bool readAt(int fd, uint64_t off, void* buf, size_t n) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  while (n) {
    ssize_t r = pread64(fd, b, n, off64_t(off));
    if (r <= 0) return false;
    b += r, off += uint64_t(r), n -= size_t(r);
  }
  return true;
}

constexpr uint32_t kMaxPicture = 16 << 20;

// FLAC: "fLaC", then metadata blocks; PICTURE is type 6.
bool flac(int fd, uint64_t at, std::vector<uint8_t>& out) {
  at += 4;
  bool found = false;
  for (int blocks = 0; blocks < 64; blocks++) {
    uint8_t h[4];
    if (!readAt(fd, at, h, 4)) return found;
    uint32_t len = be24(h + 1);
    if ((h[0] & 0x7f) == 6 && len > 32 && len < kMaxPicture) {
      std::vector<uint8_t> b(len);
      if (readAt(fd, at + 4, b.data(), len)) {
        uint32_t type = be32(&b[0]);
        size_t p = 4;
        uint32_t mime = be32(&b[p]);
        p += 4 + mime;
        if (p + 4 > len) return found;
        uint32_t desc = be32(&b[p]);
        p += 4 + desc + 16;
        if (p + 4 > len) return found;
        uint32_t size = be32(&b[p]);
        p += 4;
        if (p + size <= len && (!found || type == 3)) {
          out.assign(b.begin() + long(p), b.begin() + long(p + size));
          found = true;
          if (type == 3) return true;
        }
      }
    }
    if (h[0] & 0x80) break;  // the last metadata block
    at += 4 + len;
  }
  return found;
}

// MP4: moov/udta/meta/ilst/covr/data, found by walking the atom tree.
bool mp4Find(int fd, uint64_t start, uint64_t end, const char* const* path, int depth, std::vector<uint8_t>& out) {
  uint64_t at = start;
  while (at + 8 <= end) {
    uint8_t h[16];
    if (!readAt(fd, at, h, 8)) return false;
    uint64_t size = be32(h), header = 8;
    if (size == 1) {
      if (!readAt(fd, at + 8, h + 8, 8)) return false;
      size = uint64_t(be32(h + 8)) << 32 | be32(h + 12);
      header = 16;
    } else if (size == 0) {
      size = end - at;
    }
    if (size < header || at + size > end) return false;
    if (!memcmp(h + 4, path[0], 4)) {
      uint64_t body = at + header;
      if (!strcmp(path[0], "meta")) body += 4;  // a full box: version and flags first
      if (!path[1]) {  // "data": 4 bytes of type, 4 of locale, then the picture
        uint64_t n = size - header - 8;
        if (n == 0 || n > kMaxPicture) return false;
        out.resize(size_t(n));
        return readAt(fd, body + 8, out.data(), size_t(n));
      }
      return mp4Find(fd, body, at + size, path + 1, depth + 1, out);
    }
    at += size;
  }
  return false;
}

// ID3v2.3/2.4 at the start of an MP3: the APIC frame.
bool id3(int fd, std::vector<uint8_t>& out) {
  uint8_t h[10];
  if (!readAt(fd, 0, h, 10)) return false;
  int version = h[3];
  uint32_t size = syncsafe(h + 6);
  if (size > kMaxPicture * 2 || version < 3) return false;
  std::vector<uint8_t> tag(size);
  if (!readAt(fd, 10, tag.data(), size)) return false;
  size_t p = 0;
  while (p + 10 <= size) {
    uint32_t len = version == 4 ? syncsafe(&tag[p + 4]) : be32(&tag[p + 4]);
    if (!tag[p] || p + 10 + len > size) break;
    if (!memcmp(&tag[p], "APIC", 4) && len > 4) {
      const uint8_t* f = &tag[p + 10];
      size_t q = 1;  // text encoding
      while (q < len && f[q]) q++;  // MIME type
      q += 2;  // its terminator and the picture type
      bool wide = f[0] == 1 || f[0] == 2;
      while (q + (wide ? 1 : 0) < len && (f[q] || (wide && f[q + 1]))) q += wide ? 2 : 1;  // description
      q += wide ? 2 : 1;
      if (q < len) {
        out.assign(f + q, f + len);
        return true;
      }
    }
    p += 10 + len;
  }
  return false;
}

CoverReader g_fallback = nullptr;

}  // namespace

void setFallbackCoverReader(CoverReader reader) { g_fallback = reader; }

bool readCover(const char* path, std::vector<uint8_t>& out) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  uint8_t magic[12];
  bool ok = false, known = false;  // known: a format read here (its picture, if any, is found here)
  if (readAt(fd, 0, magic, 12)) {
    known = !memcmp(magic, "fLaC", 4) || !memcmp(magic + 4, "ftyp", 4) || !memcmp(magic, "ID3", 3);
    if (!memcmp(magic, "fLaC", 4)) {
      ok = flac(fd, 0, out);
    } else if (!memcmp(magic + 4, "ftyp", 4)) {
      static const char* const kPath[] = {"moov", "udta", "meta", "ilst", "covr", "data", nullptr};
      off64_t end = lseek64(fd, 0, SEEK_END);
      ok = mp4Find(fd, 0, uint64_t(end), kPath, 0, out);
    } else if (!memcmp(magic, "ID3", 3)) {
      ok = id3(fd, out);
      if (!ok) {  // a FLAC file with an ID3 tag in front
        uint8_t m[4];
        uint64_t skip = 10 + syncsafe(magic + 6);
        if (readAt(fd, skip, m, 4) && !memcmp(m, "fLaC", 4)) ok = flac(fd, skip, out);
      }
    }
  }
  close(fd);
  if (!ok && !known && g_fallback) ok = g_fallback(path, out);
  return ok;
}

}  // namespace rondo
