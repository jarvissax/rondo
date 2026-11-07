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
enum class HeaderLogo { None, Music, Wifi };
void darkHeader(Env& e, float w, const std::string& title, HeaderLogo logo, Color titleColor = 0xffffffff, float heightUnits = 31);

// Ambient: like YouTube Music's ambient color, the selected row's artwork color at the top fading into
// #0f0f0f. 1.0.x swaps it at once; here the new color fades in over 200 ms.
class Ambient {
 public:
  // The artwork shown for the selection (decoded at artPx); "" for none.
  void follow(Env& e, const std::string& thumb, int artPx);
  void draw(Env& e, Rect r);
  bool fading(int64_t now) const { return fade_.running(now); }

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

}  // namespace rondo::shell
