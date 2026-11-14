// tag_write.h.
#include "tag_write.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/vfs.h>  // statfs: statvfs is API 19
#endif
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#ifdef __APPLE__  // a host test: off_t is 64 bits on the Mac (32 on the Y1, hence the 64 calls)
#define pread64 pread
#define pwrite64 pwrite
#define ftruncate64 ftruncate
#define fstat64 fstat
#define stat64 stat
#define off64_t off_t
#endif

namespace rondo {

namespace {

using Bytes = std::vector<uint8_t>;

std::string extOf(const std::string& p) {
  size_t slash = p.rfind('/'), dot = p.rfind('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
  std::string x = p.substr(dot + 1);
  for (char& c : x) c = char(tolower((unsigned char)c));
  return x;
}

bool readAt(int fd, uint64_t at, void* out, size_t n) {
  uint8_t* p = static_cast<uint8_t*>(out);
  while (n) {
    ssize_t r = pread64(fd, p, n, off64_t(at));
    if (r <= 0) return false;
    p += r, at += uint64_t(r), n -= size_t(r);
  }
  return true;
}
bool writeAt(int fd, uint64_t at, const uint8_t* p, size_t n) {
  while (n) {
    ssize_t w = pwrite64(fd, p, n, off64_t(at));
    if (w <= 0) return false;
    p += w, at += uint64_t(w), n -= size_t(w);
  }
  return true;
}
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
uint32_t syncsafe(const uint8_t* p) { return uint32_t(p[0] & 0x7f) << 21 | uint32_t(p[1] & 0x7f) << 14 | uint32_t(p[2] & 0x7f) << 7 | (p[3] & 0x7f); }
void putBe32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
void putBe64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (56 - 8 * i));
}
void putLe32(Bytes& b, uint32_t v) {
  for (int s = 0; s <= 24; s += 8) b.push_back(uint8_t(v >> s));
}
void putSyncsafe(Bytes& b, uint32_t v) {
  for (int s = 21; s >= 0; s -= 7) b.push_back(uint8_t((v >> s) & 0x7f));
}
void append(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
void append(Bytes& b, const Bytes& s) { b.insert(b.end(), s.begin(), s.end()); }

// The file written again beside itself: `head`, then the old file from `from` on, put in its place
// once it's all on the card.
std::string rewrite(const std::string& path, int fd, const Bytes& head, uint64_t from) {
  struct stat64 st;
  if (fstat64(fd, &st) != 0) return strerror(errno);
  uint64_t size = uint64_t(st.st_size), total = head.size() + (size > from ? size - from : 0);
  size_t slash = path.rfind('/');
  std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
  struct statfs vs;
  if (statfs(dir.c_str(), &vs) == 0 && uint64_t(vs.f_bavail) * uint64_t(vs.f_bsize) < total + (1 << 20)) return "not enough space on the card";
  std::string tmp = dir + "/.rondo-tags-" + path.substr(slash + 1);
  if (tmp.size() - dir.size() > 200) tmp = dir + "/.rondo-tags.tmp";
  int out = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (out < 0) return strerror(errno);
  bool ok = writeAt(out, 0, head.data(), head.size());
  std::vector<uint8_t> buf(1 << 20);
  uint64_t at = from, to = head.size();
  while (ok && at < size) {
    size_t n = size_t(std::min<uint64_t>(buf.size(), size - at));
    ok = readAt(fd, at, buf.data(), n) && writeAt(out, to, buf.data(), n);
    at += n, to += n;
  }
  std::string why = ok ? "" : strerror(errno ? errno : EIO);
  if (ok && fsync(out) != 0) ok = false, why = strerror(errno);
  close(out);
  if (ok && rename(tmp.c_str(), path.c_str()) != 0) ok = false, why = strerror(errno);
  if (!ok) unlink(tmp.c_str());
  return why;
}

std::string inPlace(int fd, uint64_t at, const Bytes& b) {
  if (!writeAt(fd, at, b.data(), b.size())) return strerror(errno ? errno : EIO);
  if (fsync(fd) != 0) return strerror(errno);
  return "";
}

// ---------- FLAC: the VORBIS_COMMENT block ----------

struct VorbisField {
  const char* key;
  const char* also;  // another name the same field goes by, removed with it
};

std::string flac(const std::string& path, int fd, const TagChanges& c) {
  uint8_t h[10];
  if (!readAt(fd, 0, h, 10)) return "not a FLAC file";
  uint64_t base = 0;
  if (!memcmp(h, "ID3", 3)) base = 10 + syncsafe(h + 6) + (h[5] & 0x10 ? 10 : 0);  // an ID3v2 tag before it, kept
  uint8_t magic[4];
  if (!readAt(fd, base, magic, 4) || memcmp(magic, "fLaC", 4)) return "not a FLAC file";
  struct Block {
    int type;
    Bytes data;
  };
  std::vector<Block> blocks;
  Bytes comment;
  bool hasComment = false;
  uint64_t at = base + 4;
  for (int n = 0;; n++) {
    uint8_t bh[4];
    if (n > 1000 || !readAt(fd, at, bh, 4)) return "its tags aren't readable";
    uint32_t len = uint32_t(bh[1]) << 16 | uint32_t(bh[2]) << 8 | bh[3];
    int type = bh[0] & 0x7f;
    if (type == 4 && !hasComment) {
      comment.resize(len);
      if (!readAt(fd, at + 4, comment.data(), len)) return "its tags aren't readable";
      hasComment = true;
    } else if (type != 1 && type != 4) {  // padding left out (made again), and a second comment block
      Block b{type, Bytes(len)};
      if (len && !readAt(fd, at + 4, b.data.data(), len)) return "its tags aren't readable";
      blocks.push_back(std::move(b));
    }
    at += 4 + len;
    if (bh[0] & 0x80) break;
  }
  uint64_t audio = at;
  if (blocks.empty() || blocks[0].type != 0) return "its tags aren't readable";
  // the comments: vendor, then KEY=value strings
  std::string vendor = "Rondo";
  std::vector<std::string> fields;
  if (hasComment && comment.size() >= 8) {
    size_t p = 0;
    uint32_t vl = le32(&comment[p]);
    p += 4;
    if (p + vl + 4 <= comment.size()) {
      vendor.assign((const char*)&comment[p], vl), p += vl;
      uint32_t count = le32(&comment[p]);
      p += 4;
      for (uint32_t i = 0; i < count && p + 4 <= comment.size(); i++) {
        uint32_t l = le32(&comment[p]);
        p += 4;
        if (p + l > comment.size()) break;
        fields.emplace_back((const char*)&comment[p], l), p += l;
      }
    }
  }
  auto set = [&](std::initializer_list<const char*> keys, const std::string& v) {
    if (v.empty()) return;
    std::vector<std::string> kept;
    for (const std::string& f : fields) {
      size_t eq = f.find('=');
      std::string k = f.substr(0, eq);
      bool drop = false;
      for (const char* key : keys) drop = drop || !strcasecmp(k.c_str(), key);
      if (!drop) kept.push_back(f);
    }
    kept.push_back(std::string(*keys.begin()) + "=" + v);
    fields = kept;
  };
  set({"TITLE"}, c.title);
  set({"ARTIST"}, c.artist);
  set({"ALBUMARTIST", "ALBUM ARTIST"}, c.albumArtist);
  set({"ALBUM"}, c.album);
  set({"GENRE"}, c.genre);
  set({"DATE", "YEAR"}, c.year ? std::to_string(c.year) : "");
  set({"TRACKNUMBER"}, c.track ? std::to_string(c.track) : "");
  set({"DISCNUMBER"}, c.disc ? std::to_string(c.disc) : "");
  set({"LYRICS", "UNSYNCEDLYRICS"}, c.lyrics);
  Bytes vc;
  putLe32(vc, uint32_t(vendor.size()));
  append(vc, vendor);
  putLe32(vc, uint32_t(fields.size()));
  for (const std::string& f : fields) putLe32(vc, uint32_t(f.size())), append(vc, f);
  if (vc.size() >= (1u << 24)) return "its tags would be too big";
  // STREAMINFO, the other blocks as they were, the comments, then padding
  auto build = [&](uint64_t padding, bool withPadding) {
    Bytes out;
    std::vector<std::pair<int, const Bytes*>> all;
    all.push_back({blocks[0].type, &blocks[0].data});
    all.push_back({4, &vc});
    for (size_t i = 1; i < blocks.size(); i++) all.push_back({blocks[i].type, &blocks[i].data});
    for (size_t i = 0; i < all.size(); i++) {
      bool last = i + 1 == all.size() && !withPadding;
      uint32_t len = uint32_t(all[i].second->size());
      out.push_back(uint8_t((last ? 0x80 : 0) | all[i].first));
      out.push_back(uint8_t(len >> 16)), out.push_back(uint8_t(len >> 8)), out.push_back(uint8_t(len));
      append(out, *all[i].second);
    }
    if (withPadding) {
      out.push_back(0x81);
      out.push_back(uint8_t(padding >> 16)), out.push_back(uint8_t(padding >> 8)), out.push_back(uint8_t(padding));
      out.resize(out.size() + padding, 0);
    }
    return out;
  };
  uint64_t room = audio - (base + 4);
  Bytes tight = build(0, false);
  if (tight.size() == room) return inPlace(fd, base + 4, tight);
  if (tight.size() + 4 <= room && room - tight.size() - 4 < (1u << 24)) return inPlace(fd, base + 4, build(room - tight.size() - 4, true));
  Bytes head(base + 4);
  if (!readAt(fd, 0, head.data(), head.size())) return "its tags aren't readable";
  append(head, build(16384, true));
  return rewrite(path, fd, head, audio);
}

// ---------- MP3: ID3v2 ----------

bool ascii(const std::string& s) {
  for (unsigned char c : s)
    if (c >= 0x80) return false;
  return true;
}
// UTF-8 to UTF-16LE with a BOM (ID3v2.3's text for anything but ASCII)
Bytes utf16(const std::string& s) {
  Bytes o = {0xff, 0xfe};
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i];
    uint32_t cp;
    int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (i + n > s.size()) break;
    cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
    for (int k = 1; k < n; k++) cp = cp << 6 | (uint8_t(s[i + k]) & 0x3f);
    i += n;
    auto unit = [&](uint32_t u) { o.push_back(uint8_t(u)), o.push_back(uint8_t(u >> 8)); };
    if (cp >= 0x10000) {
      cp -= 0x10000;
      unit(0xd800 | (cp >> 10)), unit(0xdc00 | (cp & 0x3ff));
    } else {
      unit(cp);
    }
  }
  return o;
}

