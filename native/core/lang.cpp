// The languages; see lang.h.
#include "lang.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <yyjson.h>

namespace rondo {
namespace lang {
namespace {

// CLDR's plural categories, in the order a value's forms are kept
enum Form { kZero, kOne, kTwo, kFew, kMany, kOther, kForms };
const char* const kFormNames[kForms] = {"zero", "one", "two", "few", "many", "other"};

struct Entry {
  bool plural = false;
  std::string text;            // a plain text
  std::string forms[kForms];   // a plural's forms ("" where the file has none)
  bool has[kForms] = {};
};

struct Catalog {
  std::string code, name;
  std::deque<std::string> keys;  // the map's keys point into these
  std::unordered_map<std::string_view, Entry> entries;
};

std::mutex g_lock;  // loading
std::string g_dir, g_section;
std::vector<std::unique_ptr<Catalog>> g_loaded;   // never freed while Rondo runs (lang.h)
std::vector<std::unique_ptr<Catalog>> g_retired;  // another folder's (init() again: the tests), kept the same way
std::atomic<const Catalog*> g_current{nullptr}, g_english{nullptr};

std::string slurp(const std::string& path) {
  std::string out;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return out;
  char b[16384];
  size_t n;
  while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n);
  fclose(f);
  return out;
}

bool cleanCode(const std::string& c) {
  if (c.empty() || c.size() > 16) return false;
  for (char ch : c)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
  return true;
}

// <dir>/<code>.json's section, or null
std::unique_ptr<Catalog> read(const std::string& dir, const std::string& section, const std::string& code) {
  if (!cleanCode(code)) return nullptr;
  std::string body = slurp(dir + "/" + code + ".json");
  if (body.empty()) return nullptr;
  yyjson_doc* doc = yyjson_read(body.data(), body.size(), 0);
  if (!doc) {
    fprintf(stderr, "rondo: %s/%s.json isn't valid JSON\n", dir.c_str(), code.c_str());
    return nullptr;
  }
  auto c = std::make_unique<Catalog>();
  c->code = code;
  yyjson_val* root = yyjson_doc_get_root(doc);
  if (yyjson_val* n = yyjson_obj_get(root, "_name"); yyjson_is_str(n)) c->name = yyjson_get_str(n);
  if (c->name.empty()) c->name = code;
  yyjson_val* sec = yyjson_obj_get(root, section.c_str());
  yyjson_obj_iter it;
  if (yyjson_obj_iter_init(sec, &it)) {
    yyjson_val* k;
    while ((k = yyjson_obj_iter_next(&it))) {
      yyjson_val* v = yyjson_obj_iter_get_val(k);
      Entry e;
      if (yyjson_is_str(v)) {
        e.text = yyjson_get_str(v);
      } else if (yyjson_is_obj(v)) {
        e.plural = true;
        for (int f = 0; f < kForms; f++)
          if (yyjson_val* s = yyjson_obj_get(v, kFormNames[f]); yyjson_is_str(s)) e.forms[f] = yyjson_get_str(s), e.has[f] = true;
        if (!e.has[kOther]) continue;  // the checker refuses it; here it isn't used
      } else {
        continue;
      }
      c->keys.emplace_back(yyjson_get_str(k), yyjson_get_len(k));
      c->entries[c->keys.back()] = std::move(e);
    }
  }
  yyjson_doc_free(doc);
  return c;
}

// the catalog for code, loaded once (caller holds g_lock)
const Catalog* loaded(const std::string& code) {
  for (auto& c : g_loaded)
    if (c->code == code) return c.get();
  auto c = read(g_dir, g_section, code);
  if (!c) return nullptr;
  g_loaded.push_back(std::move(c));
  return g_loaded.back().get();
}

int formIndex(const char* name) {
  for (int f = 0; f < kForms; f++)
    if (!strcmp(kFormNames[f], name)) return f;
  return kOther;
}

void appendNumber(std::string& out, const Arg& a) { out += a.text; }

const Arg* findArg(Args args, std::string_view name) {
  for (const Blank& b : args)
    if (name == b.name) return &b.value;
  return nullptr;
}

// the text of an entry for these args: a plural's form by "count" in that language, else its "other"
std::string_view pick(const Entry& e, const std::string& code, Args args) {
  if (!e.plural) return e.text;
  const Arg* count = findArg(args, "count");
  int f = count && count->isNumber ? formIndex(pluralForm(code, count->number)) : kOther;
  // English and Spanish say "0 songs": an explicit zero form only where a file has one
  if (count && count->isNumber && count->number == 0 && e.has[kZero]) f = kZero;
  return e.has[f] ? e.forms[f] : e.forms[kOther];
}

std::string translate(std::string_view key, Args args, bool format) {
  const Catalog* cur = g_current.load(std::memory_order_acquire);
  const Catalog* en = g_english.load(std::memory_order_acquire);
  for (const Catalog* c : {cur, en}) {
    if (!c) continue;
    auto it = c->entries.find(key);
    if (it == c->entries.end()) continue;
    std::string_view t = pick(it->second, c->code, args);
    return format ? fill(t, args) : std::string(t);
  }
  return std::string(key);
}

void jsonString(std::string& out, std::string_view s) {
  out += '"';
  for (unsigned char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", ch);
          out += b;
        } else {
          out += char(ch);
        }
    }
  }
  out += '"';
}

