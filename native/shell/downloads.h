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

struct DownloadJob {
  std::string id, title, channel, thumb;
  int quality = 480;  // the height asked for
  enum State { Waiting, Running, Done, Failed } state = Waiting;
  int percent = 0;
  std::string stage;  // "Picture", "Sound", "Subtitles", "Saving"
  std::string error, file;
};

// Queued (false: already on the card or in the queue); a toast says which
bool downloadVideo(const Item& item, int quality);
const std::vector<DownloadJob>& downloadJobs();  // this session's, the newest last
bool downloadsActive();  // one waiting or running: the CPU stays up with the screen off (power_screens.cpp)
void cancelDownload(const std::string& id);      // a waiting or running one; its parts deleted
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
// The queue and the files as JSON (the dashboard's GET /api/downloads)
std::string downloadsJson();

}  // namespace rondo::shell