std::string id3(const std::string& path, int fd, const TagChanges& c) {
  uint8_t h[10];
  int version = 3;
  uint64_t old = 0;  // the old tag's bytes (header, frames, padding, footer)
  std::vector<std::pair<std::string, Bytes>> frames;  // kept as they were, id and whole frame
  if (readAt(fd, 0, h, 10) && !memcmp(h, "ID3", 3)) {
    version = h[3];
    int flags = h[5];
    uint32_t size = syncsafe(h + 6);
    old = 10 + uint64_t(size) + (version == 4 && (flags & 0x10) ? 10 : 0);
    if (version < 3 || version > 4) return "its ID3v2." + std::to_string(version) + " tags are too old to save into";
    if (flags & 0x80) return "its tags use ID3 unsynchronization, which Rondo can't save into";
    Bytes tag(size);
    if (size > (64u << 20) || !readAt(fd, 10, tag.data(), size)) return "its tags aren't readable";
    size_t p = 0;
    if (flags & 0x40) p = version == 4 ? syncsafe(&tag[0]) : 4 + be32(&tag[0]);  // an extended header: left out
    while (p + 10 <= size && tag[p]) {
      uint32_t len = version == 4 ? syncsafe(&tag[p + 4]) : be32(&tag[p + 4]);
      if (p + 10 + len > size) break;
      frames.push_back({std::string((const char*)&tag[p], 4), Bytes(tag.begin() + long(p), tag.begin() + long(p + 10 + len))});
      p += 10 + len;
    }
  }
  auto frame = [&](const std::string& id, const Bytes& body) {
    Bytes f;
    append(f, id);
    if (version == 4) putSyncsafe(f, uint32_t(body.size()));
    else putBe32(f, uint32_t(body.size()));
    f.push_back(0), f.push_back(0);
    append(f, body);
    return f;
  };
  // a text: UTF-8 in 2.4; Latin-1 for ASCII, else UTF-16 with a BOM, in 2.3
  auto encoded = [&](const std::string& s, Bytes& out) {
    if (version == 4) out.push_back(3), append(out, s);
    else if (ascii(s)) out.push_back(0), append(out, s);
    else out.push_back(1), append(out, utf16(s));
  };
  auto drop = [&](std::initializer_list<const char*> ids) {
    std::vector<std::pair<std::string, Bytes>> kept;
    for (auto& f : frames) {
      bool gone = false;
      for (const char* id : ids) gone = gone || f.first == id;
      if (!gone) kept.push_back(std::move(f));
    }
    frames = std::move(kept);
  };
  auto text = [&](const char* id, std::initializer_list<const char*> also, const std::string& v) {
    if (v.empty()) return;
    drop({id}), drop(also);
    Bytes body;
    encoded(v, body);
    frames.push_back({id, frame(id, body)});
  };
  text("TIT2", {}, c.title);
  text("TPE1", {}, c.artist);
  text("TPE2", {}, c.albumArtist);
  text("TALB", {}, c.album);
  text("TCON", {}, c.genre);
  if (c.year) text(version == 4 ? "TDRC" : "TYER", {"TDRC", "TYER"}, std::to_string(c.year));
  text("TRCK", {}, c.track ? std::to_string(c.track) : "");
  text("TPOS", {}, c.disc ? std::to_string(c.disc) : "");
  if (!c.lyrics.empty()) {  // USLT: encoding, language, an empty description, the text
    drop({"USLT"});
    Bytes body;
    bool wide = version != 4 && !ascii(c.lyrics);
    body.push_back(version == 4 ? 3 : wide ? 1 : 0);
    append(body, std::string("XXX"));
    if (wide) {  // the description (a BOM and its two-byte end), then the text with its own BOM
      Bytes empty = {0xff, 0xfe, 0, 0};
      append(body, empty);
      append(body, utf16(c.lyrics));
    } else {
      body.push_back(0);
      append(body, c.lyrics);
    }
    frames.push_back({"USLT", frame("USLT", body)});
  }
  Bytes data;
  for (auto& f : frames) append(data, f.second);
  auto tag = [&](uint64_t padding) {
    Bytes t = {'I', 'D', '3', uint8_t(version), 0, 0};
    putSyncsafe(t, uint32_t(data.size() + padding));
    append(t, data);
    t.resize(t.size() + padding, 0);
    return t;
  };
  if (old && 10 + data.size() <= old) return inPlace(fd, 0, tag(old - 10 - data.size()));
  return rewrite(path, fd, tag(4096), old);
}

