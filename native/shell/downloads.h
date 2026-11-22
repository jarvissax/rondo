// YouTube › Download: a video the user picks, saved to the card at the quality they choose (360p,
// 480p or 720p, the next one down when it has no such height) with its subtitles, to play with no
// network. One at a time, the rest waiting; each one's picture and sound come from the server's (or
// direct mode's) /s/ streams like playback, and are put together into one .mp4:
//   Videos/YouTube/<Title> [<id>].mp4, with <Title> [<id>].<lang>.vtt beside it
// Library › Videos lists them with the other videos, YouTube › Downloads on their own. Nothing is kept
// that the user didn't ask for: the parts are deleted once joined, or when the download fails.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "../core/api.h"

namespace rondo::shell {

std::string downloadDir();  // the card's Videos/YouTube

//
// The queue is kept (download-queue.json beside prefs.json): a restart, an update or a crash finds it as
// it was. What a download has fetched stays on the card (hidden, beside where it goes) until it is
// whole, so a paused or interrupted one goes on from there (a Range request: the server's /s/ and
// direct mode's both take them); every 1 MB it is flushed to the card and that size noted
// (download-part), so after a power cut it goes on from bytes the card surely has.
// It holds itself, and says why: while the card is the computer's (USB drive), with no network
// (Wi-Fi gone: it goes on when it's back, instead of failing), and with the card full.
struct DownloadJob {
  std::string id, title, channel, thumb;
  bool song = false;  // a YouTube Music song (an .m4a in Music/YouTube Music), else a video
  bool alone = true;  // a song asked for by itself, not only for a downloaded playlist (kept when the playlist goes)
  int quality = 480;  // the height asked for (a video)
  enum State { Waiting, Running, Done, Failed, Paused } state = Waiting;
  int percent = 0;
  // what a running one does now, in the Y1's language ("Downloading video", "Getting subtitles",
  // "Saving"); fetching: a part coming in (a row shows its percent then), the stage's words aside
  std::string stage;
  bool fetching = false;
  std::string error, file;  // error: why it failed, in the Y1's language
  bool cancelled = false;   // failed because it was cancelled: not kept in the queue's file
};

// Queued (false: already on the card or in the queue); a toast says which
bool downloadVideo(const Item& item, int quality);
const std::vector<DownloadJob>& downloadJobs();  // the queue: the kept one and this session's, the newest last
// one running, or waiting with nothing holding it: the CPU stays up with the screen off (power_screens.cpp)
bool downloadsActive();
void cancelDownload(const std::string& id);      // a waiting, running or paused one, its parts deleted; a failed one off the list
void retryDownload(const std::string& id);       // a failed one waits again, in its place
// Pause and resume: one, or all (videos and songs: one queue). A paused one keeps what it has.
void pauseDownload(const std::string& id);
void resumeDownload(const std::string& id);
void pauseDownloads();
void resumeDownloads();
// Moves on whenever anything a screen shows of the downloads changes (a percent too)
uint32_t downloadsSerial();
bool downloadsGoing();  // one waiting or running (the screens' Pause row)
bool downloadsPaused();  // one paused (Resume)
// Why nothing starts now, in the Y1's language ("" nothing holds them): "The card is on the computer",
// "Waiting for Wi-Fi", "Waiting to try again", "The card is full"
std::string downloadsHeld();
// USB drive: on, every download lets go of the card first (a file kept open there gets Rondo killed
// when vold takes it); they go on when it's back. Thread-safe (the switch's thread calls it).
void downloadsCardAway(bool away);
bool downloadsUsingCard();  // a file still open on the card: the switch waits for it to close
// The card's file of this video ("" none)
std::string downloadedFile(const std::string& id);
// The video id in a download's file name ("" when it isn't one)
std::string downloadIdOf(const std::string& path);
// The downloaded videos on the card, the newest first (id: the file; subtitle: the channel, size)
std::vector<Item> downloadedVideos();
// Deletes a download and its subtitles
bool deleteDownload(const std::string& file);
// The queue's work, from the loop: true when something a screen shows changed
bool downloadsTick();
// development (rondo --join): a download's two parts into one .mp4, as its last step does
bool joinDownloadParts(const std::string& video, const std::string& audio, const std::string& out);
// The queue and the files as JSON (the dashboard's GET /api/downloads); a job's "fetching" is its
// fetching above (its "stage", "error" and "held" are words to show, in the Y1's language)
std::string downloadsJson();

// YouTube Music › Download, in the same queue: a song saved the way the library keeps music (the layout
// Jellyfin reads), its AAC as YouTube has it (itag 140, nothing re-encoded) in an .m4a tagged with its
// details (the server's or direct mode's /music/song: artist, album, year, track), cover, lyrics and
// link:
//   Music/YouTube Music/<Album artist>/<Album> (<Year>)/<NN> - <Title>.m4a
// Organize leaves the folder be. YouTube Music › Downloads lists them; they play from the card.
std::string musicDownloadDir();  // the card's Music/YouTube Music
// Queued (false: on the card or queued already; a toast says which unless quiet). alone: asked for by
// itself (false: for a playlist)
bool downloadSong(const Item& song, bool quiet = false, bool alone = true);
int downloadSongs(const std::vector<Item>& songs);  // an album's: how many were queued
std::string downloadedSong(const std::string& id);  // the card's file of this song ("" none)
// The downloaded songs on the card, the newest first (id: the file; subtitle: artist · album)
std::vector<Item> downloadedSongs();
bool deleteSong(const std::string& file);  // and its entry; out of every playlist too

// YouTube Music › a playlist › Download playlist: every song of it (a mix: the ones its first page has,
// as it never ends) in the queue, and a playlist of them in Library, in YouTube's order:
//   Playlists/YouTube Music/<Title>.m3u8
// holding the songs on the card so far (each one that comes is put in its place). A song already on
// the card is the same file, whatever playlists have it. Downloading it again brings it up to date:
// its new songs come, the order is YouTube's; a song taken off it on YouTube leaves the playlist, not the card.
// Its cover (YouTube's picture of it, or its first song's) is beside it, <Title>.jpg: Library shows it.
struct PlaylistDownload {
  std::string id, title;
  std::string name;  // Library's (Playlists::youTubeKey)
  std::string cover;  // its picture on the card, beside its file ("" none yet)
  bool mix = false;
  bool loading = false;  // its songs being asked for
  int total = 0, onCard = 0, going = 0, paused = 0;  // its songs; on the card; waiting, running or paused; paused
};
bool downloadPlaylist(const std::string& id, const std::string& title);  // false: being asked for already
std::vector<PlaylistDownload> playlistDownloads();  // the newest first
bool playlistDownload(const std::string& id, PlaylistDownload& out);
std::string playlistDownloadOf(const std::string& libraryName);  // the YouTube id of a Library playlist ("" none)
// Its file goes, and the downloads only it was waiting for; songsToo: its songs on the card too, except
// those another downloaded playlist has or that were downloaded by themselves
bool deletePlaylistDownload(const std::string& id, bool songsToo);
// Library's playlist by its name (Playlists): a downloaded one through the above, else its file
bool deleteLibraryPlaylist(const std::string& name, bool songsToo);
bool isMixId(const std::string& id);  // a radio (RD…, not an album's RDCLAK…): no end

}  // namespace rondo::shell
