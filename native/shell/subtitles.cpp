// Subtitles; see subtitles.h.
#include "subtitles.h"

#include <dirent.h>
#include <string.h>
#include <strings.h>

#include <algorithm>

#include "app.h"

namespace rondo::shell {

const SubColor kSubColors[8] = {{"White", 0xffffff}, {"Yellow", 0xffff00}, {"Green", 0x00ff00}, {"Cyan", 0x00ffff},
                                {"Blue", 0x0000ff},  {"Magenta", 0xff00ff}, {"Red", 0xff0000},  {"Black", 0x000000}};
const char* const kSubSizes[4] = {"Small", "Normal", "Large", "Extra large"};
const char* const kSubEdges[5] = {"None", "Drop shadow", "Raised", "Depressed", "Outline"};

void resetSubtitleStyle() {
  Prefs d;
  g_prefs.subSize = d.subSize, g_prefs.subColor = d.subColor, g_prefs.subOpacity = d.subOpacity;
  g_prefs.subBack = d.subBack, g_prefs.subBackOpacity = d.subBackOpacity, g_prefs.subEdge = d.subEdge;
}

namespace {

bool endsWithCi(const std::string& s, const char* tail) {
  size_t l = strlen(tail);
  return s.size() >= l && strcasecmp(s.c_str() + s.size() - l, tail) == 0;
}

float sizeSp(int i) {
  static const float kSp[4] = {10, 12, 15, 19};
  return kSp[std::max(0, std::min(3, i))];
}

Color withAlpha(uint32_t rgb, int percent) {
  int a = std::max(0, std::min(100, percent)) * 255 / 100;
  return uint32_t(a) << 24 | (rgb & 0xffffff);
}

// the lines of `text`, each wrapped to maxW (3 lines at most from one), bottom up from `bottom`
void drawLines(Env& e, const std::vector<std::string>& texts, float w, float bottom) {
  float sp = sizeSp(g_prefs.subSize);
  int font = e.font(sp, false);
  float padX = floorf(e.text.lineHeight(font) * 0.25f + 0.5f);
  float maxW = floorf(w * 0.9f) - 2 * padX;
  std::vector<Label> lines;
  for (const std::string& t : texts) {
    size_t s = 0;
    while (s <= t.size()) {
      size_t n = t.find('\n', s);
      std::string one = t.substr(s, n == std::string::npos ? std::string::npos : n - s);
      for (Label& l : wrap(e, one, sp, false, maxW, 3)) lines.push_back(l);
      if (n == std::string::npos) break;
      s = n + 1;
    }
  }
  if (lines.empty()) return;
  const SubColor& fg = kSubColors[std::max(0, std::min(7, g_prefs.subColor))];
  const SubColor& bg = kSubColors[std::max(0, std::min(7, g_prefs.subBack))];
  Color text = withAlpha(fg.rgb, g_prefs.subOpacity), box = withAlpha(bg.rgb, g_prefs.subBackOpacity);
  // the edge in black, or white for black text (so it stays seen)
  uint32_t edgeRgb = fg.rgb == 0 ? 0xffffff : 0;
  Color edge = withAlpha(edgeRgb, std::min(100, g_prefs.subOpacity + 10)), light = withAlpha(edgeRgb ^ 0xffffff, g_prefs.subOpacity / 2);
  float o = std::max(1.f, floorf(e.dpf(1) + 0.5f));
  // on whole pixels: the glyph atlas is sampled linearly, so a line between two rows (the bottom is
  // h - 12 units, 340.2 px at the Y1's scale) came out smeared over both, blurred
  float y = floorf(bottom + 0.5f);
  for (size_t k = lines.size(); k-- > 0;) {
    const Label& l = lines[k];
    y -= float(l.h);
    float bw = float(l.w) + 2 * padX, x = floorf((w - bw) / 2);
    if (g_prefs.subBackOpacity > 0) {
      e.c.layer(4);
      e.c.fill({x, y, bw, float(l.h)}, box);
    }
    e.c.layer(4);
    float tx = x + padX;
    switch (g_prefs.subEdge) {
      case 1: drawLabel(e, l, tx + floorf(o * 1.5f + 0.5f), y + floorf(o * 1.5f + 0.5f), edge); break;  // drop shadow
      case 2:                                                              // raised: lit above, shade below
        drawLabel(e, l, tx - o, y - o, light);
        drawLabel(e, l, tx + o, y + o, edge);
        break;
      case 3:  // depressed: shade above, lit below
        drawLabel(e, l, tx - o, y - o, edge);
        drawLabel(e, l, tx + o, y + o, light);
        break;
      case 4:  // outline
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) drawLabel(e, l, tx + dx * o, y + dy * o, edge);
        break;
      default: break;
    }
    drawLabel(e, l, tx, y, text);
  }
}

}  // namespace