// ---------- MP4: moov/udta/meta/ilst ----------

struct Box {
  std::string type;
  uint64_t at = 0, size = 0, header = 8;  // where in its parent's bytes, its whole size, its header's
};
// the boxes in b[from, to)
std::vector<Box> boxes(const Bytes& b, uint64_t from, uint64_t to) {
  std::vector<Box> out;
  uint64_t at = from;
  while (at + 8 <= to) {
    Box x;
    x.at = at, x.size = be32(&b[at]), x.type.assign((const char*)&b[at + 4], 4);
    if (x.size == 1) {
      if (at + 16 > to) break;
      x.size = be64(&b[at + 8]), x.header = 16;
    } else if (x.size == 0) {
      x.size = to - at;
    }
    if (x.size < x.header || at + x.size > to) break;
    out.push_back(x);
    at += x.size;
  }
  return out;
}
Bytes box(const char* type, const Bytes& body) {
  Bytes o;
  putBe32(o, uint32_t(8 + body.size()));
  append(o, std::string(type, 4));
  append(o, body);
  return o;
}
Bytes slice(const Bytes& b, const Box& x) { return Bytes(b.begin() + long(x.at), b.begin() + long(x.at + x.size)); }

Bytes dataBox(uint32_t kind, const Bytes& payload) {
  Bytes d;
  putBe32(d, kind);  // 1: UTF-8 text; 0: numbers as in trkn and disk
  putBe32(d, 0);     // locale
  append(d, payload);
  return box("data", d);
}

