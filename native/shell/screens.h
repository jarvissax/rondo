// The screens other screens open.
#pragma once
#include <memory>

#include "app.h"

namespace rondo::shell {

enum class Scope { All, Songs, Artists, Albums, Videos, Playlists };

std::unique_ptr<Screen> makeMainMenu();
std::unique_ptr<Screen> makeDevice();
std::unique_ptr<Screen> makeSearch(Scope s);
// openNowPlaying() in 1.0.x: the Now Playing screen for what plays (nothing when nothing does)
void openNowPlaying();

// 1.0.x's local Item for a file: its title and "artist · album" (LocalLibrary.item), and the subtitle's
// first part (the artist) and the rest (the album).
struct SongInfo {
  std::string title, subtitle;
};
SongInfo songInfo(const std::string& path);
std::pair<std::string, std::string> firstAndRest(const std::string& subtitle);
// ArtColor.dominant(): the artwork's average color from a 12 x 12 sample, more saturated, of medium
// brightness; false while the picture loads (or there is none).
bool artColor(Env& e, const std::string& path, Color* out);

}  // namespace rondo::shell
