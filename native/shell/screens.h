// The screens other screens open.
#pragma once
#include <memory>

#include "app.h"

namespace rondo::shell {

enum class Scope { All, Songs, Artists, Albums, Videos, Playlists };

std::unique_ptr<Screen> makeMainMenu();
std::unique_ptr<Screen> makeDevice();
std::unique_ptr<Screen> makeSettings();
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
float statusIcons(Env& e, float right, int rowH, Color c);

}  // namespace rondo::shell
