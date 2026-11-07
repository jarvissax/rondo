// The screens other screens open.
#pragma once
#include <memory>

#include "../core/api.h"
#include "app.h"

namespace rondo::shell {

enum class Scope { All, Songs, Artists, Albums, Videos, Playlists };

std::unique_ptr<Screen> makeMainMenu();
std::unique_ptr<Screen> makeDevice();
std::unique_ptr<Screen> makeSettings();
// YouTube Music (music_screen.cpp) and its search
std::unique_ptr<Screen> makeMusic();
std::unique_ptr<Screen> makeMusicSearch();
// YouTube (video_screen.cpp), its search, and a video's player
std::unique_ptr<Screen> makeVideo();
std::unique_ptr<Screen> makeVideoSearch();
void openVideo(const Item& item);
std::unique_ptr<Screen> makeShorts();
// a video's sound (/s/<id>/140, or the file) and its pictures to try for a quality (video_player.cpp)
std::string videoSound(const Item& item);
std::vector<std::string> videoPictures(const Item& item, int height);
// streams.cpp: songs the server streams in the player's queue ("/s/<id>/140"), what the screens show
// about them, and the artwork of any queue entry at a size (a file's own path; a stream's thumbnail)
std::string streamPath(const Item& it);
bool isStream(const std::string& path);
const Item* streamItem(const std::string& path);
void playStreams(const std::vector<Item>& items, int start);
void playStreamsAppend(const Item& item, bool next);  // Play next / Add to queue
// music_screens.cpp: a playlist, album or artist row's screen; a song's menu
void openMusicContainer(const Item& item);
void openSongMenu(const Item& song);
std::string largeThumb(const std::string& thumb, int size);
std::string artOf(const std::string& path, int size);
// Wi-Fi (wifi_screens.cpp): its screen, and what Settings says about it (1.0.x's wifiName())
std::unique_ptr<Screen> makeWifi();
std::string wifiName();
// Bluetooth (bluetooth_screens.cpp): the screen, Settings' row, and the loop's headset buttons and pause
// FM radio (fm_screens.cpp): the radio's screen, a frequency as 1.0.x writes it (101.7), the loop's remembering
std::unique_ptr<Screen> makeFm();
std::string fmName(int frequency);
void fmTick();
std::unique_ptr<Screen> makeBluetooth();
std::string bluetoothState();
void bluetoothTick();
// the USB drive (usb_screen.cpp): its screen; the cable plugged into a computer or pulled out; the
// end of a switch nobody watched (true: something to draw again)
std::unique_ptr<Screen> makeUsbDrive();
void usbCableChanged(bool computer);
bool usbIdle();
std::unique_ptr<Screen> makeSearch(Scope s);
// openNowPlaying() in 1.0.x: the Now Playing screen for what plays (nothing when nothing does)
void openNowPlaying();
// a Now Playing screen whatever plays, for drawing it once unseen at startup
std::unique_ptr<Screen> makeNowPlayingPreview();
// QueueScreen: Up next, with shuffle and repeat
std::unique_ptr<Screen> makeQueue();

// the first row of a long list: search just that list
Row searchRow(Scope s);
// Library search picking songs for a playlist ("Add to <name>")
std::unique_ptr<Screen> makeSearchToAdd(const std::string& playlist);
// playlists (playlist_screens.cpp)
std::unique_ptr<Screen> makePlaylists();
std::unique_ptr<Screen> makePlaylist(const std::string& name);
std::unique_ptr<Screen> makePlaylistMenu(const std::string& name);
// the menu of a song on the card; from a playlist, with moving and removing
std::unique_ptr<Screen> makeSongMenu(const std::string& path, const std::string& playlist = "");
std::unique_ptr<Screen> makeAddToPlaylist(std::vector<std::string> paths);
// one line typed on the wheel keyboard: onDone answers "" when it took it, else the problem to show
std::unique_ptr<Screen> makeTextEntry(std::string title, std::string initial, std::string action, size_t maxLength,
                                      std::function<std::string(const std::string&)> onDone,
                                      std::function<void(const std::string&)> then);

// 1.0.x's local Item for a file: its title and "artist · album" (LocalLibrary.item), and the subtitle's
// first part (the artist) and the rest (the album).
struct SongInfo {
  std::string title, subtitle;
};
SongInfo songInfo(const std::string& path);
std::pair<std::string, std::string> firstAndRest(const std::string& subtitle);
// ArtColor.dominant(): the artwork's average color from a 12 x 12 sample, more saturated, of medium
// brightness, from the picture decoded at sourcePx (the view showing it); false while it loads.
bool artColor(Env& e, const std::string& path, int sourcePx, Color* out);
// StatusIcons: Wi-Fi (dimmed offline), the percentage and the battery, ending at `right` in a row
// rowH tall; returns its left edge.
// hidePercent: a header that runs out of room (YouTube's three pills) keeps the battery only
float statusIcons(Env& e, float right, int rowH, Color c, bool showWifi = true, bool hidePercent = false);
float statusIconsWidth(Env& e, bool showWifi);

}  // namespace rondo::shell
