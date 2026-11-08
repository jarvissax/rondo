// bundle.h.
#include "bundle.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include <openssl/evp.h>
#include <zlib.h>

namespace rondo::bundle {
namespace {

[[maybe_unused]] std::vector<uint8_t> base64(const std::string& s) {
  std::vector<uint8_t> out;
  unsigned v = 0;
  int bits = 0;
  for (char c : s) {
    int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
          : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
    if (d < 0) continue;  // padding, line breaks
    v = v << 6 | unsigned(d);
    bits += 6;
    if (bits >= 8) bits -= 8, out.push_back(uint8_t(v >> bits));
  }
  return out;
}

bool mkdirs(const std::string& dir) {
  for (size_t at = 1; at <= dir.size(); at++)
    if (at == dir.size() || dir[at] == '/') {
      std::string d = dir.substr(0, at);
      if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
  return true;
}

void removeTree(const std::string& path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    if (DIR* d = opendir(path.c_str())) {
      while (dirent* e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) removeTree(path + "/" + e->d_name);
      closedir(d);
    }
    rmdir(path.c_str());
  } else {
    unlink(path.c_str());
  }
}

// A ustar archive through zlib: regular files and folders, each kept inside `dir`
bool unpack(const std::string& file, const std::string& dir, std::string& why) {
  int fd0 = open(file.c_str(), O_RDONLY);
  if (fd0 < 0 || lseek(fd0, off_t(kHeader), SEEK_SET) != off_t(kHeader)) {
    if (fd0 >= 0) close(fd0);
    return why = "the update can't be opened", false;
  }
  gzFile in = gzdopen(fd0, "rb");  // the archive after the header
  if (!in) return close(fd0), why = "the update can't be opened", false;
  std::vector<char> buf(64 * 1024);
  bool ok = true;
  std::string longName;
  for (;;) {
    char h[512];
    if (gzread(in, h, 512) != 512) break;
    if (!h[0]) break;  // the end's zero blocks
    std::string name(h, strnlen(h, 100)), prefix(h + 345, strnlen(h + 345, 155));
    if (!prefix.empty()) name = prefix + "/" + name;
    if (!longName.empty()) name = longName, longName.clear();
    long long size = strtoll(std::string(h + 124, 12).c_str(), nullptr, 8);
    unsigned mode = unsigned(strtoul(std::string(h + 100, 8).c_str(), nullptr, 8));
    char type = h[156];
    long long padded = (size + 511) / 512 * 512;
    while (name.compare(0, 2, "./") == 0) name.erase(0, 2);
    // nothing may land outside the folder
    if (name.find("..") != std::string::npos || (!name.empty() && name[0] == '/')) {
      ok = false, why = "the update holds a bad path";
      break;
    }
    if (type == 'L') {  // GNU long name: the next entry's name
      std::string n(size_t(size), '\0');
      if (gzread(in, &n[0], unsigned(size)) != size) return gzclose(in), why = "the update is cut short", false;
      longName = std::string(n.c_str());
      if (padded > size) gzseek(in, padded - size, SEEK_CUR);
      continue;
    }
    std::string path = dir + "/" + name;
    if (type == '5') {
      if (!mkdirs(path)) ok = false, why = "a folder can't be made";
    } else if (type == '0' || type == '\0') {
      mkdirs(path.substr(0, path.rfind('/')));
      int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, (mode & 0111) ? 0755 : 0644);
      if (fd < 0) ok = false, why = "a file can't be written";
      long long left = size;
      while (ok && left > 0) {
        int n = gzread(in, buf.data(), unsigned(std::min<long long>(left, (long long)buf.size())));
        if (n <= 0 || write(fd, buf.data(), size_t(n)) != n) ok = false, why = "the update is cut short";
        left -= n;
      }
      if (fd >= 0) fsync(fd), close(fd);
      if (padded > size && ok) gzseek(in, padded - size, SEEK_CUR);
    } else if (padded) {  // links and the rest: not in a bundle, skipped
      gzseek(in, padded, SEEK_CUR);
    }
    if (!ok) break;
  }
  gzclose(in);
  return ok;
}

}  // namespace

