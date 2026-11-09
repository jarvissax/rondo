// The screens other screens open.
#pragma once
#include <memory>

#include "../core/api.h"
#include "../core/version.h"
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
void rememberStream(const std::string& path, const Item& item);  // a restored queue's streamed song
// resume.cpp: the queue kept across a restart (saved from the loop, put back at startup)
void resumeTick(int64_t now);
void resumeSaveNow();  // on the way out
void resumeRestore();
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
// Power (power_screens.cpp): the Power screen, and the screen's power for the loop: an input event passes
// (true) or is used up (waking, swallowed); the timers, wakelock and battery (true: draw a frame); when
// they next need the loop; whether frames are drawn now; a wake waiting for its first frame; a frame
// presented (the backlight after a wake). g_onSleep: the screen went off (the loop lets go of holds).
std::unique_ptr<Screen> makePower();
bool powerFilter(const InputEvent& ev);
bool powerTick(int64_t now);
int64_t powerWakeAt();
bool screenDrawing();
bool powerWantsFrame();
void framePresented();
extern std::function<void()> g_onSleep;
std::unique_ptr<Screen> makeFm();
std::string fmName(int frequency);
void fmTick();
std::unique_ptr<Screen> makeBluetooth();
// Settings › Server, its row's state ("OK · signed in", "Unreachable"…), and the main menu's account check
std::unique_ptr<Screen> makeServer();
// A list to pick one of (name, value): the current one says so, a pick goes back
std::unique_ptr<Screen> makeChoice(std::string title, std::vector<std::pair<std::string, std::string>> choices,
                                   std::function<std::string()> current, std::function<void(const std::string&)> pick,
                                   std::function<std::string(const std::string&)> note = nullptr);
// Settings › Sound, and what it sends the player (from the loop: the output in use changing)
std::unique_ptr<Screen> makeSound();
void soundTick();
std::string soundState();  // Settings' row: the preset for the output in use
std::unique_ptr<Screen> makeDashboard();  // Settings › Dashboard
extern std::string g_dashboardAssets;     // <dir>/dashboard and <dir>/fonts
void serverState(std::function<void(const std::string&)> done);
void checkAccounts();
extern std::function<void()> g_onServerChanged;  // the server's address or token changed: everything that talks to it
// Software update (SoftwareUpdate.kt): this Y1's firmware number, GET /update's version, and which is newer
constexpr const char* kVersion = kRondoVersion;  // core/version.h (tools/set-version.sh)
int firmwareLevel();
std::unique_ptr<Screen> makeSoftwareUpdate();  // update_screen.cpp
std::string bytes(uint64_t n);                  // "12 MB" (SettingsScreen.kt's bytes())
// Rondo ends to start again (an update put in place): init runs it anew
void requestRestart();
// power_screens.cpp: the screen on (a warning to read); the shutdown with its message
void wakeScreen();
void shutDownNow(const char* why);
// safety_screens.cpp: MediaTek's thermal and battery warnings without Android (firmware: register for
// the kernel's thermal shutdown signal); from the loop, true when a screen changed
void safetyStart(bool firmware);
bool safetyTick(int64_t now);
// the firmware's Rondo, which installs bundles (a development run doesn't)
bool updatesAvailable();
std::string updateVersion(const std::string& json);
bool isNewerVersion(const std::string& latest, const std::string& current);
std::string bluetoothState();
void bluetoothTick();
// From the loop: offline pauses a streamed song, online resumes it; a failed stream is tried again
void streamsNetwork(bool online);
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
// The full form: a note under the field, how the typed text is shown (a code as 123 456), and an answer
// that may come later: onDone(text, answer) calls answer("") to take it or answer(problem).
using TextAnswer = std::function<void(const std::string& text, std::function<void(const std::string& problem)> answer)>;
std::unique_ptr<Screen> makeTextEntry(std::string title, std::string initial, std::string action, std::string note,
                                      size_t maxLength, std::function<std::string(const std::string&)> shown,
                                      TextAnswer onDone, std::function<void(const std::string&)> then);

// 1.0.x's local Item for a file: its title and "artist · album" (LocalLibrary.item), and the subtitle's
// first part (the artist) and the rest (the album).
struct SongInfo {
  std::string title, subtitle;
};
SongInfo songInfo(const std::string& path);
std::pair<std::string, std::string> firstAndRest(const std::string& subtitle);
// ArtColor.dominant(): the artwork's average color from a 12 x 12 sample, more saturated, of medium
// brightness, from the picture decoded at sourcePx (the view showing it); false while it loads.
// sourceH: a picture decoded at sourcePx x sourceH (0: square), as the row that shows it decodes it
bool artColor(Env& e, const std::string& path, int sourcePx, Color* out, int sourceH = 0);
// StatusIcons: Wi-Fi (dimmed offline), the percentage and the battery, ending at `right` in a row
// rowH tall; returns its left edge.
// hidePercent: a header that runs out of room (YouTube's three pills) keeps the battery only
float statusIcons(Env& e, float right, int rowH, Color c, bool showWifi = true, bool hidePercent = false);
float statusIconsWidth(Env& e, bool showWifi);

}  // namespace rondo::shell

namespace rondo::shell {
// The dashboard's hooks into the screens' logic (on the UI thread): the radio as FmRadio's calls,
// Bluetooth's names, the Power screen's shut down and restart.
void radioStart();
void radioTune(int frequency);
void radioSeek(bool up);
void radioScan();
void radioSetBand(const std::string& band);
void radioSetSpeaker(bool on);
int radioMin();
int radioMax();
std::string radioHeardName(int frequency);
void radioSaved();  // config.json now (presets changed)
std::string btName(const BtDevice& d);
std::string btKind(uint32_t cod);
bool btIsAudio(uint32_t cod);
void powerAction(bool restart);
}  // namespace rondo::shell
