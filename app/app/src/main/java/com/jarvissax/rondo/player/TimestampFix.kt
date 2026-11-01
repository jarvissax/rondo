package com.jarvissax.rondo.player

import android.content.Context
import android.media.MediaCodec
import android.os.Handler
import android.util.Log
import com.google.android.exoplayer2.DefaultRenderersFactory
import com.google.android.exoplayer2.Renderer
import com.google.android.exoplayer2.decoder.CryptoInfo
import com.google.android.exoplayer2.mediacodec.DefaultMediaCodecAdapterFactory
import com.google.android.exoplayer2.mediacodec.MediaCodecAdapter
import com.google.android.exoplayer2.mediacodec.MediaCodecSelector
import com.google.android.exoplayer2.video.MediaCodecVideoRenderer
import com.google.android.exoplayer2.video.VideoRendererEventListener
import java.util.PriorityQueue

// The Y1's video decoder (OMX.MTK.VIDEO.DECODER.AVC through MediaCodec on Android 4.2) returns
// every decoded frame with presentationTimeUs = 0, whatever timestamp went in (measured
// 2026-10-30, see docs/device/README.md). ExoPlayer then judged every frame late, dropped ~90% of
// them and let the decoder race ahead of the audio: ~9 fps that looked sped up. Frames come out
// in display order, i.e. in ascending timestamp order, so the adapter remembers the timestamps
// it fed in and gives each output frame the smallest one still pending.
enum class TimestampMode { RAW, REORDER }

object TimestampFix {
    @Volatile var mode: TimestampMode = TimestampMode.REORDER
}

private const val MAX_PENDING = 64 // a frame the decoder silently drops must not shift every later one

class TimestampFixAdapter(private val delegate: MediaCodecAdapter) : MediaCodecAdapter by delegate {
    private val mode = TimestampFix.mode
    private val pending = PriorityQueue<Long>()
    private var loggedOut = 0

    private fun remember(presentationTimeUs: Long, size: Int, flags: Int) {
        val noOutput = MediaCodec.BUFFER_FLAG_CODEC_CONFIG or MediaCodec.BUFFER_FLAG_END_OF_STREAM
        if (mode == TimestampMode.RAW || size == 0 || flags and noOutput != 0) return
        pending.add(presentationTimeUs)
        if (pending.size > MAX_PENDING) {
            pending.poll()
            Log.w("Y1", "decoder timestamp queue overflow, dropped the oldest")
        }
    }

    override fun queueInputBuffer(index: Int, offset: Int, size: Int, presentationTimeUs: Long, flags: Int) {
        remember(presentationTimeUs, size, flags)
        delegate.queueInputBuffer(index, offset, size, presentationTimeUs, flags)
    }

    override fun queueSecureInputBuffer(index: Int, offset: Int, info: CryptoInfo, presentationTimeUs: Long, flags: Int) {
        remember(presentationTimeUs, 1, flags)
        delegate.queueSecureInputBuffer(index, offset, info, presentationTimeUs, flags)
    }

    override fun dequeueOutputBufferIndex(bufferInfo: MediaCodec.BufferInfo): Int {
        val index = delegate.dequeueOutputBufferIndex(bufferInfo)
        if (index < 0 || mode == TimestampMode.RAW || bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
            return index
        }
        val fromCodec = bufferInfo.presentationTimeUs
        val restored = if (fromCodec != 0L && pending.remove(fromCodec)) fromCodec else pending.poll() ?: fromCodec
        bufferInfo.presentationTimeUs = restored
        if (loggedOut++ < 3) Log.i("Y1", "decoder out: codec said $fromCodec, using $restored")
        return index
    }

    override fun flush() {
        delegate.flush()
        pending.clear()
    }
}

class TimestampFixAdapterFactory : MediaCodecAdapter.Factory {
    private val default = DefaultMediaCodecAdapterFactory()
    override fun createAdapter(configuration: MediaCodecAdapter.Configuration): MediaCodecAdapter =
        TimestampFixAdapter(default.createAdapter(configuration))
}

// Same renderers as ExoPlayer's default, except that the video decoder goes through the adapter.
class Y1RenderersFactory(context: Context) : DefaultRenderersFactory(context) {
    override fun buildVideoRenderers(
        context: Context,
        extensionRendererMode: Int,
        mediaCodecSelector: MediaCodecSelector,
        enableDecoderFallback: Boolean,
        eventHandler: Handler,
        eventListener: VideoRendererEventListener,
        allowedVideoJoiningTimeMs: Long,
        out: ArrayList<Renderer>,
    ) {
        out.add(
            MediaCodecVideoRenderer(
                context,
                TimestampFixAdapterFactory(),
                mediaCodecSelector,
                allowedVideoJoiningTimeMs,
                enableDecoderFallback,
                eventHandler,
                eventListener,
                MAX_DROPPED_VIDEO_FRAME_COUNT_TO_NOTIFY,
            ),
        )
    }
}