bool verify(const std::string& file, std::string& why) {
  FILE* f = fopen(file.c_str(), "rb");
  if (!f) return why = "the download is missing", false;
  uint8_t head[kHeader];
  if (fread(head, 1, kHeader, f) != kHeader || memcmp(head, "RONDOB1\n", 8) != 0) {
    fclose(f);
    return why = "this isn't a Rondo update", false;
  }
  std::vector<uint8_t> sig(head + 8, head + kHeader);
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  EVP_DigestInit_ex(md, EVP_sha256(), nullptr);
  std::vector<uint8_t> buf(64 * 1024);
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) EVP_DigestUpdate(md, buf.data(), n);
  fclose(f);
  uint8_t digest[32];
  unsigned dl = 0;
  EVP_DigestFinal_ex(md, digest, &dl);
  EVP_MD_CTX_free(md);
  EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, kPublicKey, sizeof(kPublicKey));
  EVP_MD_CTX* v = EVP_MD_CTX_new();
  bool good = key && v && EVP_DigestVerifyInit(v, nullptr, nullptr, nullptr, key) == 1 &&
              EVP_DigestVerify(v, sig.data(), sig.size(), digest, sizeof(digest)) == 1;
  EVP_MD_CTX_free(v);
  EVP_PKEY_free(key);
  if (!good) return why = "the update's signature isn't Rondo's", false;
  return true;
}

bool install(const std::string& file, const std::string& dataDir, std::string& version, std::string& why) {
  if (!verify(file, why)) return false;
  std::string part = dataDir + "/update.part";
  removeTree(part);
  if (!mkdirs(part) || !unpack(file, part, why)) {
    removeTree(part);
    return false;
  }
  version.clear();
  if (FILE* v = fopen((part + "/VERSION").c_str(), "r")) {
    char b[64] = {0};
    if (fgets(b, sizeof(b), v)) version = b;
    fclose(v);
  }
  while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' ')) version.pop_back();
  bool odd = version.empty() || !isdigit((unsigned char)version[0]);
  for (char c : version) odd |= !(isalnum((unsigned char)c) || c == '.' || c == '-');
  if (odd) {
    removeTree(part);
    return why = "the update's version is odd", false;
  }
  std::string dir = dataDir + "/" + version;
  struct stat st;
  if (stat((part + "/rondo").c_str(), &st) != 0) {
    removeTree(part);
    return why = "the update has no Rondo in it", false;
  }
  chmod((part + "/rondo").c_str(), 0755);
  // the version that ran (this one) stays, for the start script to fall back on; older ones go
  std::string running;
  if (FILE* c = fopen((dataDir + "/current").c_str(), "r")) {
    char b[64] = {0};
    if (fgets(b, sizeof(b), c)) running = b;
    fclose(c);
    while (!running.empty() && (running.back() == '\n' || running.back() == '\r')) running.pop_back();
  }
  removeTree(dir);
  if (rename(part.c_str(), dir.c_str()) != 0) return why = "the update can't be put in place", false;
  std::string tmp = dataDir + "/current.part";
  if (FILE* c = fopen(tmp.c_str(), "w")) {
    fprintf(c, "%s\n", version.c_str());
    fflush(c);
    fsync(fileno(c));
    fclose(c);
  }
  if (rename(tmp.c_str(), (dataDir + "/current").c_str()) != 0) return why = "the update can't be chosen", false;
  unlink((dataDir + "/crashes").c_str());  // a fresh count for the new one
  if (DIR* d = opendir(dataDir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.empty() || !isdigit((unsigned char)n[0]) || n == version || n == running) continue;
      struct stat s2;
      if (stat((dataDir + "/" + n + "/rondo").c_str(), &s2) == 0) removeTree(dataDir + "/" + n);
    }
    closedir(d);
  }
  unlink(file.c_str());
  return true;
}

}  // namespace rondo::bundle
