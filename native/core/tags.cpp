#include "tags.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <vector>

namespace rondo {
namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p + 4)) << 32 | le32(p); }
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

std::string trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

int trackNumber(const std::string& s) { return atoi(s.c_str()); }  // "3/12" -> 3

// One Vorbis comment list (FLAC, Ogg Vorbis, Opus): little-endian lengths, "KEY=value".
void vorbisComments(const uint8_t* p, size_t n, Tags& t) {
  if (n < 8) return;
  size_t at = 4 + le32(p);
  if (at + 4 > n) return;
  uint32_t count = le32(p + at);
  at += 4;
  for (uint32_t i = 0; i < count && at + 4 <= n; i++) {
    uint32_t len = le32(p + at);
    at += 4;
    if (at + len > n) break;
    std::string c((const char*)p + at, len);
    at += len;
    size_t eq = c.find('=');
    if (eq == std::string::npos) continue;
    std::string key = c.substr(0, eq), v = trim(c.substr(eq + 1));
    if (v.empty()) continue;
    // a repeated field: the last one wins, as in Android's reader (today's Rondo shows that one)
    if (!strcasecmp(key.c_str(), "TITLE")) t.title = v;
    else if (!strcasecmp(key.c_str(), "ARTIST")) t.artist = v;
    else if (!strcasecmp(key.c_str(), "ALBUMARTIST")) t.albumArtist = v;
    else if (!strcasecmp(key.c_str(), "ALBUM")) t.album = v;
    else if (!strcasecmp(key.c_str(), "TRACKNUMBER")) t.track = trackNumber(v);
  }
}

bool flac(int fd, uint64_t at, Tags& t) {
  at += 4;
  for (int blocks = 0; blocks < 128; blocks++) {
    uint8_t h[4];
    if (!readAt(fd, at, h, 4)) break;
    uint32_t len = uint32_t(h[1]) << 16 | uint32_t(h[2]) << 8 | h[3];
    int type = h[0] & 0x7f;
    if (type == 0 && len >= 18) {  // STREAMINFO
      uint8_t s[18];
      if (readAt(fd, at + 4, s, 18)) {
        uint32_t rate = uint32_t(s[10]) << 12 | uint32_t(s[11]) << 4 | s[12] >> 4;
        uint64_t total = uint64_t(s[13] & 0x0f) << 32 | uint32_t(s[14]) << 24 | uint32_t(s[15]) << 16 |
                         uint32_t(s[16]) << 8 | s[17];
        if (rate && total) t.durationSec = int(total / rate);
      }
    } else if (type == 4 && len < (4u << 20)) {  // VORBIS_COMMENT
      std::vector<uint8_t> b(len);
      if (readAt(fd, at + 4, b.data(), len)) vorbisComments(b.data(), len, t);
    }
    if (h[0] & 0x80) break;
    at += 4 + len;
  }
  return true;
}

// MP4: moov/mvhd for the duration, moov/udta/meta/ilst for the tags.
void mp4Walk(int fd, uint64_t start, uint64_t end, int depth, Tags& t) {
  uint64_t at = start;
  while (at + 8 <= end && depth < 8) {
    uint8_t h[16];
    if (!readAt(fd, at, h, 8)) return;
    uint64_t size = be32(h), header = 8;
    if (size == 1) {
      if (!readAt(fd, at + 8, h + 8, 8)) return;
      size = be64(h + 8), header = 16;
    } else if (size == 0) {
      size = end - at;
    }
    if (size < header || at + size > end) return;
    const char* type = (const char*)h + 4;
    uint64_t body = at + header, len = size - header;
    if (!memcmp(type, "moov", 4) || !memcmp(type, "udta", 4) || !memcmp(type, "ilst", 4)) {
      mp4Walk(fd, body, at + size, depth + 1, t);
    } else if (!memcmp(type, "meta", 4)) {
      mp4Walk(fd, body + 4, at + size, depth + 1, t);  // a full box
    } else if (!memcmp(type, "mvhd", 4) && len >= 32) {
      uint8_t m[32];
      if (readAt(fd, body, m, 32)) {
        uint32_t scale = m[0] == 1 ? be32(m + 20) : be32(m + 12);
        uint64_t dur = m[0] == 1 ? be64(m + 24) : be32(m + 16);
        if (scale) t.durationSec = int(dur / scale);
      }
    } else if (depth >= 3 && len > 16 && len < 65536) {  // an ilst item: its "data" child holds the value
      std::vector<uint8_t> b(static_cast<size_t>(len));
      if (readAt(fd, body, b.data(), size_t(len)) && !memcmp(&b[4], "data", 4)) {
        uint32_t dlen = be32(&b[0]);
        if (dlen >= 16 && dlen <= len) {
          std::string v = trim(std::string((const char*)&b[16], dlen - 16));
          if (!memcmp(type, "\xa9nam", 4)) t.title = v;
          else if (!memcmp(type, "\xa9" "ART", 4)) t.artist = v;
          else if (!memcmp(type, "aART", 4)) t.albumArtist = v;
          else if (!memcmp(type, "\xa9" "alb", 4)) t.album = v;
          else if (!memcmp(type, "trkn", 4) && dlen >= 20) t.track = b[16 + 2] << 8 | b[16 + 3];
        }
      }
    }
    at += size;
  }
}

