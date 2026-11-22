// Settings › Language, and the picker the very first start shows before anything else (as the stock
// firmware does): every language in lang/, each in its own name, the status bar's title in the
// highlighted one. A pick applies at once, no restart: the screens open are made again in it.
#include "../core/lang.h"
#include "app.h"
#include "main_queue.h"
#include "screens.h"

namespace rondo::shell {
namespace {

class LanguageScreen : public ShellListScreen {
 public:
  explicit LanguageScreen(bool firstBoot) : ShellListScreen(tr("language.title")), first_(firstBoot) {
    langs_ = lang::available();
    if (langs_.empty()) langs_.push_back({"en", "English"});
    std::string now = first_ ? "en" : lang::current();
    std::vector<Row> rows;
    int at = 0;
    for (size_t i = 0; i < langs_.size(); i++) {
      Row r;
      r.title = langs_[i].name;
      if (langs_[i].code == now) at = int(i);
      if (!first_ && langs_[i].code == now) r.trailing = tr("common.current");
      std::string code = langs_[i].code;
      bool first = first_;
      r.onSelect = [code, first] { setLanguage(code, first ? LanguageFrom::FirstBoot : LanguageFrom::Settings); };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows));
    list_.selectIndex(at);
  }
  // "Language", "Idioma"…: the word in the language the wheel is on
  std::string title() const override {
    int i = list_.selected();
    return i >= 0 && i < int(langs_.size()) ? lang::trIn(langs_[size_t(i)].code, "language.title") : title_;
  }
  bool onAction(Action a, int steps) override {
    // the first start waits for a language: Center picks the highlighted one
    if (first_ && a == Action::Back) return true;
    return ShellListScreen::onAction(a, steps);
  }

 private:
  bool first_;
  std::vector<lang::Language> langs_;
};

}  // namespace

std::unique_ptr<Screen> makeLanguage(bool firstBoot) { return std::unique_ptr<Screen>(new LanguageScreen(firstBoot)); }

void setLanguage(const std::string& code, LanguageFrom from) {
  if (code != lang::current() && !lang::use(code)) return;
  g_prefs.language = code;
  savePrefsNow();
  // YouTube's own words in it too: the requests from now on say it (and what was kept is fetched again)
  if (g_onServerChanged) g_onServerChanged();
  // after the action that asked (the picker's row runs this, and goes with its screen)
  postToMain([from] {
    std::vector<std::unique_ptr<Screen>> fresh;
    fresh.push_back(makeMainMenu());
    if (from == LanguageFrom::Settings) fresh.push_back(makeSettings("language"));
    if (from == LanguageFrom::Dashboard) fresh.push_back(makeSettings("dashboard"));
    g_app->restack(std::move(fresh), from == LanguageFrom::Dashboard);
  });
}

}  // namespace rondo::shell
