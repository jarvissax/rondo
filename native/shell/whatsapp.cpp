// WaClient (whatsapp.h): the server's /wa/ routes, what came kept for the screens, and the card's copy.
#include "whatsapp.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <thread>

#include <yyjson.h>

#include "app.h"
#include "main_queue.h"
#include "screens.h"

namespace rondo::shell {
namespace {

constexpr const char* kCardDir = "/storage/sdcard0/.rondo/whatsapp";
constexpr int64_t kCardMediaBytes = 1024LL * 1024 * 1024;  // the card's copy of photos, videos, voice: 1 GB
constexpr int kPage = 40;      // messages a chat loads at a time
constexpr int kKeepOnCard = 80;  // a chat's newest messages kept on the card
constexpr int kPollWait = 12;  // s the server holds a poll open (under the API's 15 s without data)

std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += char(c);
        }
    }
  }
  return o + "\"";
}

std::string sget(yyjson_val* o, const char* k) {
  const char* s = yyjson_get_str(yyjson_obj_get(o, k));
  return s ? s : "";
}
int64_t iget(yyjson_val* o, const char* k) {
  yyjson_val* v = yyjson_obj_get(o, k);
  return yyjson_is_num(v) ? int64_t(yyjson_get_num(v)) : 0;
}
bool bget(yyjson_val* o, const char* k) { return yyjson_get_bool(yyjson_obj_get(o, k)); }
// a message's status as the server numbers it (whatsappReduce.ts): yours 1 waiting, 2 sent, 3 delivered,
// 4 read, 5 played; theirs 0
std::string statusOf(yyjson_val* o, const char* k) {
  static const char* kWords[] = {"", "pending", "sent", "delivered", "read", "played"};
  yyjson_val* v = yyjson_obj_get(o, k);
  if (yyjson_is_str(v)) return yyjson_get_str(v);
  int64_t n = yyjson_is_num(v) ? int64_t(yyjson_get_num(v)) : 0;
  return n >= 0 && n <= 5 ? kWords[n] : "";
}
std::string rawOf(yyjson_val* v) {
  size_t len = 0;
  char* t = yyjson_val_write(v, 0, &len);
  std::string s = t ? std::string(t, len) : "";
  free(t);
  return s;
}

WaChat parseChat(yyjson_val* c) {
  WaChat ch;
  ch.jid = sget(c, "jid");
  ch.name = sget(c, "name");
  ch.group = bget(c, "group");
  ch.pinned = bget(c, "pinned");
  ch.archived = bget(c, "archived");
  ch.at = iget(c, "at");
  ch.unread = int(iget(c, "unread"));
  yyjson_val* p = yyjson_obj_get(c, "preview");
  if (yyjson_is_obj(p)) {
    ch.hasPreview = true;
    ch.preview.text = sget(p, "text");
    ch.preview.sender = sget(p, "sender");
    ch.preview.status = statusOf(p, "status");
    ch.preview.fromMe = bget(p, "fromMe");
    ch.preview.kind = sget(p, "kind");
    ch.preview.caption = sget(p, "caption");
    ch.preview.seconds = int(iget(p, "seconds"));
  }
  ch.raw = rawOf(c);
  return ch;
}

WaMessage parseMessage(yyjson_val* m) {
  WaMessage w;
  w.id = sget(m, "id");
  w.fromMe = bget(m, "fromMe");
  w.sender = sget(m, "sender");
  w.name = sget(m, "name");
  w.at = iget(m, "at");
  w.kind = sget(m, "kind");
  w.text = sget(m, "text");
  w.status = statusOf(m, "status");
  w.mine = sget(m, "mine");
  w.edited = bget(m, "edited");
  w.preview = bget(m, "preview");
  w.seq = iget(m, "seq");
  yyjson_val* md = yyjson_obj_get(m, "media");
  if (yyjson_is_obj(md)) {
    w.mime = sget(md, "mime");
    w.seconds = int(iget(md, "seconds"));
    w.width = int(iget(md, "width"));
    w.height = int(iget(md, "height"));
    w.size = iget(md, "size");
    w.fileName = sget(md, "fileName");
    w.gif = bget(md, "gif");
  }
  yyjson_val* q = yyjson_obj_get(m, "quoted");
  if (yyjson_is_obj(q)) {
    w.quoted = true;
    w.quote.id = sget(q, "id");
    w.quote.sender = sget(q, "sender");
    w.quote.name = sget(q, "name");
    w.quote.kind = sget(q, "kind");
    w.quote.text = sget(q, "text");
  }
  yyjson_val* rs = yyjson_obj_get(m, "reactions");
  size_t i, n;
  yyjson_val* r;
  yyjson_arr_foreach(rs, i, n, r) w.reactions.push_back({sget(r, "emoji"), int(iget(r, "count"))});
  w.raw = rawOf(m);
  return w;
}

