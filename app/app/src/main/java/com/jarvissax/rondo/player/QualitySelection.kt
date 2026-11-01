package com.jarvissax.rondo.player

import com.google.android.exoplayer2.C
import com.google.android.exoplayer2.source.TrackGroup
import com.google.android.exoplayer2.source.chunk.MediaChunk
import com.google.android.exoplayer2.trackselection.AdaptiveTrackSelection
import com.google.android.exoplayer2.upstream.BandwidthMeter
import com.google.android.exoplayer2.util.Clock
import com.google.common.collect.ImmutableList

// Quality switching without a pause. Re-running track selection (overrides, size constraints)
// makes ExoPlayer drop its video buffer: 1.8-3.7 s of buffering per switch on the Y1. Instead
// one adaptive selection over all renditions lives for the whole playback, and the preferred
// height is read from here each time the next segment is chosen. Already-buffered segments of
// the old quality more than KEEP_AHEAD_US ahead are discarded, so the change shows within a few
// seconds while the next ones load.
object QualityPreference {
    @Volatile var height: Int? = null // null = Auto
    const val AUTO_MAX_HEIGHT = 360 // the panel is 480x360; 360p (640x360) already covers it
}

private const val KEEP_AHEAD_US = 3_000_000L

class QualityTrackSelection(
    group: TrackGroup,
    tracks: IntArray,
    type: Int,
    bandwidthMeter: BandwidthMeter,
    checkpoints: List<AdaptationCheckpoint>,
) : AdaptiveTrackSelection(
    group,
    tracks,
    type,
    bandwidthMeter,
    DEFAULT_MIN_DURATION_FOR_QUALITY_INCREASE_MS.toLong(),
    DEFAULT_MAX_DURATION_FOR_QUALITY_DECREASE_MS.toLong(),
    DEFAULT_MIN_DURATION_TO_RETAIN_AFTER_DISCARD_MS.toLong(),
    DEFAULT_MAX_WIDTH_TO_DISCARD,
    DEFAULT_MAX_HEIGHT_TO_DISCARD,
    DEFAULT_BANDWIDTH_FRACTION,
    DEFAULT_BUFFERED_FRACTION_TO_LIVE_EDGE_FOR_QUALITY_INCREASE,
    checkpoints,
    Clock.DEFAULT,
) {
    private fun indexForHeight(height: Int): Int? = (0 until length()).firstOrNull { getFormat(it).height == height }

    // tracks are ordered by decreasing bitrate: the first one under the cap is the best allowed
    private fun autoIndex(): Int {
        val adaptive = super.getSelectedIndex()
        if (getFormat(adaptive).height <= QualityPreference.AUTO_MAX_HEIGHT) return adaptive
        return (0 until length()).firstOrNull { getFormat(it).height <= QualityPreference.AUTO_MAX_HEIGHT } ?: adaptive
    }

    override fun getSelectedIndex(): Int = QualityPreference.height?.let { indexForHeight(it) } ?: autoIndex()

    override fun getSelectionReason(): Int =
        if (QualityPreference.height != null) C.SELECTION_REASON_MANUAL else super.getSelectionReason()

    override fun evaluateQueueSize(playbackPositionUs: Long, queue: List<MediaChunk>): Int {
        val target = getSelectedFormat()
        val keepUntilUs = playbackPositionUs + KEEP_AHEAD_US
        for (i in queue.indices) {
            val chunk = queue[i]
            if (chunk.startTimeUs >= keepUntilUs && chunk.trackFormat.height != target.height) return i
        }
        return queue.size
    }
}

class QualityTrackSelectionFactory : AdaptiveTrackSelection.Factory() {
    override fun createAdaptiveTrackSelection(
        group: TrackGroup,
        tracks: IntArray,
        type: Int,
        bandwidthMeter: BandwidthMeter,
        adaptationCheckpoints: ImmutableList<AdaptiveTrackSelection.AdaptationCheckpoint>,
    ): AdaptiveTrackSelection =
        if (group.type == C.TRACK_TYPE_VIDEO) {
            QualityTrackSelection(group, tracks, type, bandwidthMeter, adaptationCheckpoints)
        } else {
            super.createAdaptiveTrackSelection(group, tracks, type, bandwidthMeter, adaptationCheckpoints)
        }
}
