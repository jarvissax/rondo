// The YouTube Music screens' building blocks, 1:1 with 1.0.x (ui/screens/Common.kt, CollectionScreens.kt):
// the dark header, the background tinted by the selected artwork, the mini player, and MusicListScreen,
// which puts them around a MUSIC RowList.
#pragma once
#include "../core/api.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {

// darkHeader(): 31 units (28 on the Wi-Fi password screen), a logo (16 units), the title (14 bold), the
// status icons.
enum class HeaderLogo { None, Music, Wifi, Video };
void darkHeader(Env& e, float w, const std::string& title, HeaderLogo logo, Color titleColor = 0xffffffff, float heightUnits = 31);

// Ambient: like YouTube Music's ambient color, the selected row's artwork color at the top fading into
// #0f0f0f. 1.0.x swaps it at once; here the new color fades in over 200 ms.
class Ambient {
 public:
  // The artwork shown for the selection (decoded at artPx); "" for none.
  // artPxH: the picture's height as its row shows it (0: square)
  void follow(Env& e, const std::string& thumb, int artPx, int artPxH = 0);
  void draw(Env& e, Rect r);
  bool fading(int64_t now) const { return fade_.running(now); }
  // the background's color at height y of a screen h tall (what a translucent fill over it comes to)
  Color colorAt(Env& e, float y, float h) const;

 private:
  void fill(Env& e, Rect r, bool has, Color c, float alpha);
  bool has_ = false, hadFrom_ = false;
  Color shown_ = 0, from_ = 0;
  Tween fade_;
};

// Common.kt's music rows: songs play with their siblings as the queue, containers (playlists, albums,
// artists) open their own screen. `queue` is what plays around a chosen song (everything a paged list
// has loaded so far).
std::vector<Row> musicRows(const std::vector<Item>& items, std::function<std::vector<Item>()> queue);
void openMusicItem(const Item& item, const std::vector<Item>& siblings);
// videoRows(): YouTube's rows; center plays the video
std::vector<Row> videoRows(const std::vector<Item>& items);
// hold center on a YouTube video: Download, at a height picked (downloads.h)
void askDownload(const Item& item);
// offlineOr(): what a list says when it couldn't load (pointing at Settings › Wi-Fi without a network)
std::string offlineOr(const std::string& problem);
// "HTTP 500 for /music/home", or the connection's failure, as 1.0.x words it
std::string httpProblem(int status, const std::string& path);

// MiniPlayer: 32 units at the bottom of the music screens while a song plays: a progress line, the
// artwork, title and artist, the play state.
constexpr float kMiniPlayer = 32;
bool miniPlayerShown();
void miniPlayer(Env& e, Rect r);

// MusicListScreen: the header, a MUSIC RowList and the mini player, on the ambient background. It
// draws its own header, so no shell status bar.
class MusicListScreen : public Screen {
 public:
  explicit MusicListScreen(std::string title) : title_(std::move(title)) { list_.style = RowStyle::Music; }
  std::string title() const override { return title_; }
  bool statusBar() const override { return false; }
  bool waitsForContent() const override { return true; }
  bool onAction(Action a, int steps) override;
  bool step(float dt) override;
  bool tick(int64_t now) override;
  void draw(Env& e, float w, float h) override;

 protected:
  std::string title_;
  RowList list_;
  Ambient ambient_;
  int64_t now_ = 0;
  std::string miniSig_;
};

// YouTube Music's and YouTube's screens (MusicScreen.kt, VideoScreens.kt): a pill per tab and the round
// search button in the header, which take the wheel when it turns up past the first row
class TabbedListScreen : public MusicListScreen {
 public:
  using MusicListScreen::MusicListScreen;
  bool onAction(Action a, int steps) override;
  bool tick(int64_t now) override;

 protected:
  // the tabs for what the source has now (an account's tabs come and go with it: a switch between
  // the server and direct mode, a sign-in); syncTabs() takes them when they changed, true if so
  virtual std::vector<std::string> wantedTabs() const = 0;
  bool syncTabs();
  virtual void load(int tab) = 0;
  virtual void openSearch() = 0;
  void focusTabsFromTop();  // the list's top hands the wheel to the pills (call once rows are set up)
  // the pills from x in a header H tall; returns where the search button ends
  float drawPills(Env& e, float x, int H, float screenH);
  std::vector<std::string> tabs_;
  int active_ = 0, focused_ = -1;  // the pills, then the search button at tabs_.size()
};

}  // namespace rondo::shell
