// Podcasts from RSS feeds: the main menu's Podcasts (rss_screens.cpp), in the iPod's light look, apart
// from YouTube Music's own podcasts (podcasts.h). Shows come from Apple's directory (and Podcast Index
// with a key), Apple's charts, a feed's address or an OPML file (the dashboard); the Y1 reads their
// feeds and streams their episodes from the podcast hosts itself (direct/rss.h, in both modes).
//
// Episodes play as spoken word through books.h's session, as audiobooks and YouTube Music's episodes
// do: their place kept (a little earlier after a pause), their show's speed, chapters (Podcasting 2.0
// chapter files, the MP3's own) or 30 s and 15 s skips, bookmarks, the sleep timer, and the music
// waiting. A video episode opens in the video player and keeps its place too; Listen only plays its
// sound as spoken word. Played marks are the Y1's own (a feed has none).
//
// New episodes: what a show put out since it was subscribed to (then, only its newest), not finished,
// for 30 days. Looked for when Podcasts opens and every 4 hours while the Y1 is awake, on Wi-Fi; a
// show's "Download new episodes" keeps its latest few on the card (Podcasts/<show>/, beside YouTube
// Music's), and can delete them once played. Nothing downloads unless that's on or asked for.
//
//   rss.json   beside prefs.json: the shows subscribed to (their settings and latest episodes), the
//              episodes played, queued, saved for later or on the card, Up next, Save for later, the
//              country. Metadata only; places, speeds and bookmarks are the library's (books.h).
#pragma once
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/books.h"
#include "app.h"