// the new ilst: the old one's items, those being set made again
Bytes ilst(const Bytes& old, const TagChanges& c) {
  struct Item {
    std::string type;
    Bytes whole;
  };
  std::vector<Item> items;
  for (const Box& x : boxes(old, 8, old.size())) items.push_back({x.type, slice(old, x)});
  auto set = [&](const std::string& type, std::initializer_list<const char*> also, const Bytes& value) {
    std::vector<Item> kept;
    for (Item& it : items) {
      bool gone = it.type == type;
      for (const char* a : also) gone = gone || it.type == a;
      if (!gone) kept.push_back(std::move(it));
    }
    kept.push_back({type, box(type.c_str(), value)});
    items = std::move(kept);
  };
  auto textItem = [&](const char* type, std::initializer_list<const char*> also, const std::string& v) {
    if (v.empty()) return;
    Bytes s(v.begin(), v.end());
    set(type, also, dataBox(1, s));
  };
  textItem("\xa9nam", {}, c.title);
  textItem("\xa9" "ART", {}, c.artist);
  textItem("aART", {}, c.albumArtist);
  textItem("\xa9" "alb", {}, c.album);
  textItem("\xa9gen", {"gnre"}, c.genre);
  textItem("\xa9" "day", {}, c.year ? std::to_string(c.year) : "");
  textItem("\xa9lyr", {}, c.lyrics);
  if (c.track) set("trkn", {}, dataBox(0, {0, 0, uint8_t(c.track >> 8), uint8_t(c.track), 0, 0, 0, 0}));
  if (c.disc) set("disk", {}, dataBox(0, {0, 0, uint8_t(c.disc >> 8), uint8_t(c.disc), 0, 0}));
  Bytes body;
  for (Item& it : items) append(body, it.whole);
  return box("ilst", body);
}

