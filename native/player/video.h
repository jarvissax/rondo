// The picture of a video (the video renderer of 1.0.x's ExoPlayer), in rondo-player. The server's
// /s/<id>/<itag> (one of YouTube's H.264 DASH streams, through HttpSource) or a file on the card is read
// ahead by FFmpeg on one thread; another feeds MediaTek's hardware decoder through the native MediaCodec
// and renders each picture into the drawing process's VideoSurface when the sound reaches it.
//
// The sound is the engine's: a video's audio (/s/<id>/140) plays exactly like a song, and the picture
// follows its clock, so it pauses, buffers and seeks with it.
//
// The clock runs on across repeats (Shorts loop, as does any video with Repeat on): each pass of the
// sound starts at a time the engine names with loop(), and the reader, at the stream's end, goes back
// to its start with the pictures' times moved on by as much, so the decoder never stops.
#pragma once
#include <pthread.h>
#include <stdint.h>

#include <atomic>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace rondo {

class VideoDecoder {
 public:
  // The sound's position in microseconds, and whether it is moving (playing).
  using Clock = std::function<int64_t(bool& moving)>;
  explicit VideoDecoder(Clock clock) : clock_(std::move(clock)) {}

  // A video from `atMs`: the first of `sources` that opens, each a URL (sent with the token) or a file.
  // Another quality is another play() from where the sound is.
  // gate: the sound waits for the first picture (a start); another quality doesn't hold the sound up.
  // baseUs: the clock's time at the stream's start (in a later pass of a repeat).
  void play(const std::vector<std::string>& sources, const std::string& token, int64_t atMs, bool gate = true,
            int64_t baseUs = 0);
  void seek(int64_t atMs);  // the sound jumped (its clock now reads atMs): it waits for the picture there
  void setLooping(bool on);   // the sound repeats: the stream's end is followed by its start
  void loop(int64_t baseUs);  // the sound's next pass begins at this time on the clock
  bool failed() const { return failed_.load(); }  // the picture can't be shown (the sound plays on)
  // The picture at the latest start or seek has been shown (or there is none to wait for): the sound
  // may play. ExoPlayer starts both together the same way.
  bool ready() const { return failed_.load() || shownSeek_.load() == latestSeek_.load(); }
  void stop();
  void setLatency(int64_t us) { latencyUs_ = us; }  // how long the sound takes to come out after the clock

  int height() const { return height_.load(); }        // the picture's height (0 until known)
  int64_t readMs() const { return readMs_.load(); }    // how far ahead the stream has been read

 private:
  struct Packet {
    std::vector<uint8_t> data;  // Annex-B
    int64_t ptsUs;
    bool eos;
  };
  struct Format {
    int width = 0, height = 0;
    std::vector<uint8_t> csd0, csd1;  // the SPS and PPS with start codes
  };
  static void* demuxThread(void* self);
  static void* renderThread(void* self);
  void demuxLoop();
  void renderLoop();
  void startThreads();  // under mu_

  Clock clock_;
  std::atomic<int64_t> latencyUs_{0};
  std::atomic<int> height_{0};
  std::atomic<int64_t> readMs_{0};
  std::atomic<uint32_t> latestSeek_{0}, shownSeek_{0};
  std::atomic<bool> failed_{false};
  bool catchUp_ = false;  // under mu_: the job started where the sound already is (another quality)

  pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;  // everything below
  pthread_cond_t cv_ = PTHREAD_COND_INITIALIZER;
  bool started_ = false;
  bool want_ = false;
  uint32_t job_ = 0;  // a new video, another quality or a stop
  std::vector<std::string> sources_;
  std::string token_;
  uint32_t seekGen_ = 0;  // a seek or a play: the packets after it start from seekMs_
  int64_t seekMs_ = 0;
  int64_t baseUs_ = 0;           // the clock at the stream's start for that seek
  std::deque<int64_t> loops_;    // the clock at the start of the passes to come
  bool looping_ = false;
  // from the demuxer to the renderer
  uint32_t formatJob_ = 0;  // the job `format_` belongs to
  Format format_;
  std::deque<Packet> packets_;
  size_t queuedBytes_ = 0;
  uint32_t queueSeek_ = 0;  // the seek the queued packets follow
};

}  // namespace rondo
