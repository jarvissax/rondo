package com.jarvissax.rondo.player

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import com.google.android.exoplayer2.ExoPlayer
import com.google.android.exoplayer2.Format
import com.google.android.exoplayer2.analytics.AnalyticsListener
import com.google.android.exoplayer2.decoder.DecoderReuseEvaluation
import com.google.android.exoplayer2.source.LoadEventInfo
import com.google.android.exoplayer2.source.MediaLoadData
import java.io.IOException

// Logs what the decoder actually does on the Y1: which codec, the format, and every 5 s the
// rendered/dropped/skipped frame counts, effective fps and playback speed against the wall clock.
class PlaybackStats(private val player: ExoPlayer) : AnalyticsListener {
    private val handler = Handler(Looper.getMainLooper())
    private var lastWall = 0L
    private var lastPos = 0L
    private var lastRendered = 0
    private var lastDropped = 0
    private var lastSkipped = 0
    private var lastQueued = 0
    private var lastDroppedInput = 0
    private var lastToKeyframe = 0

    private val tick = object : Runnable {
        override fun run() {
            report()
            handler.postDelayed(this, 5_000)
        }
    }

    init {
        handler.postDelayed(tick, 5_000)
    }

    private fun report() {
        if (!player.isPlaying) {
            lastWall = 0
            return
        }
        val c = player.videoDecoderCounters ?: return
        c.ensureUpdated()
        val wall = SystemClock.elapsedRealtime()
        val pos = player.currentPosition
        if (lastWall != 0L) {
            val dt = (wall - lastWall) / 1000.0
            val rendered = c.renderedOutputBufferCount - lastRendered
            Log.i(
                "Y1",
                String.format(
                    "stats: %.1f fps rendered, dropped %d, skipped %d, speed %.2fx, %dp",
                    rendered / dt,
                    c.droppedBufferCount - lastDropped,
                    c.skippedOutputBufferCount - lastSkipped,
                    (pos - lastPos) / 1000.0 / dt,
                    player.videoFormat?.height ?: 0,
                ),
            )
            Log.i(
                "Y1",
                "counters: queued ${c.queuedInputBufferCount - lastQueued}, dropped-input ${c.droppedInputBufferCount - lastDroppedInput}, " +
                    "drop-to-keyframe ${c.droppedToKeyframeCount - lastToKeyframe}, max-consecutive-dropped ${c.maxConsecutiveDroppedBufferCount}, " +
                    "late-offset avg ${if (c.videoFrameProcessingOffsetCount > 0) c.totalVideoFrameProcessingOffsetUs / c.videoFrameProcessingOffsetCount / 1000 else 0} ms",
            )
        }
        lastWall = wall
        lastPos = pos
        lastRendered = c.renderedOutputBufferCount
        lastDropped = c.droppedBufferCount
        lastSkipped = c.skippedOutputBufferCount
        lastQueued = c.queuedInputBufferCount
        lastDroppedInput = c.droppedInputBufferCount
        lastToKeyframe = c.droppedToKeyframeCount
    }

    override fun onVideoDecoderInitialized(
        eventTime: AnalyticsListener.EventTime,
        decoderName: String,
        initializedTimestampMs: Long,
        initializationDurationMs: Long,
    ) {
        Log.i("Y1", "video decoder $decoderName (init ${initializationDurationMs} ms)")
    }

    override fun onAudioDecoderInitialized(
        eventTime: AnalyticsListener.EventTime,
        decoderName: String,
        initializedTimestampMs: Long,
        initializationDurationMs: Long,
    ) {
        Log.i("Y1", "audio decoder $decoderName (init ${initializationDurationMs} ms)")
    }

    override fun onVideoInputFormatChanged(
        eventTime: AnalyticsListener.EventTime,
        format: Format,
        decoderReuseEvaluation: DecoderReuseEvaluation?,
    ) {
        Log.i("Y1", "video format ${format.width}x${format.height} ${format.frameRate} fps ${format.codecs} ${format.bitrate / 1000} kbps")
    }

    override fun onLoadError(
        eventTime: AnalyticsListener.EventTime,
        loadEventInfo: LoadEventInfo,
        mediaLoadData: MediaLoadData,
        error: IOException,
        wasCanceled: Boolean,
    ) {
        Log.w("Y1", "load error ${loadEventInfo.uri.path} (canceled=$wasCanceled): $error")
    }

    override fun onDroppedVideoFrames(eventTime: AnalyticsListener.EventTime, droppedFrames: Int, elapsedMs: Long) {
        Log.w("Y1", "dropped $droppedFrames video frames in ${elapsedMs} ms")
    }
}