// a JID as a file name ("12345@s.whatsapp.net" has no character a FAT card refuses)
std::string safe(const std::string& jid) {
  std::string s;
  for (char c : jid) s += (isalnum((unsigned char)c) || c == '@' || c == '.' || c == '-' || c == '_') ? c : '_';
  return s;
}

bool cardThere() { return g_library.presentRoots() & 1; }

void mkdirs(const std::string& path) {
  for (size_t at = 1; at <= path.size(); at++)
    if (at == path.size() || path[at] == '/') mkdir(path.substr(0, at).c_str(), 0777);
}

std::string readAll(const std::string& path) {
  std::string s;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char b[16384];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
  }
  return s;
}

bool writeAll(const std::string& path, const std::string& text) {
  std::string tmp = path + ".part";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0;
  fclose(f);
  return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

// the card's copy of media over 1 GB: the files used longest ago go (a thread's work)
void trimMedia() {
  struct F {
    std::string path;
    int64_t size;
    time_t used;
  };
  std::vector<F> all;
  int64_t total = 0;
  std::string root = std::string(kCardDir) + "/media";
  DIR* d = opendir(root.c_str());
  if (!d) return;
  std::vector<std::string> dirs;
  while (dirent* e = readdir(d))
    if (e->d_name[0] != '.') dirs.push_back(root + "/" + e->d_name);
  closedir(d);
  for (const std::string& dir : dirs) {
    DIR* s = opendir(dir.c_str());
    if (!s) continue;
    while (dirent* e = readdir(s)) {
      if (e->d_name[0] == '.') continue;
      std::string p = dir + "/" + e->d_name;
      struct stat st;
      if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        all.push_back({p, int64_t(st.st_size), st.st_mtime});
        total += st.st_size;
      }
    }
    closedir(s);
  }
  if (total <= kCardMediaBytes) return;
  std::sort(all.begin(), all.end(), [](const F& a, const F& b) { return a.used < b.used; });
  for (const F& f : all) {
    if (total <= kCardMediaBytes * 9 / 10) break;  // a little room, so it isn't trimmed at every file
    if (unlink(f.path.c_str()) == 0) total -= f.size;
  }
}

std::string extFor(const WaMessage& m) {
  if (m.kind == "image") return ".jpg";
  if (m.kind == "sticker") return ".png";
  if (m.kind == "video") return ".mp4";
  if (m.kind == "voice") return ".ogg";
  if (m.mime.find("mpeg") != std::string::npos) return ".mp3";
  if (m.mime.find("mp4") != std::string::npos || m.mime.find("aac") != std::string::npos) return ".m4a";
  if (m.mime.find("ogg") != std::string::npos) return ".ogg";
  size_t dot = m.fileName.rfind('.');
  return dot != std::string::npos && m.fileName.size() - dot <= 6 ? m.fileName.substr(dot) : ".bin";
}

std::string mediaFile(const std::string& jid, const WaMessage& m, const std::string& ext = "") {
  return std::string(kCardDir) + "/media/" + safe(jid) + "/" + safe(m.id) + (ext.empty() ? extFor(m) : ext);
}

std::string jidPath(const std::string& jid) { return Api::escape(jid); }

std::string sourceKey() { return serverConnected() ? serverUrl() + "\n" + serverToken() : "none"; }
std::string g_source;

}  // namespace

WaClient& waClient() {
  static WaClient c;
  return c;
}

// ---- the screens holding it ----

void WaClient::hold() {
  if (sourceKey() != g_source) {
    bool first = g_source.empty();
    g_source = sourceKey();
    if (!first) serverChanged();
  }
  if (holds_++ > 0) return;
  if (!serverConnected()) {  // WhatsApp is the Rondo server's (YouTube may come directly or not)
    state_ = State::NoServer;
    return changed();
  }
  if (!cardLoaded_) loadCard();
  // back after a while: what came meanwhile is the chats' list again (not every message since), and
  // the chats opened before load anew when opened
  for (auto& t : threads_)
    if (!t.second.open) t.second.loaded = false;
  loadStatus();
  loadChats();
}

void WaClient::release() {
  if (holds_ <= 0 || --holds_ > 0) return;
  if (pollId_) g_core.cancel(pollId_);
  pollId_ = 0;
  polling_ = false;
  saveNow();
}