// moov again with the new ilst (udta and meta made when missing; a free box inside meta left out)
Bytes newMoov(const Bytes& moov, const TagChanges& c, bool& fragmented) {
  static const uint8_t kHdlr[] = {0, 0, 0, 33, 'h', 'd', 'l', 'r', 0, 0, 0, 0, 0, 0, 0, 0, 'm', 'd', 'i', 'r', 'a', 'p', 'p', 'l', 0, 0, 0, 0, 0, 0, 0, 0, 0};
  Bytes body;
  bool sawUdta = false;
  for (const Box& x : boxes(moov, 8, moov.size())) {
    if (x.type == "mvex") fragmented = true;
    if (x.type != "udta") {
      append(body, slice(moov, x));
      continue;
    }
    sawUdta = true;
    Bytes u = slice(moov, x), ubody;
    bool sawMeta = false;
    for (const Box& y : boxes(u, 8, u.size())) {
      if (y.type != "meta") {
        append(ubody, slice(u, y));
        continue;
      }
      sawMeta = true;
      // a full box (version and flags before its children), or QuickTime's plain one
      Bytes m = slice(u, y);
      bool full = !(m.size() >= 16 && !memcmp(&m[12], "hdlr", 4));
      Bytes mbody(m.begin() + 8, m.begin() + (full ? 12 : 8));
      bool sawIlst = false, sawHdlr = false;
      for (const Box& z : boxes(m, full ? 12 : 8, m.size())) {
        if (z.type == "ilst") sawIlst = true, append(mbody, ilst(slice(m, z), c));
        else if (z.type == "free") continue;
        else {
          if (z.type == "hdlr") sawHdlr = true;
          append(mbody, slice(m, z));
        }
      }
      if (!sawHdlr) mbody.insert(mbody.begin() + (full ? 4 : 0), kHdlr, kHdlr + sizeof(kHdlr));
      if (!sawIlst) append(mbody, ilst(box("ilst", {}), c));
      append(ubody, box("meta", mbody));
    }
    if (!sawMeta) {
      Bytes mbody = {0, 0, 0, 0};
      mbody.insert(mbody.end(), kHdlr, kHdlr + sizeof(kHdlr));
      append(mbody, ilst(box("ilst", {}), c));
      append(ubody, box("meta", mbody));
    }
    append(body, box("udta", ubody));
  }
  if (!sawUdta) {
    Bytes mbody = {0, 0, 0, 0};
    mbody.insert(mbody.end(), kHdlr, kHdlr + sizeof(kHdlr));
    append(mbody, ilst(box("ilst", {}), c));
    append(body, box("udta", box("meta", mbody)));
  }
  return box("moov", body);
}

// every chunk offset (stco, co64) in a moov past `after` moved by delta; false when one wouldn't fit
bool shiftChunks(Bytes& b, uint64_t from, uint64_t to, int64_t delta, uint64_t after) {
  for (const Box& x : boxes(b, from, to)) {
    uint64_t body = x.at + x.header, end = x.at + x.size;
    if (x.type == "trak" || x.type == "mdia" || x.type == "minf" || x.type == "stbl") {
      if (!shiftChunks(b, body, end, delta, after)) return false;
    } else if ((x.type == "stco" || x.type == "co64") && body + 8 <= end) {
      uint32_t n = be32(&b[body + 4]);
      bool wide = x.type == "co64";
      for (uint32_t i = 0; i < n; i++) {
        uint64_t at = body + 8 + uint64_t(i) * (wide ? 8 : 4);
        if (at + (wide ? 8 : 4) > end) break;
        if (wide) {
          if (be64(&b[at]) >= after) putBe64(&b[at], uint64_t(int64_t(be64(&b[at])) + delta));
        } else {
          if (be32(&b[at]) < after) continue;
          int64_t v = int64_t(be32(&b[at])) + delta;
          if (v < 0 || v > int64_t(UINT32_MAX)) return false;
          b[at] = uint8_t(v >> 24), b[at + 1] = uint8_t(v >> 16), b[at + 2] = uint8_t(v >> 8), b[at + 3] = uint8_t(v);
        }
      }
    }
  }
  return true;
}