void appendUtf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) s += char(cp);
  else if (cp < 0x800) s += char(0xc0 | (cp >> 6)), s += char(0x80 | (cp & 0x3f));
  else if (cp < 0x10000) s += char(0xe0 | (cp >> 12)), s += char(0x80 | ((cp >> 6) & 0x3f)), s += char(0x80 | (cp & 0x3f));
  else {
    s += char(0xf0 | (cp >> 18)), s += char(0x80 | ((cp >> 12) & 0x3f));
    s += char(0x80 | ((cp >> 6) & 0x3f)), s += char(0x80 | (cp & 0x3f));
  }
}

// An ID3v2 text frame's value in UTF-8 (encodings 0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8).
std::string id3Text(const uint8_t* f, size_t n) {
  if (n < 1) return "";
  std::string out;
  int enc = f[0];
  if (enc == 0) {
    for (size_t i = 1; i < n && f[i]; i++) appendUtf8(out, f[i]);
  } else if (enc == 3) {
    size_t e = 1;
    while (e < n && f[e]) e++;
    out.assign((const char*)f + 1, e - 1);
  } else {
    size_t i = 1;
    bool le = false;
    if (enc == 1 && n >= 3) {
      le = f[1] == 0xff && f[2] == 0xfe;
      i = 3;
    }
    for (; i + 1 < n; i += 2) {
      uint32_t u = le ? (f[i] | f[i + 1] << 8) : (f[i] << 8 | f[i + 1]);
      if (!u) break;
      if (u >= 0xd800 && u < 0xdc00 && i + 3 < n) {
        uint32_t lo = le ? (f[i + 2] | f[i + 3] << 8) : (f[i + 2] << 8 | f[i + 3]);
        u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
        i += 2;
      }
      appendUtf8(out, u);
    }
  }
  return trim(out);
}

bool id3(int fd, Tags& t, uint64_t* audioStart) {
  uint8_t h[10];
  if (!readAt(fd, 0, h, 10)) return false;
  int version = h[3];
  uint32_t size = syncsafe(h + 6);
  *audioStart = 10 + size;
  if (version < 3 || size > (16u << 20)) return false;
  std::vector<uint8_t> tag(size);
  if (!readAt(fd, 10, tag.data(), size)) return false;
  size_t p = 0;
  while (p + 10 <= size && tag[p]) {
    uint32_t len = version == 4 ? syncsafe(&tag[p + 4]) : be32(&tag[p + 4]);
    if (p + 10 + len > size) break;
    std::string id((const char*)&tag[p], 4);
    const uint8_t* f = &tag[p + 10];
    if (id == "TIT2") t.title = id3Text(f, len);
    else if (id == "TPE1") t.artist = id3Text(f, len);
    else if (id == "TPE2") t.albumArtist = id3Text(f, len);
    else if (id == "TALB") t.album = id3Text(f, len);
    else if (id == "TRCK") t.track = trackNumber(id3Text(f, len));
    else if (id == "TLEN") t.durationSec = atoi(id3Text(f, len).c_str()) / 1000;
    p += 10 + len;
  }
  return true;
}