void WaClient::serverChanged() {
  generation_++;
  if (pollId_) g_core.cancel(pollId_);
  pollId_ = 0;
  polling_ = false;
  chats_.clear();
  threads_.clear();
  contacts_.clear();
  contactsLoaded_ = chatsLoaded_ = false;
  seq_ = 0;
  state_ = State::Unknown;
  problem_.clear();
  // the card's copy was of the other server's WhatsApp
  if (cardThere()) {
    std::string dir = kCardDir;
    unlink((dir + "/chats.json").c_str());
    if (DIR* d = opendir((dir + "/chats").c_str())) {
      while (dirent* e = readdir(d))
        if (e->d_name[0] != '.') unlink((dir + "/chats/" + e->d_name).c_str());
      closedir(d);
    }
  }
  changed();
}

void WaClient::loadStatus() {
  uint32_t gen = generation_;
  g_core.get("/wa/status", 0, [this, gen](int status, const std::string& body) {
    if (gen != generation_) return;
    if (status == 404) state_ = State::Old;
    else if (status == 0) state_ = state_ == State::Unknown ? State::Offline : state_;
    else if (status == 401) problem_ = tr("whatsapp.serverRefused");
    if (status == 200) {
      yyjson_doc* d = yyjson_read(body.data(), body.size(), 0);
      std::string s = d ? sget(yyjson_doc_get_root(d), "state") : "";
      if (d) yyjson_doc_free(d);
      state_ = s == "open" ? State::Open : s == "linking" ? State::Linking : s == "unlinked" ? State::Unlinked : State::Connecting;
      problem_.clear();
    }
    changed();
  });
}

void WaClient::loadChats() {
  uint32_t gen = generation_;
  g_core.get("/wa/chats", 0, [this, gen](int status, const std::string& body) {
    if (gen != generation_) return;
    if (status != 200) {
      if (status == 404) state_ = State::Old;
      else if (status == 0 && state_ == State::Unknown) state_ = State::Offline;
      if (status != 404 && !chatsLoaded_) problem_ = status ? tr("whatsapp.chatsFailed", {{"status", status}}) : "";
      changed();
      if (holds_ > 0 && status != 404) postToMain([this, gen] { if (gen == generation_ && holds_ > 0) loadChats(); }, 5000);
      return;
    }
    yyjson_doc* d = yyjson_read(body.data(), body.size(), 0);
    if (!d) return;
    yyjson_val* root = yyjson_doc_get_root(d);
    seq_ = iget(root, "seq");
    std::vector<WaChat> list;
    size_t i, n;
    yyjson_val* c;
    yyjson_arr_foreach(yyjson_obj_get(root, "chats"), i, n, c) list.push_back(parseChat(c));
    yyjson_doc_free(d);
    // a chat open here keeps reading as read
    for (WaChat& ch : list)
      if (threads_.count(ch.jid) && threads_[ch.jid].open && ch.unread) markRead(ch.jid), ch.unread = 0;
    chats_ = std::move(list);
    chatsLoaded_ = true;
    problem_.clear();
    changed();
    saveSoon();
    poll();
  });
}

// ---- following the server ----

void WaClient::poll() {
  if (polling_ || holds_ <= 0 || state_ == State::NoServer || state_ == State::Old) return;
  polling_ = true;
  uint32_t gen = generation_;
  pollId_ = g_core.get("/wa/updates?since=" + std::to_string(seq_) + "&wait=" + std::to_string(kPollWait), 0,
                       [this, gen](int status, const std::string& body) {
    if (gen != generation_) return;
    polling_ = false;
    pollId_ = 0;
    if (status == 200) {
      applyUpdates(body);
      return poll();
    }
    if (status == 404) {
      state_ = State::Old;
      return changed();
    }
    // the network or the server away a moment: asked again in a few seconds
    postToMain([this, gen] { if (gen == generation_) poll(); }, 3000);
  });
}