std::string mp4(const std::string& path, int fd, const TagChanges& c) {
  struct stat64 st;
  if (fstat64(fd, &st) != 0) return strerror(errno);
  uint64_t size = uint64_t(st.st_size);
  // the top-level boxes, read by their headers only
  struct Top {
    std::string type;
    uint64_t at, size;
  };
  std::vector<Top> tops;
  for (uint64_t at = 0; at + 8 <= size;) {
    uint8_t h[16];
    if (!readAt(fd, at, h, 8)) break;
    uint64_t s = be32(h);
    if (s == 1 && readAt(fd, at + 8, h + 8, 8)) s = be64(h + 8);
    else if (s == 0) s = size - at;
    if (s < 8 || at + s > size) break;
    tops.push_back({std::string((const char*)h + 4, 4), at, s});
    at += s;
  }
  size_t mi = tops.size();
  bool moof = false, mdatAfter = false;
  for (size_t i = 0; i < tops.size(); i++) {
    if (tops[i].type == "moov" && mi == tops.size()) mi = i;
    else if (tops[i].type == "moof") moof = true;
    else if (tops[i].type == "mdat" && mi != tops.size()) mdatAfter = true;
  }
  if (mi == tops.size()) return "not an MP4 file Rondo can read";
  const Top& mv = tops[mi];
  if (mv.size > (64u << 20)) return "its index is too big";
  Bytes moov(mv.size);
  if (!readAt(fd, mv.at, moov.data(), moov.size())) return "its tags aren't readable";
  if (be32(&moov[0]) == 1) return "its index is too big";
  bool fragmented = moof;
  Bytes nm = newMoov(moov, c, fragmented);
  // room where moov is: moov, and a free box right after it
  uint64_t room = mv.size;
  if (mi + 1 < tops.size() && (tops[mi + 1].type == "free" || tops[mi + 1].type == "skip")) room += tops[mi + 1].size;
  auto padded = [&](uint64_t total) {  // nm and a free box filling it to `total` (total - nm.size() >= 8)
    Bytes b = nm;
    Bytes free = box("free", Bytes(size_t(total - nm.size() - 8), 0));
    append(b, free);
    return b;
  };
  if (nm.size() == room) return inPlace(fd, mv.at, nm);
  if (nm.size() + 8 <= room) return inPlace(fd, mv.at, padded(room));
  if (!mdatAfter) {  // moov at the end: it grows into the file's end
    std::string why = inPlace(fd, mv.at, nm);
    if (why.empty() && ftruncate64(fd, off64_t(mv.at + nm.size())) != 0) why = strerror(errno);
    return why;
  }
  if (fragmented) return "it's a fragmented MP4, which Rondo can't save into";
  // moov before the sound: the file again, with every chunk offset moved
  uint64_t total = nm.size() + 4096;
  Bytes grown = padded(total);
  int64_t delta = int64_t(total) - int64_t(room);
  if (!shiftChunks(grown, 8, nm.size(), delta, mv.at)) return "it's too big to save into";
  Bytes head(mv.at);
  if (mv.at && !readAt(fd, 0, head.data(), head.size())) return "its tags aren't readable";
  append(head, grown);
  return rewrite(path, fd, head, mv.at + room);
}

}  // namespace

bool canWriteTags(const std::string& path) {
  std::string x = extOf(path);
  return x == "flac" || x == "mp3" || x == "m4a" || x == "m4b" || x == "mp4" || x == "aac";
}

std::string writeTags(const std::string& path, const TagChanges& c) {
  std::string x = extOf(path);
  if (!canWriteTags(path)) return "Rondo can't save tags into ." + x + " files yet";
  int fd = open(path.c_str(), O_RDWR);
  if (fd < 0) return strerror(errno);
  uint8_t h[12] = {0};
  std::string why;
  bool got = readAt(fd, 0, h, sizeof(h));
  if (!got) why = "the file is empty";
  else if (x == "flac") why = flac(path, fd, c);
  else if (x == "mp3") why = id3(path, fd, c);
  else if (!memcmp(h + 4, "ftyp", 4)) why = mp4(path, fd, c);
  else why = "Rondo can't save tags into this ." + x + " file";  // ADTS .aac has no tags of this kind
  close(fd);
  return why;
}

}  // namespace rondo