void jsonEntry(std::string& out, const Entry& e) {
  if (!e.plural) return jsonString(out, e.text);
  out += '{';
  bool first = true;
  for (int f = 0; f < kForms; f++) {
    if (!e.has[f]) continue;
    if (!first) out += ',';
    first = false;
    jsonString(out, kFormNames[f]);
    out += ':';
    jsonString(out, e.forms[f]);
  }
  out += '}';
}

}  // namespace

bool init(const std::string& dir, const std::string& section, const std::string& code) {
  {
    std::lock_guard<std::mutex> l(g_lock);
    if (dir != g_dir || section != g_section)
      for (auto& c : g_loaded) g_retired.push_back(std::move(c));
    if (dir != g_dir || section != g_section) g_loaded.clear();
    g_dir = dir, g_section = section;
    const Catalog* en = loaded("en");
    if (!en) fprintf(stderr, "rondo: no %s/en.json: texts show as their keys\n", dir.c_str());
    g_english.store(en, std::memory_order_release);
    g_current.store(en, std::memory_order_release);
  }
  return code.empty() || code == "en" ? g_english.load() != nullptr : use(code);
}

bool use(const std::string& code) {
  std::lock_guard<std::mutex> l(g_lock);
  const Catalog* c = loaded(code);
  if (!c) return false;
  g_current.store(c, std::memory_order_release);
  return true;
}

std::string current() {
  const Catalog* c = g_current.load(std::memory_order_acquire);
  return c ? c->code : "en";
}

const std::string& folder() { return g_dir; }

std::vector<Language> available() {
  std::vector<Language> out;
  std::string dir;
  {
    std::lock_guard<std::mutex> l(g_lock);
    dir = g_dir;
  }
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() < 6 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
      std::string code = n.substr(0, n.size() - 5);
      if (!cleanCode(code)) continue;
      std::string name = nameOf(code);
      if (!name.empty()) out.push_back({code, name});
    }
    closedir(d);
  }
  std::sort(out.begin(), out.end(), [](const Language& a, const Language& b) {
    if ((a.code == "en") != (b.code == "en")) return a.code == "en";
    return a.name < b.name;
  });
  return out;
}

std::string nameOf(const std::string& code) {
  {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto& c : g_loaded)
      if (c->code == code) return c->name;
  }
  if (!cleanCode(code)) return "";
  // only the name: a file not loaded yet isn't parsed whole for it
  std::string body = slurp(g_dir + "/" + code + ".json");
  if (body.empty()) return "";
  yyjson_doc* doc = yyjson_read(body.data(), body.size(), 0);
  if (!doc) return "";
  std::string name;
  if (yyjson_val* n = yyjson_obj_get(yyjson_doc_get_root(doc), "_name"); yyjson_is_str(n)) name = yyjson_get_str(n);
  yyjson_doc_free(doc);
  return name.empty() ? code : name;
}

