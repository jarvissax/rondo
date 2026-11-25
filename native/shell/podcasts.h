// YouTube Music's podcasts (YouTube Music › Podcasts, a show's screen, an episode's menu): episodes play
// as spoken word through books.h's session, as audiobooks do (their place kept, a resume a little
// earlier after a pause, Next and Previous by YouTube's chapters or 30 s and 15 s, bookmarks, the sleep
// timer), at their show's speed, and the music waits for them. Played marks go both ways with the
// account: an episode finished here is Played on the phone (YouTube Music's own Mark as played), one
// marked or finished on the phone shows Played here. Where one stopped stays exact on the Y1; one only
// YouTube knows was started opens at about where YouTube says (its percent).
//
//   podcasts.json   beside prefs.json: the episodes played, queued or downloaded (what they are, their
//                   chapters, the card's file, the Played mark last agreed with YouTube), and Episodes
//                   up next. Metadata only; places, speeds and bookmarks are the library's (books.h).
#pragma once
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/api.h"
#include "../core/books.h"
#include "app.h"

namespace rondo::shell {

struct BookPlay;

// An episode as YouTube Music lists it (server/src/podcasts.ts's Episode), with what /music/episode adds
struct Episode {
  std::string id, title, show, showId, thumb;
  std::string date, status;  // YouTube's words, in the Rondo's language: "6d ago"; "3 hr 21 min", "45 min left", "Played"
  std::string description;
  int64_t lengthMs = 0;  // 0: not known yet
  int progress = -1;     // how much of it YouTube says the account heard, 0..100 (-1: it didn't say)
  bool played = false;   // YouTube's Played mark (progress >= 0)
  int later = -1;        // in Episodes for Later (-1: not known)
  std::vector<FileChapter> chapters;  // YouTube's chapter markers
  bool detailed = false;              // /music/episode answered: chapters and length are what YouTube has
};

// ---- the spoken-word session's (books.cpp) ----
bool isEpisodeKey(const std::string& key);  // "yt:<id>"
std::string episodeKey(const std::string& id);
std::string episodeIdOf(const std::string& key);
std::string showSpeedKey(const std::string& showId);  // "ytshow:<id>": a show's speed, for all its episodes
std::string episodeSpeedKey(const std::string& key);  // its show's (its own key when the show isn't known)
bool episodeDetails(const std::string& key, BookPlay& out);  // its file or stream, length, chapters
void episodeFinishedChanged(const std::string& key, bool finished);  // YouTube's Played mark follows
bool episodeEnded(const std::string& key);  // the next of Episodes up next started (true)
void episodeRestored(const std::string& key, const std::string& path);  // a streamed one back after a restart

// ---- reading YouTube Music's answers ----
bool parseEpisode(const std::string& json, Episode& out);  // /music/episode/<id>
// a list's items: its episodes (kind "episode") and shows (kind "podcast", as Items); next: its token
void parseEpisodes(const std::string& json, std::vector<Episode>& out, std::vector<Item>* shows = nullptr, std::string* next = nullptr);
Episode episodeOf(const Item& item);  // a home shelf's card or a mixed list's row
// What YouTube says of these now: their Played marks (and percents) agreed with the Y1's (both ways)
void noteEpisodes(const std::vector<Episode>& list);

// ---- what the screens do ----
// Resume, or Start: as spoken word, the music waiting. One YouTube says was started (and the Y1 never
// played) starts at about YouTube's place.
void playEpisode(const Episode& e);
void playEpisodeFrom(const Episode& e, int64_t ms);
// Play next / Add to queue: Episodes up next, after the episode or book playing; while music plays,
// after the song playing; with nothing playing, at once
void queueEpisode(const Episode& e, bool next);
std::vector<Episode> episodesUpNext();
void removeFromUpNext(const std::string& id);
void markEpisodePlayed(const Episode& e, bool played);
// the account's (false in done: it didn't go through)
void setEpisodeLater(const Episode& e, bool later, std::function<void(bool ok)> done);
void saveShow(const std::string& showId, bool saved, std::function<void(bool ok)> done);
// what a row says: "6d ago · 1 h 20 min left", "Played"; with its show ("Huberman Lab · …") in mixed lists
std::string episodeSubtitle(const Episode& e, bool withShow);
bool episodeNew(const Episode& e);  // the dot: not started here, nor heard on YouTube
// the started ones not finished, the last played first (Continue listening)
std::vector<Episode> episodesInProgress();
// a list's GETs are asked fresh for a little while after the account changed (YouTube shows a change late)
int podcastCacheMs();

// ---- the screens (podcast_screens.cpp) ----
void openPodcast(const std::string& showId, const std::string& title);  // a show
void openEpisodeMenu(const Episode& e, bool onItsShow = false);
std::vector<Row> episodeRows(const std::vector<Episode>& list, bool withShow);
// YouTube Music's Podcasts pill: its rows from GET /music/podcasts (status 0 or the body), or without an
// account (signedIn false); problem: what to say when it couldn't load
std::vector<Row> podcastsTabRows(bool signedIn, int status, const std::string& body, const std::string& problem, std::function<void()> retry);
std::unique_ptr<Screen> makePodcastSearch(const std::string& query = "");  // music_screens.cpp: shows and episodes
std::unique_ptr<Screen> makeEpisodesUpNext();
std::unique_ptr<Screen> makePodcastDownloads();  // Downloaded: on the card, and on their way

// ---- downloads (downloads.cpp's queue, the song's way): Podcasts/<show>/<title> [<id>].m4a ----
std::string podcastDownloadDir();
bool downloadEpisode(const Episode& e);
std::string downloadedEpisode(const std::string& id);  // its file ("" none)
std::vector<Episode> downloadedEpisodes();             // the newest first
bool deleteEpisodeDownload(const std::string& id);
void episodeDownloaded(const std::string& id, const std::string& file);  // downloads.cpp, once it's saved
bool episodeDownloadInfo(const std::string& id, Episode& out);           // what it was asked with

// ---- the loop's ----
bool podcastsTick(int64_t now);  // true: something shown changed
void podcastsSaveNow();

// ---- the dashboard's (dashboard_server.cpp) ----
std::string episodeJson(const Episode& e);  // with the Y1's place and marks
std::string podcastsStateJson();            // Continue listening, Episodes up next, downloaded
// a list from the source (GET /music/podcast…, a search) as the dashboard shows it: its head as it came,
// each episode with the Y1's place and marks; showId: a show's page (its episodes name it, its speed)
std::string podcastListJson(const std::string& sourceBody, const std::string& showId = "");
// an episode the dashboard names: what the Y1 keeps of it, else what the page sent
Episode episodeFromFields(const std::string& id, const std::string& title, const std::string& show, const std::string& showId,
                          const std::string& thumb, int64_t lengthMs, int progress);

}  // namespace rondo::shell
