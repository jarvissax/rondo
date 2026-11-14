// Organizing the card the way Jellyfin (and Plex and Kodi) expect a media library, from the files'
// tags:
//
//   Music/<Album artist>/<Album> (<Year>)/<NN> - <Title>.flac        (Disc N/ inside a multi-disc album)
//   Videos/Shows/<Series> (<Years>)/Season <NN>/<Series> S<NN>E<NN>.mp4  (a file named like S01E02;
//                                                                        years: "2023-" for a series
//                                                                        still running, "2010-2022")
//   Videos/Movies/<Title> (<Year>)/<Title> (<Year>).mp4                 (any other video)
//
// A song's .lrc and a video's .srt move with it, and a folder's cover picture moves to the album's
// folder when that folder held one album only. Nothing is moved until a plan is applied: planOrganize
// only works out where everything would go (the preview). No Rondo state here: the shell carries the
// library's entries, plays, ratings, playlists and the queue along with what applyOrganize moved.
//
// A series' years come from its folder ("Silo (2023-)"), a movie's year from its file or folder name
// or its tags; what the names don't say, the dashboard looks up online (shell/media_lookup.h) and
// hands to planOrganize as OrganizeNames. Once organized, the folders keep what was found.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rondo {

struct OrganizeItem {  // a file of the library and its tags
  std::string path, title, artist, albumArtist, album;
  int track = 0, disc = 0, year = 0;
  bool video = false;
};

struct OrganizeMove {
  enum Kind { Song, Video, Lyrics, Subtitles, Cover } kind;
  std::string from, to;
};

struct OrganizePlan {
  std::vector<OrganizeMove> moves;  // the files first, each followed by its own sidecars
  int songs = 0, videos = 0, sidecars = 0, covers = 0;
  int alreadyThere = 0;  // files already where they belong
};

// A series or a movie among the card's videos, as Organize names it from the files
struct VideoTitle {
  bool series = false;
  std::string key;    // what OrganizeNames goes by: a series' name folded, a movie's file
  std::string name;   // "Stranger Things", "Interstellar"
  std::string years;  // a series': "2016", "2016-", "2016-2025"; a movie's: "2014"; "" when unknown
  std::vector<std::string> files;  // a series': its episodes
};
// card: the root the folders go into ("/storage/sdcard0"); YouTube's and YouTube Music's downloads left out
std::vector<VideoTitle> videoTitles(const std::vector<OrganizeItem>& items, const std::string& card);

// Names found for VideoTitle keys (the dashboard's lookup): the name and years to file them under
struct OrganizeNames {
  struct Name {
    std::string name, years;
  };
  std::map<std::string, Name> titles;
};

// card: the root the folders go into ("/storage/sdcard0"); only items under it are moved
OrganizePlan planOrganize(const std::vector<OrganizeItem>& items, const std::string& card, const OrganizeNames& names = {});

struct OrganizeResult {
  std::vector<OrganizeMove> done;  // what moved
  std::vector<std::string> errors;  // what couldn't ("<file>: <why>")
};
// Moves the files (rename() within the card), making folders as needed, then removes the folders the
// moves left empty. progress(done, total) after each file.
OrganizeResult applyOrganize(const OrganizePlan& plan, const std::string& card,
                             const std::function<void(int, int)>& progress = nullptr);

// A name as a folder or file name on the card (FAT): no / \ : * ? " < > |, no trailing dots or spaces,
// at most 80 bytes
std::string cardName(const std::string& name);

}  // namespace rondo