// MP3 duration without TLEN: a Xing/Info frame count, else the file size at the first frame's bitrate.
void mp3Duration(int fd, uint64_t start, uint64_t fileSize, Tags& t) {
  uint8_t b[256];
  if (!readAt(fd, start, b, sizeof(b))) return;
  if (b[0] != 0xff || (b[1] & 0xe0) != 0xe0) return;
  static const int kRates[2][16] = {{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
                                    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}};
  static const int kHz[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
  int ver = (b[1] >> 3) & 3, rateIdx = b[2] >> 4, hzIdx = (b[2] >> 2) & 3;
  if (hzIdx == 3 || ver == 1) return;
  int hz = kHz[ver][hzIdx], kbps = kRates[ver == 3 ? 0 : 1][rateIdx];
  int samplesPerFrame = ver == 3 ? 1152 : 576;
  for (int off = 4; off + 12 < int(sizeof(b)); off++) {
    if (!memcmp(b + off, "Xing", 4) || !memcmp(b + off, "Info", 4)) {
      if (be32(b + off + 4) & 1) {
        uint32_t frames = be32(b + off + 8);
        if (hz) t.durationSec = int(uint64_t(frames) * samplesPerFrame / hz);
        return;
      }
    }
  }
  if (kbps) t.durationSec = int((fileSize - start) * 8 / (uint64_t(kbps) * 1000));
}

// Ogg: the comment header in the first pages, the duration from the last page's granule position.
bool ogg(int fd, uint64_t fileSize, Tags& t) {
  std::vector<uint8_t> b(65536);
  ssize_t n = pread64(fd, b.data(), b.size(), 0);
  if (n < 64) return false;
  b.resize(size_t(n));
  uint32_t rate = 0, preskip = 0;
  for (size_t i = 0; i + 16 < b.size(); i++) {
    if (!memcmp(&b[i], "\x01vorbis", 7) && i + 16 < b.size()) rate = le32(&b[i + 12]);
    else if (!memcmp(&b[i], "OpusHead", 8)) rate = 48000, preskip = b[i + 10] | b[i + 11] << 8;
    else if (!memcmp(&b[i], "\x03vorbis", 7)) {
      vorbisComments(&b[i + 7], b.size() - i - 7, t);
      break;
    } else if (!memcmp(&b[i], "OpusTags", 8)) {
      vorbisComments(&b[i + 8], b.size() - i - 8, t);
      break;
    }
  }
  uint64_t tail = fileSize > 65536 ? fileSize - 65536 : 0;
  std::vector<uint8_t> e(size_t(fileSize - tail));
  if (rate && readAt(fd, tail, e.data(), e.size())) {
    for (size_t i = e.size() >= 14 ? e.size() - 14 : 0; i-- > 0;) {
      if (!memcmp(&e[i], "OggS", 4)) {
        uint64_t granule = le64(&e[i + 6]);
        if (granule > preskip) t.durationSec = int((granule - preskip) / rate);
        break;
      }
    }
  }
  return true;
}

bool wav(int fd, uint64_t fileSize, Tags& t) {
  uint8_t h[12];
  if (!readAt(fd, 0, h, 12) || memcmp(h + 8, "WAVE", 4)) return false;
  uint64_t at = 12;
  uint32_t byteRate = 0;
  while (at + 8 <= fileSize) {
    uint8_t c[16];
    if (!readAt(fd, at, c, 8)) break;
    uint32_t len = le32(c + 4);
    if (!memcmp(c, "fmt ", 4) && len >= 12 && readAt(fd, at + 8, c, 12)) byteRate = le32(c + 8);
    if (!memcmp(c, "data", 4)) {
      if (byteRate) t.durationSec = int(len / byteRate);
      break;
    }
    at += 8 + len + (len & 1);
  }
  return true;
}

}  // namespace

Tags readTags(const char* path) {
  Tags t;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return t;
  off64_t size = lseek64(fd, 0, SEEK_END);
  uint8_t m[12];
  if (readAt(fd, 0, m, 12)) {
    if (!memcmp(m, "fLaC", 4)) {
      t.read = flac(fd, 0, t);
    } else if (!memcmp(m + 4, "ftyp", 4)) {
      mp4Walk(fd, 0, uint64_t(size), 0, t);
      t.read = true;
    } else if (!memcmp(m, "OggS", 4)) {
      t.read = ogg(fd, uint64_t(size), t);
    } else if (!memcmp(m, "RIFF", 4)) {
      t.read = wav(fd, uint64_t(size), t);
    } else if (!memcmp(m, "ID3", 3)) {
      uint64_t audio = 0;
      t.read = id3(fd, t, &audio);
      uint8_t f[4];
      if (readAt(fd, audio, f, 4) && !memcmp(f, "fLaC", 4)) flac(fd, audio, t);
      else if (t.durationSec < 0) mp3Duration(fd, audio, uint64_t(size), t);
    } else if (m[0] == 0xff && (m[1] & 0xe0) == 0xe0) {
      mp3Duration(fd, 0, uint64_t(size), t);
      t.read = true;
    }
  }
  close(fd);
  return t;
}

}  // namespace rondo