void WaClient::applyUpdates(const std::string& body) {
  yyjson_doc* d = yyjson_read(body.data(), body.size(), 0);
  if (!d) return;
  yyjson_val* root = yyjson_doc_get_root(d);
  int64_t seq = iget(root, "seq");
  std::string st = sget(root, "status");
  State was = state_;
  if (!st.empty())
    state_ = st == "open" ? State::Open : st == "linking" ? State::Linking : st == "unlinked" ? State::Unlinked : State::Connecting;
  bool any = state_ != was;
  size_t i, n;
  yyjson_val* v;
  std::vector<std::string> readAgain;
  yyjson_arr_foreach(yyjson_obj_get(root, "chats"), i, n, v) {
    WaChat ch = parseChat(v);
    auto t = threads_.find(ch.jid);
    if (t != threads_.end() && t->second.open && ch.unread) readAgain.push_back(ch.jid), ch.unread = 0;
    auto at = std::find_if(chats_.begin(), chats_.end(), [&](const WaChat& c) { return c.jid == ch.jid; });
    if (at != chats_.end()) *at = std::move(ch);
    else if (ch.at > 0 || ch.unread > 0) chats_.push_back(std::move(ch));
    any = true;
  }
  yyjson_arr_foreach(yyjson_obj_get(root, "messages"), i, n, v) {
    std::string jid = sget(v, "chat");
    auto t = threads_.find(jid);
    if (t == threads_.end() || !t->second.loaded) continue;
    upsert(jid, parseMessage(v));
    any = true;
  }
  yyjson_doc_free(d);
  if (seq > seq_) seq_ = seq;
  for (const std::string& jid : readAgain) markRead(jid);
  if (any) {
    sortChats();
    changed();
    saveSoon();
  }
}

void WaClient::sortChats() {
  std::stable_sort(chats_.begin(), chats_.end(), [](const WaChat& a, const WaChat& b) {
    if (a.pinned != b.pinned) return a.pinned > b.pinned;
    return a.at > b.at;
  });
}

// ---- a chat ----

const WaChat* WaClient::chat(const std::string& jid) const {
  for (const WaChat& c : chats_)
    if (c.jid == jid) return &c;
  return nullptr;
}

const std::vector<WaMessage>* WaClient::messages(const std::string& jid) const {
  auto t = threads_.find(jid);
  return t != threads_.end() && (t->second.loaded || !t->second.msgs.empty()) ? &t->second.msgs : nullptr;
}

bool WaClient::olderToCome(const std::string& jid) const {
  auto t = threads_.find(jid);
  return t != threads_.end() && t->second.loaded && !t->second.complete;
}

bool WaClient::loadingOlder(const std::string& jid) const {
  auto t = threads_.find(jid);
  return t != threads_.end() && t->second.loadingOlder;
}

void WaClient::open(const std::string& jid) {
  Thread& t = threads_[jid];
  t.open++;
  if (t.msgs.empty() && cardThere()) {  // the card's copy at once
    std::string text = readAll(std::string(kCardDir) + "/chats/" + safe(jid) + ".json");
    if (yyjson_doc* d = text.empty() ? nullptr : yyjson_read(text.data(), text.size(), 0)) {
      size_t i, n;
      yyjson_val* v;
      yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(d), "messages"), i, n, v) t.msgs.push_back(parseMessage(v));
      yyjson_doc_free(d);
      changed();
    }
  }
  for (WaChat& c : chats_)
    if (c.jid == jid && c.unread) {
      c.unread = 0;
      markRead(jid);
      changed();
    }
  if (t.loaded) return;
  uint32_t gen = generation_;
  g_core.get("/wa/chats/" + jidPath(jid) + "/messages?limit=" + std::to_string(kPage), 0,
             [this, gen, jid](int status, const std::string& body) {
    if (gen != generation_ || status != 200) return;
    yyjson_doc* d = yyjson_read(body.data(), body.size(), 0);
    if (!d) return;
    std::vector<WaMessage> msgs;
    size_t i, n;
    yyjson_val* v;
    yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(d), "messages"), i, n, v) msgs.push_back(parseMessage(v));
    yyjson_val* c = yyjson_obj_get(yyjson_doc_get_root(d), "chat");
    if (!chat(jid) && yyjson_is_obj(c)) {  // a chat started here (from the contacts): its name
      WaChat ch = parseChat(c);
      ch.at = 0;
      chats_.push_back(std::move(ch));
    }
    yyjson_doc_free(d);
    Thread& t = threads_[jid];
    t.complete = int(msgs.size()) < kPage;
    putMessages(jid, std::move(msgs), true);
    t.loaded = true;
    changed();
    saveSoon();
  });
}

void WaClient::close(const std::string& jid) {
  auto t = threads_.find(jid);
  if (t != threads_.end() && t->second.open > 0) t->second.open--;
}

