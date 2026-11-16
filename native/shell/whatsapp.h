// WhatsApp on the Y1, through the Rondo server (server/src/whatsapp.ts, the /wa/ routes): the server is
// one of the owner's WhatsApp linked devices; the Y1 reads and sends through it. No notifications:
// what came is seen when WhatsApp is opened (decided Nov 15).
//
// WaClient keeps what the screens show: the chats, each opened chat's messages, the contacts. While a
// WhatsApp screen is open it follows the server's changes (a long poll of /wa/updates). A copy is kept
// on the card, hidden (/storage/sdcard0/.rondo/whatsapp): the chats and recent messages open at once,
// and the photos, videos and voice messages opened stay there too, up to 1 GB, the oldest going first.
#pragma once
#include <stdint.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rondo::shell {

struct WaPreview {
  std::string text, sender, status;  // sender: in a group, who wrote it ("" yourself)
  bool fromMe = false;
  std::string kind, caption;  // what it is (voice, image, video…), a photo's or video's words
  int seconds = 0;            // a voice message's or a video's length
};
struct WaChat {
  std::string jid, name;
  bool group = false, pinned = false, archived = false, hasPreview = false;
  int64_t at = 0;  // ms
  int unread = 0;
  WaPreview preview;
  std::string raw;  // its JSON as the server sent it (the card's copy)
};
struct WaQuote {
  std::string id, sender, name, kind, text;
};
struct WaReaction {
  std::string emoji;
  int count = 0;
};
struct WaMessage {
  std::string id, sender, name, kind, text, status;
  bool fromMe = false, edited = false, preview = false, quoted = false;
  int64_t at = 0, seq = 0;
  // media: its type, seconds (voice, video), size in pixels, bytes, a document's name
  std::string mime, fileName;
  int seconds = 0, width = 0, height = 0;
  int64_t size = 0;
  bool gif = false;
  WaQuote quote;
  std::vector<WaReaction> reactions;
  // on the Y1 only: being sent (an id of our own until the server answers), or not sent
  bool sending = false, failed = false;
  std::string localFile;  // what was sent from the card (sending again uses it)
  std::string mine;       // my own reaction ("" none)
  std::string raw;
};
struct WaContact {
  std::string jid, name, number, about, pushName;
  bool saved = false, group = false, hasAbout = false;
};

class WaClient {
 public:
  // where things stand with the server: none connected, one before 3.3.0, WhatsApp not linked yet (on
  // the server's admin page), linking, connecting, open; Unknown before the first answer, Offline when
  // it can't be reached
  enum class State { Unknown, NoServer, Old, Unlinked, Linking, Connecting, Open, Offline };
  State state() const { return state_; }
  std::string problem() const { return problem_; }  // why it couldn't load ("" none)
  // the screens open (each holds one): the first starts following the server, the last stops it
  void hold();
  void release();
  // moves on whenever anything shown changed: the screens draw again
  uint32_t serial() const { return serial_; }
  bool loaded() const { return chatsLoaded_; }  // the chats came (from the server or the card)
  const std::vector<WaChat>& chats() const { return chats_; }
  const WaChat* chat(const std::string& jid) const;
  // a chat's messages, oldest first ("" not loaded yet: open() loads them)
  const std::vector<WaMessage>* messages(const std::string& jid) const;
  bool olderToCome(const std::string& jid) const;
  bool loadingOlder(const std::string& jid) const;
  // a chat on screen: its messages (the card's copy first), marked read, kept read while it's open
  void open(const std::string& jid);
  void close(const std::string& jid);
  void older(const std::string& jid);
  // sending: text (an answer to quoted, "" none), a voice message (a WAV file, removed once sent), a
  // photo or a video from the card
  void sendText(const std::string& jid, const std::string& text, const std::string& quoted);
  void sendVoice(const std::string& jid, const std::string& wav, int seconds);
  void sendMedia(const std::string& jid, const std::string& file, bool video);
  void retry(const std::string& jid, const std::string& id);  // a message that wasn't sent
  // my reaction to a message: an emoji, or "" to take it back (shown at once, undone if it didn't go)
  void react(const std::string& jid, const std::string& id, const std::string& emoji);
  // the contacts (loaded once a session), and one's profile with "about"
  void loadContacts(std::function<void()> done);
  const std::vector<WaContact>& contacts() const { return contacts_; }
  bool contactsLoaded() const { return contactsLoaded_; }
  void profile(const std::string& jid, std::function<void(const WaContact&, bool ok)> done);
  // a message's media on the card (photo, video, voice): there already, or fetched first; "" when it
  // couldn't be (problem says why)
  void media(const std::string& jid, const WaMessage& m, std::function<void(const std::string& file, const std::string& problem)> done);
  std::string mediaFileIfKept(const std::string& jid, const WaMessage& m) const;
  bool fetching(const std::string& jid, const std::string& id) const;
  // the pictures the image loader asks the server for (ui/image.cpp: "/wa/" paths, w and h added)
  static std::string avatarPath(const std::string& jid);
  static std::string previewPath(const std::string& jid, const WaMessage& m);
  // what a message is in a line ("Photo", "Voice message 0:12", its text)
  static std::string summary(const WaMessage& m);
  void serverChanged();  // another server (or none): everything known goes

 private:
  void changed() { serial_++; }
  void loadStatus();
  void loadChats();
  void poll();
  void applyUpdates(const std::string& body);
  void putMessages(const std::string& jid, std::vector<WaMessage> msgs, bool replaceAll);
  void upsert(const std::string& jid, WaMessage m);
  void markRead(const std::string& jid);
  void sendFile(const std::string& jid, const std::string& path, const std::string& file, const std::string& type, WaMessage local);
  void sent(const std::string& jid, const std::string& localId, int status, const std::string& body);
  void saveSoon();
  void saveNow();
  void loadCard();
  void sortChats();

  State state_ = State::Unknown;
  std::string problem_;
  uint32_t serial_ = 1;
  int holds_ = 0;
  int64_t seq_ = 0;
  bool chatsLoaded_ = false, polling_ = false, cardLoaded_ = false;
  uint32_t pollId_ = 0, generation_ = 0;
  std::vector<WaChat> chats_;
  struct Thread {
    std::vector<WaMessage> msgs;
    bool loaded = false, complete = false, loadingOlder = false;
    int open = 0;
  };
  std::map<std::string, Thread> threads_;
  std::vector<WaContact> contacts_;
  bool contactsLoaded_ = false, contactsLoading_ = false;
  std::vector<std::function<void()>> contactsWaiting_;
  std::map<std::string, std::vector<std::function<void(const std::string&, const std::string&)>>> fetching_;
  int localIds_ = 0;
  bool saveQueued_ = false;
};

WaClient& waClient();

// The screens (whatsapp_screens.cpp): the chats; a chat; the main menu's tile
std::unique_ptr<class Screen> makeWhatsApp();

}  // namespace rondo::shell