namespace rondo::shell {

struct BookPlay;

struct RssEpisode {
  std::string id, show, guid, title, url, type, image, chapters;  // show: its show's id
  std::string showTitle;  // what the lists say it's from
  int64_t date = 0;       // unix s (0 unknown)
  int duration = 0;       // s (0 unknown)
  int64_t size = 0;       // bytes, as the feed says
  int season = 0, number = 0;
  std::string episodeType;  // "", "trailer", "bonus"
  bool video() const { return type.compare(0, 6, "video/") == 0; }
};

struct RssShow {
  std::string id, feed, title, author, art, description, link, genre;
  bool serial = false;
  bool priv = false;  // added by its address (a private or paid feed): only ever read by the Y1 itself
  int64_t count = 0, updated = 0, apple = 0;
};

// ---- the spoken-word session's (books.cpp) ----
bool isRssKey(const std::string& key);  // "rss:<id>"
std::string rssKey(const std::string& episodeId);
std::string rssIdOf(const std::string& key);
std::string rssShowSpeedKey(const std::string& showId);  // "rssshow:<id>": a show's speed, for all its episodes
std::string rssSpeedKey(const std::string& key);
bool rssDetails(const std::string& key, BookPlay& out);  // its file or stream, length, chapters
void rssFinishedChanged(const std::string& key, bool finished);  // out of New episodes; deleted when played (a show's choice)
bool rssEnded(const std::string& key);  // the next of Up next started (true)
void rssRestored(const std::string& key, const std::string& path);  // a streamed one back after a restart
// an episode's file from its host, through the local server: the player's and a download's path (the
// player keeps 511 bytes of a path: a long link is looked up by the local server in the show's feed)
std::string rssStreamPath(const RssEpisode& e);
bool isRssStream(const std::string& path);
bool rssUpNextWaiting();

// ---- reading the local server's answers (direct/rss.h) ----
bool parseRssFeed(const std::string& json, RssShow& show, std::vector<RssEpisode>& eps);
std::vector<RssShow> parseRssShows(const std::string& json);  // a search's, the charts'
// newest first; a story show ("serial") with its first episode first
void sortRssEpisodes(std::vector<RssEpisode>& eps, bool serial);
// urls false: the episodes without their links (a show's page: playing one finds its link by its id)
std::string rssFeedPath(const RssShow& s, bool fresh = false, int64_t since = 0, bool urls = true);
// a picture through the local server, w x h: the screens' thumb
std::string rssArt(const std::string& url);

// ---- subscriptions ----
std::vector<RssShow> rssShows();  // subscribed: the latest updated first
bool rssSubscribed(const std::string& showId);
// eps: the show's episodes now (its newest counts as new; the rest, from before, don't)
void rssSubscribe(const RssShow& s, const std::vector<RssEpisode>& eps);
void rssUnsubscribe(const std::string& showId, bool deleteDownloads);
const RssShow* rssShow(const std::string& showId);  // subscribed, or known from an episode kept
// a show's page was read: what's kept of it brought up to date (its title, art, latest episodes)
void rssNoteFeed(const RssShow& s, const std::vector<RssEpisode>& eps);
// a show by its feed's address (the dashboard, an OPML file): read, then subscribed; done(ok, its
// title or why not)
void rssAddFeed(const std::string& url, bool priv, std::function<void(bool ok, const std::string& what)> done);
// OPML: the shows subscribed to, as other podcast apps read them; and a file's shows subscribed to
// (done(added, already, failed) once every feed was tried)
std::string rssOpml();
int rssImportOpml(const std::string& opml, std::function<void(int added, int already, int failed)> done);

// "Download new episodes", Wi-Fi only: the latest `keep` on the card; played ones deleted if chosen
struct RssAuto {
  bool on = false;
  int keep = 3;
  bool deletePlayed = false;
};
RssAuto rssAuto(const std::string& showId);
void setRssAuto(const std::string& showId, const RssAuto& a);
constexpr int kRssKeeps[] = {1, 2, 3, 5, 10};

// ---- the lists ----
std::vector<RssEpisode> rssNewEpisodes();            // newest first
int rssNewCount(const std::string& showId = "");     // one show's, or all
std::vector<RssEpisode> rssInProgress();              // Continue listening: the last played first
std::vector<RssEpisode> rssLater();                   // Save for later: the last saved first
bool rssIsLater(const std::string& id);
void setRssLater(const RssEpisode& e, bool later);
std::vector<RssEpisode> rssDownloaded();              // newest first
std::vector<RssEpisode> rssUpNext();
void rssRemoveFromUpNext(const std::string& id);
// the country whose directory and charts are asked ("us" unless chosen)
std::string rssCountry();
void setRssCountry(const std::string& code);
extern const char* const kRssCountries[];  // Apple's podcast countries (ISO codes), by code
extern const int kRssCountryCount;
std::string rssCountryName(const std::string& code);  // in the Rondo's language
// Apple's podcast genres for Top shows: their id and name (0: all)
struct RssGenre {
  int id;
  const char* key;
};
extern const RssGenre kRssGenres[];
extern const int kRssGenreCount;

// ---- what the screens do ----
void playRss(const RssEpisode& e);  // Resume or Start (a video: the video player, where it was)
void playRssFrom(const RssEpisode& e, int64_t ms);
void listenRss(const RssEpisode& e);  // a video episode's sound only, as spoken word
void queueRss(const RssEpisode& e, bool next);  // Play next / Add to queue
void markRssPlayed(const RssEpisode& e, bool played);
std::string rssSubtitle(const RssEpisode& e, bool withShow);  // "Nov 22 · 1 h 20 min left", "Played"
bool rssNew(const RssEpisode& e);  // the dot: never started here
bool rssStarted(const RssEpisode& e, bool* finished = nullptr);
const RssEpisode* rssKept(const std::string& id);
void rssKeep(const RssEpisode& e);  // what's kept of an episode (played, queued, saved, downloaded)
// its show notes as plain text (asked of the local server, which kept them with the feed)
void rssNotes(const RssEpisode& e, std::function<void(const std::string& notes)> done);
// a video episode in the video player keeps its place there (video_player.cpp)
void rssVideoPlace(const std::string& key, int64_t ms, int64_t durationMs, bool ended);
int64_t rssVideoStart(const std::string& key);  // where it opens

// ---- downloads (downloads.cpp's queue): Podcasts/<show>/<date> <title>.<ext> ----
bool downloadRss(const RssEpisode& e, bool quiet = false, bool automatic = false);
std::string downloadedRss(const std::string& id);  // its file ("" none)
bool deleteRssDownload(const std::string& id);
void rssDownloaded(const std::string& id, const std::string& file);  // downloads.cpp, once it's saved
std::string rssDownloadFile(const std::string& id);  // where it goes (its extension from its type)
bool rssDownloadInfo(const std::string& id, std::string& url, std::string& title, std::string& show, std::string& thumb);

// ---- new episodes ----
// looked for now (Podcasts opening: at most every 15 min) or when 4 hours have passed (the loop)
void rssCheck(bool opening);
void rssCheckNow();  // Podcast settings: whenever it was last done
bool rssChecking();
int64_t rssCheckedAt();  // ms since 1970, the last whole check (0 never)

// ---- the loop's ----
bool rssTick(int64_t now);  // true: something shown changed
void rssSaveNow();
uint32_t rssSerial();  // moves on with every change a screen shows

// ---- the screens (rss_screens.cpp) ----
std::unique_ptr<Screen> makeRssHome();  // the main menu's Podcasts
void openRssShow(const RssShow& s);
void openRssEpisodeMenu(const RssEpisode& e, bool onItsShow = false);
std::unique_ptr<Screen> makeRssUpNext();
std::unique_ptr<Screen> makeRssDownloaded();  // Downloaded: on the card, and on their way
std::unique_ptr<Screen> makeRssSettings();  // Podcasts › Podcast settings

// ---- the dashboard's (dashboard_server.cpp) ----
std::string rssEpisodeJson(const RssEpisode& e);
std::string rssShowJson(const RssShow& s);
std::string rssStateJson();  // subscriptions, New, Continue, Later, Up next, Downloaded, the country, checks
RssEpisode rssEpisodeFromJson(const std::string& json);  // what the page sends back of an episode
RssShow rssShowFromJson(const std::string& json);

}  // namespace rondo::shell