void WaClient::older(const std::string& jid) {
  auto it = threads_.find(jid);
  if (it == threads_.end() || !it->second.loaded || it->second.complete || it->second.loadingOlder || it->second.msgs.empty()) return;
  it->second.loadingOlder = true;
  changed();
  int64_t before = it->second.msgs.front().at;
  uint32_t gen = generation_;
  g_core.get("/wa/chats/" + jidPath(jid) + "/messages?limit=" + std::to_string(kPage) + "&before=" + std::to_string(before), 0,
             [this, gen, jid](int status, const std::string& body) {
    if (gen != generation_) return;
    Thread& t = threads_[jid];
    t.loadingOlder = false;
    if (status == 200) {
      if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
        std::vector<WaMessage> msgs;
        size_t i, n;
        yyjson_val* v;
        yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(d), "messages"), i, n, v) msgs.push_back(parseMessage(v));
        yyjson_doc_free(d);
        t.complete = int(msgs.size()) < kPage;
        putMessages(jid, std::move(msgs), false);
      }
    }
    changed();
  });
}

// messages into a chat, kept in time order and once each; replaceAll: the server's page is the chat now
// (what's still being sent from here stays)
void WaClient::putMessages(const std::string& jid, std::vector<WaMessage> msgs, bool replaceAll) {
  Thread& t = threads_[jid];
  if (replaceAll) {
    std::vector<WaMessage> mine;
    for (WaMessage& m : t.msgs)
      if (m.sending || m.failed) mine.push_back(std::move(m));
    t.msgs.clear();
    for (WaMessage& m : mine) msgs.push_back(std::move(m));
  }
  for (WaMessage& m : msgs) upsert(jid, std::move(m));
}

void WaClient::upsert(const std::string& jid, WaMessage m) {
  std::vector<WaMessage>& v = threads_[jid].msgs;
  auto same = std::find_if(v.begin(), v.end(), [&](const WaMessage& x) { return x.id == m.id; });
  if (same != v.end()) {
    if (!same->localFile.empty() && m.localFile.empty()) m.localFile = same->localFile;
    *same = std::move(m);
    return;
  }
  auto at = std::upper_bound(v.begin(), v.end(), m.at, [](int64_t t, const WaMessage& x) { return t < x.at; });
  v.insert(at, std::move(m));
}

void WaClient::markRead(const std::string& jid) {
  g_core.post("/wa/chats/" + jidPath(jid) + "/read", "{}", [](int, const std::string&) {});
}

// ---- sending ----

void WaClient::sendText(const std::string& jid, const std::string& text, const std::string& quoted) {
  WaMessage m;
  m.id = "local-" + std::to_string(++localIds_);
  m.fromMe = true;
  m.kind = "text";
  m.text = text;
  m.at = int64_t(time(nullptr)) * 1000;
  m.sending = true;
  m.status = "pending";
  if (!quoted.empty()) {
    if (const auto* all = messages(jid))
      for (const WaMessage& q : *all)
        if (q.id == quoted) m.quoted = true, m.quote = {q.id, q.sender, q.fromMe ? tr("whatsapp.you") : q.name, q.kind, q.text};
  }
  upsert(jid, m);
  changed();
  std::string body = "{\"text\":" + jstr(text) + (quoted.empty() ? "" : ",\"quoted\":" + jstr(quoted)) + "}";
  uint32_t gen = generation_;
  std::string id = m.id;
  g_core.post("/wa/chats/" + jidPath(jid) + "/text", body, [this, gen, jid, id](int status, const std::string& reply) {
    if (gen == generation_) sent(jid, id, status, reply);
  });
}

void WaClient::sendVoice(const std::string& jid, const std::string& wav, int seconds) {
  WaMessage m;
  m.id = "local-" + std::to_string(++localIds_);
  m.fromMe = true;
  m.kind = "voice";
  m.seconds = seconds;
  m.at = int64_t(time(nullptr)) * 1000;
  m.localFile = wav;
  sendFile(jid, "/wa/chats/" + jidPath(jid) + "/voice", wav, "audio/wav", m);
}

void WaClient::sendMedia(const std::string& jid, const std::string& file, bool video) {
  WaMessage m;
  m.id = "local-" + std::to_string(++localIds_);
  m.fromMe = true;
  m.kind = video ? "video" : "image";
  m.at = int64_t(time(nullptr)) * 1000;
  m.localFile = file;
  std::string lower = file;
  for (char& c : lower) c = char(tolower((unsigned char)c));
  auto ends = [&](const char* x) { size_t n = strlen(x); return lower.size() >= n && lower.compare(lower.size() - n, n, x) == 0; };
  std::string type = video ? (ends(".mp4") || ends(".m4v") ? "video/mp4" : "application/octet-stream")
                           : (ends(".png") ? "image/png" : ends(".jpg") || ends(".jpeg") ? "image/jpeg" : "application/octet-stream");
  sendFile(jid, "/wa/chats/" + jidPath(jid) + "/media?kind=" + m.kind, file, type, m);
}

