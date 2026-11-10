// json.h.
#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace rondo::direct {

std::string firstKey(J o) {
  if (!o || !yyjson_is_obj(o)) return "";
  size_t i, n;
  J k, v;
  yyjson_obj_foreach(o, i, n, k, v) return str(k);
  return "";
}

std::string text(J t) {
  if (!t) return "";
  if (yyjson_is_str(t)) return str(t);
  if (J s = key(t, "simpleText")) return str(s);
  if (J c = key(t, "content")) return str(c);
  std::string out;
  each(key(t, "runs"), [&](J r) { out += str(key(r, "text")); });
  return out;
}

Endpoint endpoint(J e) {
  Endpoint out;
  if (!e || !yyjson_is_obj(e)) return out;
  size_t i, n;
  J k, v;
  yyjson_obj_foreach(e, i, n, k, v) {
    std::string name = str(k);
    auto ends = [&](const char* s) { size_t l = strlen(s); return name.size() >= l && name.compare(name.size() - l, l, s) == 0; };
    if (ends("Endpoint") || ends("Command")) {
      out.name = name;
      out.payload = v;
      break;
    }
  }
  J p = out.payload;
  out.browseId = str(key(p, "browseId"));
  out.videoId = str(key(p, "videoId"));
  out.playlistId = str(key(p, "playlistId"));
  out.params = str(key(p, "params"));
  out.pageType = str(path(p, "browseEndpointContextSupportedConfigs", "browseEndpointContextMusicConfig", "pageType"));
  out.musicVideoType = str(path(p, "watchEndpointMusicSupportedConfigs", "watchEndpointMusicConfig", "musicVideoType"));
  return out;
}

std::vector<Run> runs(J t) {
  std::vector<Run> out;
  each(key(t, "runs"), [&](J r) { out.push_back({str(key(r, "text")), endpoint(key(r, "navigationEndpoint"))}); });
  return out;
}

Endpoint textEndpoint(J t) {
  if (J e = key(t, "navigationEndpoint")) return endpoint(e);
  if (J e = key(t, "titleNavigationEndpoint")) return endpoint(e);
  J first = idx(key(t, "runs"), 0);
  return endpoint(key(first, "navigationEndpoint"));
}

std::string widest(J thumbs) {
  J list = yyjson_is_arr(thumbs) ? thumbs : key(thumbs, "thumbnails") ? key(thumbs, "thumbnails") : key(thumbs, "sources");
  std::string best;
  int64_t bestW = -1;
  each(list, [&](J t) {
    std::string url = str(key(t, "url"));
    int64_t w = num(key(t, "width"), 0);
    if (!url.empty() && w > bestW) best = url, bestW = w;
  });
  return best;
}

J find(J v, const char* name, int depth) {
  if (!v || depth < 0) return nullptr;
  if (yyjson_is_obj(v)) {
    if (J hit = yyjson_obj_get(v, name)) return hit;
    size_t i, n;
    J k, x;
    yyjson_obj_foreach(v, i, n, k, x) {
      if (J hit = find(x, name, depth - 1)) return hit;
    }
  } else if (yyjson_is_arr(v)) {
    size_t i, n;
    J x;
    yyjson_arr_foreach(v, i, n, x) {
      if (J hit = find(x, name, depth - 1)) return hit;
    }
  }
  return nullptr;
}

void findAll(J v, const char* name, std::vector<J>& out, int depth) {
  if (!v || depth < 0) return;
  if (yyjson_is_obj(v)) {
    if (J hit = yyjson_obj_get(v, name)) {
      out.push_back(hit);
      return;
    }
    size_t i, n;
    J k, x;
    yyjson_obj_foreach(v, i, n, k, x) findAll(x, name, out, depth - 1);
  } else if (yyjson_is_arr(v)) {
    size_t i, n;
    J x;
    yyjson_arr_foreach(v, i, n, x) findAll(x, name, out, depth - 1);
  }
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::string bullets(const std::string& s) {
  static const std::string dot = "\xe2\x80\xa2";  // •
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t at = s.find(dot, i);
    if (at == std::string::npos) {
      out += s.substr(i);
      break;
    }
    std::string before = s.substr(i, at - i);
    while (!before.empty() && isspace((unsigned char)before.back())) before.pop_back();
    out += before + " \xc2\xb7 ";
    i = at + dot.size();
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
  }
  return trim(out);
}

std::string joinDot(const std::vector<std::string>& parts) {
  std::string out;
  for (const std::string& p : parts) {
    if (p.empty()) continue;
    if (!out.empty()) out += " \xc2\xb7 ";
    out += p;
  }
  return out;
}

int clockSeconds(const std::string& raw) {
  std::string s = trim(raw);
  if (s.empty() || s.size() > 9) return 0;
  int total = 0, parts = 0, digits = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    if (i == s.size() || s[i] == ':') {
      if (digits == 0 || (parts > 0 && digits != 2)) return 0;
      parts++;
      digits = 0;
      continue;
    }
    if (!isdigit((unsigned char)s[i])) return 0;
    digits++;
  }
  if (parts < 2 || parts > 3) return 0;
  size_t a = 0;
  while (a <= s.size()) {
    size_t b = s.find(':', a);
    if (b == std::string::npos) b = s.size();
    total = total * 60 + atoi(s.substr(a, b - a).c_str());
    a = b + 1;
  }
  return total;
}

std::string quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          out += b;
        } else {
          out += char(c);
        }
    }
  }
  return out + "\"";
}

std::string uriComponent(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (isalnum(c) || strchr("-_.!~*'()", c)) out += char(c);
    else out += '%', out += hex[c >> 4], out += hex[c & 15];
  }
  return out;
}

std::string uriDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      out += char(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else if (s[i] == '+') {
      out += ' ';
    } else {
      out += s[i];
    }
  }
  return out;
}

}  // namespace rondo::direct
