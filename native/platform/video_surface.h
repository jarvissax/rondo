// Where video pictures arrive (the PlayerView surface of 1.0.x's VideoPlayerScreen). The drawing process
// owns it: a SurfaceTexture on one of its GL textures, published as the binder service "rondo.video".
// rondo-player's hardware decoder renders into it from the other side, and the GPU samples each picture
// as the decoder left it (an external texture, no copy). S2 proved the path with SurfaceFlinger stopped.
#pragma once

namespace rondo {

constexpr const char* kVideoService = "rondo.video";

class VideoSurface {
 public:
  // The surface, made and published the first time: on the drawing thread, with its GL context current.
  // nullptr when it can't be made.
  static VideoSurface* get();
  static VideoSurface* made();  // the surface if get() has made it, else nullptr (never makes it)
  virtual ~VideoSurface() {}

  // Readable when the decoder has sent a picture: poll it with the input.
  virtual int fd() const = 0;
  // On the drawing thread: takes the newest picture that arrived. True when the picture changed.
  virtual bool latch() = 0;
  // A picture has been latched since clear(): a video that is starting shows black until then.
  virtual bool hasPicture() const = 0;
  virtual void clear() = 0;
  virtual unsigned latches() const = 0;  // pictures taken so far (a later one: a newer picture)

  virtual unsigned texture() const = 0;       // GL_TEXTURE_EXTERNAL_OES
  virtual const float* transform() const = 0;  // the picture's texture transform, 4x4 column-major
  virtual int width() const = 0;               // the picture's size, without the decoder's padding
  virtual int height() const = 0;
};

}  // namespace rondo