void WaClient::sendFile(const std::string& jid, const std::string& path, const std::string& file, const std::string& type, WaMessage m) {
  m.sending = true;
  m.failed = false;
  m.status = "pending";
  upsert(jid, m);
  changed();
  uint32_t gen = generation_;
  std::string id = m.id;
  g_core.upload(path, file, type, [this, gen, jid, id](int status, const std::string& reply) {
    if (gen == generation_) sent(jid, id, status, reply);
  });
}

void WaClient::retry(const std::string& jid, const std::string& id) {
  const std::vector<WaMessage>* all = messages(jid);
  if (!all) return;
  for (const WaMessage& m : *all) {
    if (m.id != id || !m.failed) continue;
    WaMessage copy = m;
    if (copy.kind == "text") {
      auto& v = threads_[jid].msgs;
      v.erase(std::remove_if(v.begin(), v.end(), [&](const WaMessage& x) { return x.id == id; }), v.end());
      return sendText(jid, copy.text, copy.quoted ? copy.quote.id : "");
    }
    if (copy.kind == "voice") return sendFile(jid, "/wa/chats/" + jidPath(jid) + "/voice", copy.localFile, "audio/wav", copy);
    return sendMedia(jid, copy.localFile, copy.kind == "video");
  }
}

void WaClient::react(const std::string& jid, const std::string& id, const std::string& emoji) {
  auto& v = threads_[jid].msgs;
  auto it = std::find_if(v.begin(), v.end(), [&](const WaMessage& x) { return x.id == id; });
  if (it == v.end()) return;
  WaMessage before = *it;
  auto add = [&](const std::string& e, int by) {
    for (auto r = it->reactions.begin(); r != it->reactions.end(); ++r)
      if (r->emoji == e) {
        r->count += by;
        if (r->count <= 0) it->reactions.erase(r);
        return;
      }
    if (by > 0) it->reactions.push_back({e, by});
  };
  if (!it->mine.empty()) add(it->mine, -1);
  if (!emoji.empty()) add(emoji, 1);
  it->mine = emoji;
  changed();
  uint32_t gen = generation_;
  std::string body = "{\"id\":" + jstr(id) + ",\"emoji\":" + jstr(emoji) + "}";
  g_core.post("/wa/chats/" + jidPath(jid) + "/react", body, [this, gen, jid, before](int status, const std::string& reply) {
    if (gen != generation_) return;
    if (status == 200) {
      if (yyjson_doc* d = yyjson_read(reply.data(), reply.size(), 0)) {
        upsert(jid, parseMessage(yyjson_doc_get_root(d)));
        yyjson_doc_free(d);
      }
      return changed();
    }
    upsert(jid, before);  // as it was
    changed();
    g_app->toast(status == 404 ? tr("whatsapp.react.needsServer")
                 : status == 0 ? tr("whatsapp.react.noConnection")
                               : tr("whatsapp.react.failed", {{"status", status}}));
  });
}

// the server's answer to something sent: the message as WhatsApp has it replaces ours
void WaClient::sent(const std::string& jid, const std::string& localId, int status, const std::string& body) {
  auto& v = threads_[jid].msgs;
  auto local = std::find_if(v.begin(), v.end(), [&](const WaMessage& x) { return x.id == localId; });
  yyjson_doc* d = status == 200 ? yyjson_read(body.data(), body.size(), 0) : nullptr;
  if (!d) {
    if (local != v.end()) local->sending = false, local->failed = true;
    std::string why;
    if (status && body.size() > 2) {
      if (yyjson_doc* e = yyjson_read(body.data(), body.size(), 0)) {
        why = sget(yyjson_doc_get_root(e), "error");
        yyjson_doc_free(e);
      }
    }
    // the server's own reason (its words, in English) when it gave one
    g_app->toast(why.empty() ? (status ? tr("whatsapp.notSentHttp", {{"status", status}}) : tr("whatsapp.notSentNoConnection")) : why);
    return changed();
  }
  WaMessage m = parseMessage(yyjson_doc_get_root(d));
  yyjson_doc_free(d);
  std::string file;
  if (local != v.end()) {
    file = local->localFile;
    v.erase(local);
  }
  // a voice message recorded here plays from its own recording (WAV) without asking the server
  if (m.kind == "voice" && !file.empty() && cardThere()) {
    std::string kept = mediaFile(jid, m, ".wav");
    mkdirs(kept.substr(0, kept.rfind('/')));
    if (rename(file.c_str(), kept.c_str()) == 0) file = kept;
  }
  m.localFile = file;
  std::string line = summary(m), kind = m.kind, caption = m.kind == "image" || m.kind == "video" ? m.text : "";
  int seconds = m.seconds;
  upsert(jid, std::move(m));
  // the chat's line in the list (the poll brings the server's soon)
  for (WaChat& c : chats_)
    if (c.jid == jid) {
      c.at = int64_t(time(nullptr)) * 1000;
      c.hasPreview = true;
      c.preview = {line, "", "sent", true};
      c.preview.kind = kind, c.preview.caption = caption, c.preview.seconds = seconds;
    }
  sortChats();
  changed();
  saveSoon();
}