const char* pluralForm(const std::string& code, long long n) {
  std::string l = code.substr(0, code.find_first_of("-_"));
  long long a = n < 0 ? -n : n, m10 = a % 10, m100 = a % 100;
  // no plural forms at all
  if (l == "ja" || l == "zh" || l == "ko" || l == "vi" || l == "th" || l == "id" || l == "ms") return "other";
  // 0 and 1 are singular
  if (l == "fr" || l == "hi" || l == "fa" || l == "bn") return a <= 1 ? "one" : "other";
  // the East Slavic languages
  if (l == "ru" || l == "uk" || l == "be") {
    if (m10 == 1 && m100 != 11) return "one";
    if (m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14)) return "few";
    return "many";
  }
  if (l == "pl") {
    if (a == 1) return "one";
    if (m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14)) return "few";
    return "many";
  }
  if (l == "cs" || l == "sk") return a == 1 ? "one" : a >= 2 && a <= 4 ? "few" : "other";
  // English, Spanish, German, Italian, Portuguese, Turkish, Azerbaijani, Dutch, the Nordic languages…
  return a == 1 ? "one" : "other";
}

std::string trIn(const std::string& code, std::string_view key, Args args) {
  const Catalog* c = nullptr;
  {
    std::lock_guard<std::mutex> l(g_lock);
    // a region without a file of its own speaks its language's (es-MX: es)
    c = loaded(code);
    if (!c && code.find_first_of("-_") != std::string::npos) c = loaded(code.substr(0, code.find_first_of("-_")));
  }
  const Catalog* en = g_english.load(std::memory_order_acquire);
  for (const Catalog* k : {c, en}) {
    if (!k) continue;
    auto it = k->entries.find(key);
    if (it != k->entries.end()) return fill(pick(it->second, k->code, args), args);
  }
  return std::string(key);
}

std::string fill(std::string_view text, Args args) {
  std::string out;
  out.reserve(text.size() + 16);
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '{') {
      size_t close = text.find('}', i + 1);
      if (close != std::string_view::npos) {
        std::string_view name = text.substr(i + 1, close - i - 1);
        if (const Arg* a = findArg(args, name)) {
          if (a->isNumber) appendNumber(out, *a);
          else out += a->text;
          i = close + 1;
          continue;
        }
      }
    }
    out += text[i++];
  }
  return out;
}

std::string pageStrings(const std::string& section, const std::string& code) {
  std::string dir;
  {
    std::lock_guard<std::mutex> l(g_lock);
    dir = g_dir;
  }
  auto en = read(dir, section, "en");
  auto other = code == "en" ? nullptr : read(dir, section, code);
  const Catalog* c = other ? other.get() : en.get();
  std::string out = "{\"code\":";
  jsonString(out, c ? c->code : "en");
  out += ",\"name\":";
  jsonString(out, c ? c->name : "English");
  out += ",\"languages\":[";
  bool first = true;
  for (const Language& l : available()) {
    if (!first) out += ',';
    first = false;
    out += "{\"code\":";
    jsonString(out, l.code);
    out += ",\"name\":";
    jsonString(out, l.name);
    out += '}';
  }
  out += "],\"strings\":{";
  first = true;
  if (en) {
    // English's keys in its order, each in the other language where it has it
    for (const std::string& k : en->keys) {
      const Entry* e = &en->entries[k];
      if (other) {
        auto it = other->entries.find(k);
        if (it != other->entries.end()) e = &it->second;
      }
      if (!first) out += ',';
      first = false;
      jsonString(out, k);
      out += ':';
      jsonEntry(out, *e);
    }
  }
  out += "}}";
  return out;
}

}  // namespace lang

std::string tr(std::string_view key) { return lang::translate(key, {}, false); }
std::string tr(std::string_view key, lang::Args args) { return lang::translate(key, args, true); }

}  // namespace rondo