std::vector<SubTrack> subtitleFiles(const std::string& videoPath) {
  std::vector<SubTrack> out;
  size_t slash = videoPath.rfind('/');
  if (slash == std::string::npos) return out;
  std::string dir = videoPath.substr(0, slash), name = videoPath.substr(slash + 1);
  size_t dot = name.rfind('.');
  std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* en = readdir(d)) {
    std::string f = en->d_name;
    if (f.size() <= stem.size() || f.compare(0, stem.size(), stem) != 0 || f[stem.size()] != '.') continue;
    if (!endsWithCi(f, ".srt") && !endsWithCi(f, ".vtt") && !endsWithCi(f, ".ass") && !endsWithCi(f, ".ssa")) continue;
    // what's between the video's name and the kind: "en", "en.forced", "English.sdh", "az.auto"…
    std::string middle = f.substr(stem.size() + 1, f.size() - stem.size() - 5);
    SubTrack t;
    t.file = dir + "/" + f;
    t.id = f;
    std::vector<std::string> flags;
    size_t a = 0;
    while (!middle.empty() && a <= middle.size()) {
      size_t b = middle.find('.', a);
      std::string part = middle.substr(a, b == std::string::npos ? std::string::npos : b - a);
      // "hi" is Hindi first, hearing-impaired after a language
      if (strcasecmp(part.c_str(), "auto") == 0) t.autoGen = true;
      else if (strcasecmp(part.c_str(), "forced") == 0) flags.push_back("forced");
      else if (strcasecmp(part.c_str(), "sdh") == 0 || strcasecmp(part.c_str(), "cc") == 0) flags.push_back("SDH");
      else if (strcasecmp(part.c_str(), "default") == 0) {
      } else if (t.lang.empty() && findLanguage(part)) t.lang = findLanguage(part)->code;
      else if (t.lang.empty() && languageNamed(part)) t.lang = languageNamed(part)->code;
      else if (strcasecmp(part.c_str(), "hi") == 0) flags.push_back("SDH");
      else if (t.name.empty()) t.name = part;
      if (b == std::string::npos) break;
      a = b + 1;
    }
    if (t.name.empty()) t.name = t.lang.empty() ? "Subtitles" : languageName(t.lang);
    if (t.autoGen) t.name += " (auto-generated)";
    for (const std::string& fl : flags) t.name += " \xc2\xb7 " + fl;
    out.push_back(std::move(t));
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const SubTrack& a, const SubTrack& b) {
    if (a.autoGen != b.autoGen) return !a.autoGen;
    return a.name < b.name;
  });
  return out;
}

int pickTrack(const std::vector<SubTrack>& tracks) {
  if (tracks.empty()) return -1;
  const std::string& want = g_prefs.subLang;
  for (int pass = 0; pass < 2 && !want.empty(); pass++)
    for (size_t i = 0; i < tracks.size(); i++)
      if (tracks[i].autoGen == (pass == 1) && strcasecmp(tracks[i].lang.c_str(), want.c_str()) == 0) return int(i);
  for (size_t i = 0; i < tracks.size(); i++)
    if (!tracks[i].autoGen) return int(i);
  return 0;
}

uint64_t cuesAt(const std::vector<Cue>& cues, int64_t ms, int64_t* nextChange) {
  uint64_t key = 1469598103934665603ull;
  int64_t next = -1;
  for (size_t i = 0; i < cues.size(); i++) {
    const Cue& c = cues[i];
    if (c.start > ms) {
      if (next < 0 || c.start < next) next = c.start;
      break;  // sorted by start: none after it shows yet
    }
    if (ms < c.end) {
      key = (key ^ (i + 1)) * 1099511628211ull;
      if (next < 0 || c.end < next) next = c.end;
    }
  }
  if (nextChange) *nextChange = next;
  return key;
}

void drawSubtitles(Env& e, const std::vector<Cue>& cues, int64_t ms, float w, float bottom) {
  std::vector<std::string> texts;
  for (const Cue& c : cues) {
    if (c.start > ms) break;
    if (ms < c.end) texts.push_back(c.text);
  }
  drawLines(e, texts, w, bottom);
}

void drawSubtitleSample(Env& e, const std::string& text, float w, float bottom) { drawLines(e, {text}, w, bottom); }

}  // namespace rondo::shell