// ---- contacts ----

void WaClient::loadContacts(std::function<void()> done) {
  if (contactsLoaded_) return done();
  contactsWaiting_.push_back(std::move(done));
  if (contactsLoading_) return;
  contactsLoading_ = true;
  uint32_t gen = generation_;
  g_core.get("/wa/contacts", 60000, [this, gen](int status, const std::string& body) {
    if (gen != generation_) return;
    contactsLoading_ = false;
    if (status == 200) {
      if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
        contacts_.clear();
        size_t i, n;
        yyjson_val* v;
        yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(d), "contacts"), i, n, v) {
          WaContact c;
          c.jid = sget(v, "jid"), c.name = sget(v, "name"), c.number = sget(v, "number"), c.pushName = sget(v, "pushName");
          c.saved = bget(v, "saved");
          contacts_.push_back(std::move(c));
        }
        yyjson_doc_free(d);
        contactsLoaded_ = true;
      }
    }
    auto waiting = std::move(contactsWaiting_);
    contactsWaiting_.clear();
    for (auto& f : waiting) f();
    changed();
  });
}

void WaClient::profile(const std::string& jid, std::function<void(const WaContact&, bool)> done) {
  uint32_t gen = generation_;
  g_core.get("/wa/contacts/" + jidPath(jid), 300000, [gen, done, this](int status, const std::string& body) {
    if (gen != generation_) return;
    WaContact c;
    yyjson_doc* d = status == 200 ? yyjson_read(body.data(), body.size(), 0) : nullptr;
    if (d) {
      yyjson_val* v = yyjson_doc_get_root(d);
      c.jid = sget(v, "jid"), c.name = sget(v, "name"), c.number = sget(v, "number"), c.pushName = sget(v, "pushName");
      c.saved = bget(v, "saved"), c.group = bget(v, "group");
      c.hasAbout = yyjson_obj_get(v, "about") != nullptr;
      c.about = sget(v, "about");
      yyjson_doc_free(d);
    }
    done(c, d != nullptr);
  });
}

// ---- media ----

std::string WaClient::mediaFileIfKept(const std::string& jid, const WaMessage& m) const {
  if (!m.localFile.empty() && access(m.localFile.c_str(), R_OK) == 0) return m.localFile;
  if (!cardThere()) return "";
  std::string f = mediaFile(jid, m);
  if (access(f.c_str(), R_OK) == 0) return f;
  if (m.kind == "voice" && m.fromMe) {
    std::string wav = mediaFile(jid, m, ".wav");
    if (access(wav.c_str(), R_OK) == 0) return wav;
  }
  return "";
}

bool WaClient::fetching(const std::string& jid, const std::string& id) const { return fetching_.count(jid + "\n" + id) > 0; }

void WaClient::media(const std::string& jid, const WaMessage& m,
                     std::function<void(const std::string& file, const std::string& problem)> done) {
  std::string kept = mediaFileIfKept(jid, m);
  if (!kept.empty()) {
    utime(kept.c_str(), nullptr);  // used now: the last to go
    return done(kept, "");
  }
  if (!cardThere()) return done("", tr("whatsapp.media.noCard"));
  std::string key = jid + "\n" + m.id;
  auto& waiting = fetching_[key];
  waiting.push_back(std::move(done));
  if (waiting.size() > 1) return;
  changed();
  std::string file = mediaFile(jid, m);
  mkdirs(file.substr(0, file.rfind('/')));
  // a photo at the Y1's size or a little more (the viewer zooms); the rest as it is
  std::string path = "/wa/media/" + jidPath(jid) + "/" + m.id + (m.kind == "image" ? "?w=1280" : "");
  uint32_t gen = generation_;
  g_core.save(path, file, [this, gen, key, file](int status, const std::string&) {
    if (gen != generation_) return;
    auto waiting = std::move(fetching_[key]);
    fetching_.erase(key);
    std::string problem = status == 200 ? ""
                          : status == 410 ? tr("whatsapp.media.gone")
                          : status == 0   ? tr("whatsapp.media.noConnection")
                                          : tr("whatsapp.media.failed", {{"status", status}});
    for (auto& f : waiting) f(status == 200 ? file : "", problem);
    changed();
    if (status == 200) std::thread(trimMedia).detach();
  });
}

std::string WaClient::avatarPath(const std::string& jid) { return "/wa/avatar/" + jidPath(jid) + "?v=1"; }

std::string WaClient::previewPath(const std::string& jid, const WaMessage& m) {
  std::string base = "/wa/media/" + jidPath(jid) + "/" + m.id;
  if (m.kind == "image" || m.kind == "sticker") return base + "?r=1";
  return m.preview ? base + "?preview=1" : "";
}

std::string WaClient::summary(const WaMessage& m) {
  // a photo's or a video's words after what it is ("Photo: …")
  auto captioned = [&](const std::string& what) { return m.text.empty() ? what : tr("whatsapp.summary.captioned", {{"what", what}, {"text", m.text}}); };
  if (m.kind == "text") return m.text;
  if (m.kind == "image") return captioned(tr("whatsapp.photo"));
  if (m.kind == "video")
    return captioned(m.gif ? std::string("GIF") : m.seconds > 0 ? tr("whatsapp.videoLength", {{"length", formatTime(m.seconds)}}) : tr("whatsapp.video"));
  if (m.kind == "voice") return m.seconds > 0 ? tr("whatsapp.voiceMessageLength", {{"length", formatTime(m.seconds)}}) : tr("whatsapp.voiceMessage");
  if (m.kind == "audio") return m.seconds > 0 ? tr("whatsapp.audioLength", {{"length", formatTime(m.seconds)}}) : tr("whatsapp.audio");
  if (m.kind == "document") return tr("whatsapp.summary.document", {{"name", m.fileName.empty() ? m.text : m.fileName}});
  if (m.kind == "sticker") return tr("whatsapp.sticker");
  if (m.kind == "deleted") return tr("whatsapp.deleted");
  if (m.kind == "location") return tr("whatsapp.summary.location");
  if (m.kind == "contact") return tr("whatsapp.summary.contact", {{"name", m.text}});
  if (m.kind == "poll") return tr("whatsapp.summary.poll", {{"question", m.text}});
  return m.text.empty() ? tr("whatsapp.summary.unsupported") : m.text;
}

// ---- the card's copy ----

void WaClient::loadCard() {
  cardLoaded_ = true;
  if (!cardThere() || chatsLoaded_) return;
  std::string text = readAll(std::string(kCardDir) + "/chats.json");
  yyjson_doc* d = text.empty() ? nullptr : yyjson_read(text.data(), text.size(), 0);
  if (!d) return;
  yyjson_val* root = yyjson_doc_get_root(d);
  // whose it was: another server's copy isn't shown
  if (sget(root, "source") == std::to_string(std::hash<std::string>()(g_source))) {
    size_t i, n;
    yyjson_val* v;
    yyjson_arr_foreach(yyjson_obj_get(root, "chats"), i, n, v) chats_.push_back(parseChat(v));
    chatsLoaded_ = !chats_.empty();
  }
  yyjson_doc_free(d);
  changed();
}

void WaClient::saveSoon() {
  if (saveQueued_) return;
  saveQueued_ = true;
  postToMain([this] { saveNow(); }, 10000);
}

void WaClient::saveNow() {
  saveQueued_ = false;
  if (!cardThere() || !chatsLoaded_ || state_ == State::NoServer) return;
  std::string dir = kCardDir;
  mkdirs(dir + "/chats");
  std::string text = "{\"source\":" + jstr(std::to_string(std::hash<std::string>()(g_source))) + ",\"chats\":[";
  bool first = true;
  for (const WaChat& c : chats_) {
    if (c.raw.empty()) continue;
    text += (first ? "" : ",") + c.raw;
    first = false;
  }
  writeAll(dir + "/chats.json", text + "]}");
  for (const auto& t : threads_) {
    if (!t.second.loaded) continue;
    std::string m = "{\"messages\":[";
    const auto& v = t.second.msgs;
    size_t from = v.size() > size_t(kKeepOnCard) ? v.size() - kKeepOnCard : 0;
    bool f = true;
    for (size_t i = from; i < v.size(); i++) {
      if (v[i].raw.empty()) continue;
      m += (f ? "" : ",") + v[i].raw;
      f = false;
    }
    writeAll(dir + "/chats/" + safe(t.first) + ".json", m + "]}");
  }
}

}  // namespace rondo::shell
