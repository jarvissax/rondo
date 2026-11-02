package com.jarvissax.rondo.player

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.ConnectivityManager
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Binder
import android.os.IBinder
import android.os.SystemClock
import android.support.v4.media.session.MediaSessionCompat
import android.util.Log
import android.view.KeyEvent
import androidx.core.app.NotificationCompat
import androidx.media.session.MediaButtonReceiver
import com.google.android.exoplayer2.C
import com.google.android.exoplayer2.DefaultLoadControl
import com.google.android.exoplayer2.ExoPlayer
import com.google.android.exoplayer2.MediaItem
import com.google.android.exoplayer2.PlaybackException
import com.google.android.exoplayer2.Player
import com.google.android.exoplayer2.audio.AudioAttributes
import com.google.android.exoplayer2.ext.mediasession.MediaSessionConnector
import com.google.android.exoplayer2.source.DefaultMediaSourceFactory
import com.google.android.exoplayer2.source.MediaSource
import com.google.android.exoplayer2.source.ProgressiveMediaSource
import com.google.android.exoplayer2.source.dash.DashMediaSource
import com.google.android.exoplayer2.trackselection.DefaultTrackSelector
import com.google.android.exoplayer2.upstream.DefaultDataSource
import com.google.android.exoplayer2.upstream.DefaultHttpDataSource
import com.google.android.exoplayer2.util.MimeTypes
import java.io.File
import com.jarvissax.rondo.Config
import com.jarvissax.rondo.Prefs
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.net.FastRetryLoadErrorPolicy
import com.jarvissax.rondo.net.Item

private const val TAG = "Y1"
private const val NOTIFICATION_ID = 1
private const val CHANNEL_ID = "playback"

// Owns playback: survives screen-off and the Activity going away, answers headset/Bluetooth
// buttons through a media session, and recovers from expired upstream URLs and Wi-Fi drops.
// Two engines behind one surface: ExoPlayer for everything streamed and for videos on the card,
// LocalAudioEngine (platform MediaPlayer) for music on the card. An Item whose id is a file path
// ("/storage/...") is local.
class PlayerService : Service() {
    // PROGRESSIVE = itag 18 (360p Baseline H.264 + AAC in one file), the decoder comparison case
    enum class Mode { VIDEO, AUDIO, PROGRESSIVE }

    inner class LocalBinder : Binder() {
        val service: PlayerService get() = this@PlayerService
    }

    lateinit var player: ExoPlayer
        private set
    lateinit var trackSelector: DefaultTrackSelector
        private set
    private lateinit var local: LocalAudioEngine

    private lateinit var session: MediaSessionCompat
    private lateinit var connector: MediaSessionConnector
    private lateinit var wifiLock: WifiManager.WifiLock

    private var currentId: String? = null
    private var pausedForNetwork = false
    private var foreground = false
    private val recoveries = ArrayDeque<Long>() // timestamps of recent automatic rebuilds

    // a new server or token from the dashboard: later requests use it
    private val serverChanged: (Config.Change) -> Unit = { change ->
        if (change == Config.Change.SERVER) {
            dataSourceFactory.setDefaultRequestProperties(mapOf("Authorization" to Api.authHeader))
            Api.clearCache()
        }
    }

    private val dataSourceFactory = DefaultHttpDataSource.Factory()
        .setDefaultRequestProperties(mapOf("Authorization" to Api.authHeader))
        .setUserAgent(Api.USER_AGENT + " ExoPlayer/2.19.1")
        .setConnectTimeoutMs(15_000)
        .setReadTimeoutMs(15_000)

    // rejected connects are retried every 250 ms (see net/ConnectRetry.kt), other errors as ExoPlayer does
    private val loadErrorPolicy = FastRetryLoadErrorPolicy()

    override fun onCreate() {
        super.onCreate()
        Config.listen(serverChanged)
        FmRadio.listen(radioChanged)
        // all renditions stay in one adaptive selection (no viewport cap): see QualitySelection.kt
        QualityPreference.height = Prefs.videoQuality(this)
        trackSelector = DefaultTrackSelector(this, QualityTrackSelectionFactory()).apply {
            setParameters(buildUponParameters().clearViewportSizeConstraints())
        }
        // Battery: the Wi-Fi radio is the big cost while streaming, and it can only doze between
        // downloads. Buffer up to 5 minutes (a song is ~5 MB at 128k) or 12 MB (~2 minutes of
        // 360p video), then let the radio rest until 15 s are left.
        val loadControl = DefaultLoadControl.Builder()
            .setBufferDurationsMs(15_000, 300_000, 2_500, 5_000)
            .setTargetBufferBytes(12 * 1024 * 1024)
            .setPrioritizeTimeOverSizeThresholds(false)
            .build()
        player = ExoPlayer.Builder(this, Y1RenderersFactory(this))
            // items added later (Play next / Add to queue) get the same authenticated data source
            .setMediaSourceFactory(DefaultMediaSourceFactory(dataSourceFactory).setLoadErrorHandlingPolicy(loadErrorPolicy))
            .setTrackSelector(trackSelector)
            .setLoadControl(loadControl)
            // CPU lock only; the Wi-Fi lock is ours (WIFI_MODE_FULL lets the radio power-save
            // between bursts, ExoPlayer's would be HIGH_PERF), held only while streaming
            .setWakeMode(C.WAKE_MODE_LOCAL)
            .setAudioAttributes(
                AudioAttributes.Builder().setUsage(C.USAGE_MEDIA).setContentType(C.AUDIO_CONTENT_TYPE_MUSIC).build(),
                true, // handle audio focus
            )
            .setHandleAudioBecomingNoisy(true) // headphones unplugged -> pause
            .build()
        player.addListener(playerListener)
        player.addAnalyticsListener(PlaybackStats(player))
        local = LocalAudioEngine(this) { changed() }

        @Suppress("DEPRECATION")
        wifiLock = (applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager)
            .createWifiLock(WifiManager.WIFI_MODE_FULL, "Y1:stream")
            .apply { setReferenceCounted(false) }

        session = MediaSessionCompat(this, "Y1").apply { isActive = true }
        connector = MediaSessionConnector(session).apply {
            setPlayer(player)
            // headset / Bluetooth / screen-off buttons drive whichever engine is playing
            setMediaButtonEventHandler { _, intent -> onMediaButton(intent) }
        }

        @Suppress("DEPRECATION")
        registerReceiver(connectivityReceiver, IntentFilter(ConnectivityManager.CONNECTIVITY_ACTION))
        Log.i(TAG, "PlayerService created")
    }

    override fun onBind(intent: Intent?): IBinder = LocalBinder()

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        MediaButtonReceiver.handleIntent(session, intent)
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        FmRadio.unlisten(radioChanged)
        Config.unlisten(serverChanged)
        unregisterReceiver(connectivityReceiver)
        connector.setPlayer(null)
        session.release()
        local.stop()
        player.release()
        if (wifiLock.isHeld) wifiLock.release()
        Log.i(TAG, "PlayerService destroyed")
        super.onDestroy()
    }

    private fun onMediaButton(intent: Intent): Boolean {
        @Suppress("DEPRECATION")
        val key = intent.getParcelableExtra<KeyEvent>(Intent.EXTRA_KEY_EVENT) ?: return false
        if (key.action != KeyEvent.ACTION_DOWN || key.repeatCount > 0) return true
        when (key.keyCode) {
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, KeyEvent.KEYCODE_HEADSETHOOK -> togglePlay()
            KeyEvent.KEYCODE_MEDIA_PLAY -> if (!isPlaying()) togglePlay()
            KeyEvent.KEYCODE_MEDIA_PAUSE -> if (isPlaying()) togglePlay()
            KeyEvent.KEYCODE_MEDIA_NEXT -> next()
            KeyEvent.KEYCODE_MEDIA_PREVIOUS -> previous()
            else -> return false
        }
        return true
    }

    // the playback test screen (MainActivity): one stream by video ID
    fun play(id: String, mode: Mode, startMs: Long = 0) {
        local.stop()
        currentId = id
        isVideo = mode != Mode.AUDIO
        Log.i(TAG, "play $mode $id from ${startMs}ms")
        player.setMediaSource(buildSource(id, mode))
        if (startMs > 0) player.seekTo(startMs)
        player.prepare()
        player.playWhenReady = true
    }

    // --- what the shell shows and drives; each ExoPlayer MediaItem carries its Item as the tag ---

    var isVideo = false
        private set

    val isLocal: Boolean get() = local.active

    val current: Item?
        get() = if (local.active) local.item else player.currentMediaItem?.localConfiguration?.tag as? Item

    val upNext: Item?
        get() {
            if (local.active) return local.upNext
            val i = player.nextMediaItemIndex
            return if (i == C.INDEX_UNSET) null else player.getMediaItemAt(i).localConfiguration?.tag as? Item
        }

    fun queue(): List<Item> =
        if (local.active) local.queue
        else (0 until player.mediaItemCount).mapNotNull { player.getMediaItemAt(it).localConfiguration?.tag as? Item }

    fun currentIndex(): Int = if (local.active) local.index else player.currentMediaItemIndex

    fun isPlaying(): Boolean = if (local.active) local.isPlaying() else player.isPlaying
    fun isBuffering(): Boolean = if (local.active) local.isBuffering() else player.playbackState == Player.STATE_BUFFERING
    fun positionMs(): Long = if (local.active) local.position() else player.currentPosition

    fun durationMs(): Long {
        val d = if (local.active) local.duration() else player.duration.takeIf { it != C.TIME_UNSET } ?: 0L
        return if (d > 0) d else (current?.durationSec ?: 0) * 1000L
    }

    fun bufferedMs(): Long = if (local.active) durationMs() else player.bufferedPosition

    var shuffle: Boolean
        get() = if (local.active) local.shuffle else player.shuffleModeEnabled
        set(value) {
            local.shuffle = value
            player.shuffleModeEnabled = value
        }

    var repeat: Int
        get() = if (local.active) local.repeat else player.repeatMode
        set(value) {
            local.repeat = value
            player.repeatMode = value
        }

    fun playQueue(items: List<Item>, start: Int) {
        if (items.isEmpty()) return
        val first = items[start.coerceIn(0, items.lastIndex)]
        // one engine per queue: local songs with local songs, streamed with streamed
        val sameKind = items.filter { isFile(it) == isFile(first) }
        val index = sameKind.indexOf(first).coerceAtLeast(0)
        isVideo = false
        currentId = first.id
        Log.i(TAG, "play queue of ${sameKind.size} from #$index ${if (isFile(first)) "(card)" else first.id}")
        if (isFile(first)) {
            player.stop()
            player.clearMediaItems()
            local.play(sameKind, index)
        } else {
            local.stop()
            player.setMediaSources(sameKind.map { audioSource(it) }, index, 0)
            player.prepare()
            player.playWhenReady = true
        }
        changed()
    }

    fun playVideo(item: Item, startMs: Long = 0) {
        local.stop()
        isVideo = true
        currentId = item.id
        Log.i(TAG, "play video ${if (isFile(item)) "(card)" else item.id}")
        if (isFile(item)) {
            val media = MediaItem.Builder().setUri(Uri.fromFile(File(item.id))).setMediaId(item.id).setTag(item).build()
            player.setMediaSource(ProgressiveMediaSource.Factory(DefaultDataSource.Factory(this)).createMediaSource(media))
        } else {
            val media = MediaItem.Builder().setUri("${Api.base}/mpd/${item.id}").setMediaId(item.id).setTag(item).build()
            player.setMediaSource(DashMediaSource.Factory(dataSourceFactory).setLoadErrorHandlingPolicy(loadErrorPolicy).createMediaSource(media))
        }
        if (startMs > 0) player.seekTo(startMs)
        player.prepare()
        player.playWhenReady = true
    }

    fun stopVideo() {
        if (!isVideo) return
        player.stop()
        player.clearMediaItems()
        isVideo = false
        changed()
    }

    fun jumpTo(index: Int) {
        if (local.active) return local.jumpTo(index)
        if (index in 0 until player.mediaItemCount) {
            player.seekTo(index, 0)
            player.play()
        }
    }

    fun togglePlay() {
        if (local.active) return local.toggle()
        if (player.isPlaying) {
            player.pause()
        } else {
            if (player.playbackState == Player.STATE_IDLE) player.prepare()
            player.play()
        }
    }

    fun next() {
        if (local.active) local.next() else if (player.hasNextMediaItem()) player.seekToNextMediaItem()
    }

    // like an iPod: back to the start, or to the previous track in the first 3 s
    fun previous() {
        if (local.active) return local.previous()
        if (player.currentPosition > 3_000 || !player.hasPreviousMediaItem()) player.seekTo(0) else player.seekToPreviousMediaItem()
    }

    fun seekTo(ms: Long) {
        val d = durationMs()
        val target = if (d > 0) ms.coerceIn(0, maxOf(0, d - 1_000)) else ms.coerceAtLeast(0)
        if (local.active) local.seekTo(target) else player.seekTo(target)
    }

    fun seekBy(deltaMs: Long) = seekTo(positionMs() + deltaMs)

    // false when the song cannot join this queue (card songs and streamed songs are separate queues)
    fun playNext(item: Item): Boolean = enqueue(item, next = true)
    fun addToQueue(item: Item): Boolean = enqueue(item, next = false)

    private fun enqueue(item: Item, next: Boolean): Boolean {
        val empty = current == null
        if (empty) {
            playQueue(listOf(item), 0)
            return true
        }
        if (isFile(item) != local.active || isVideo) return false
        if (local.active) {
            if (next) local.insertNext(item) else local.append(item)
        } else {
            val media = audioMediaItem(item)
            if (next) player.addMediaItem(player.currentMediaItemIndex + 1, media) else player.addMediaItem(media)
        }
        return true
    }

    private fun isFile(item: Item) = item.id.startsWith("/")

    private fun audioSource(item: Item): MediaSource = ProgressiveMediaSource.Factory(dataSourceFactory)
        .setLoadErrorHandlingPolicy(loadErrorPolicy)
        .createMediaSource(audioMediaItem(item))

    companion object {
        // a song is its proxied AAC stream (itag 140); the Item rides along as the tag
        fun audioMediaItem(item: Item): MediaItem =
            MediaItem.Builder().setUri("${Api.base}/s/${item.id}/140").setMimeType(MimeTypes.AUDIO_MP4).setMediaId(item.id).setTag(item).build()
    }

    // null = Auto (adaptive, capped at 360p); otherwise the rendition with that height. Changes
    // the preference in place instead of re-running track selection, so nothing is re-buffered.
    fun setQuality(height: Int?) {
        QualityPreference.height = height
        Log.i(TAG, "quality -> ${height ?: "auto"}")
    }

    private fun buildSource(id: String, mode: Mode): MediaSource = when (mode) {
        Mode.VIDEO -> DashMediaSource.Factory(dataSourceFactory)
            .setLoadErrorHandlingPolicy(loadErrorPolicy)
            .createMediaSource(MediaItem.fromUri("${Api.base}/mpd/$id"))
        Mode.AUDIO -> ProgressiveMediaSource.Factory(dataSourceFactory)
            .setLoadErrorHandlingPolicy(loadErrorPolicy)
            .createMediaSource(MediaItem.Builder().setUri("${Api.base}/s/$id/140").setMimeType(MimeTypes.AUDIO_MP4).build())
        Mode.PROGRESSIVE -> ProgressiveMediaSource.Factory(dataSourceFactory)
            .setLoadErrorHandlingPolicy(loadErrorPolicy)
            .createMediaSource(MediaItem.Builder().setUri("${Api.base}/s/$id/18").setMimeType(MimeTypes.VIDEO_MP4).build())
    }

    // ExoPlayer is playing something from the server (not a video on the card)
    private val exoStreaming: Boolean
        get() = player.mediaItemCount > 0 && (player.currentMediaItem?.localConfiguration?.tag as? Item)?.let { isFile(it) } != true

    // Re-prepare and continue where playback stopped: the manifest/stream is fetched again and the
    // server re-resolves expired upstream URLs (502), or the network came back. At most 3 a minute.
    private fun recover(error: PlaybackException) {
        if (player.mediaItemCount == 0) return
        // the server can't stream this video at all (404 no_dash): another try ends the same way
        val status = (error.cause as? com.google.android.exoplayer2.upstream.HttpDataSource.InvalidResponseCodeException)?.responseCode
        if (status == 404 || status == 400) return
        val now = SystemClock.elapsedRealtime()
        while (recoveries.isNotEmpty() && now - recoveries.first() > 60_000) recoveries.removeFirst()
        if (recoveries.size >= 3) {
            Log.e(TAG, "giving up after ${recoveries.size} recoveries in a minute", error)
            return
        }
        recoveries.addLast(now)
        val index = player.currentMediaItemIndex
        val position = player.currentPosition
        Log.w(TAG, "recovering from ${error.errorCodeName} at #$index ${position}ms (attempt ${recoveries.size})")
        player.seekTo(index, position)
        player.prepare()
        player.play()
    }

    // Foreground notification and Wi-Fi lock follow what is playing, for both engines.
    private fun changed() {
        // music started: the radio stops (and FmScreen pauses music when the radio starts)
        if (isPlaying() && (FmRadio.state.on || FmRadio.state.starting)) FmRadio.stop()
        val streaming = !local.active && player.playWhenReady && exoStreaming
        if (streaming && !wifiLock.isHeld) wifiLock.acquire() else if (!streaming && wifiLock.isHeld) wifiLock.release()
        val item = current
        val active = item != null && (isPlaying() || (local.active && local.isBuffering()) || (!local.active && player.playWhenReady))
        if (active && item != null) {
            startForeground(NOTIFICATION_ID, notification(item))
            foreground = true
        } else if (FmRadio.state.on) {
            startForeground(NOTIFICATION_ID, radioNotification())
            foreground = true
        } else if (foreground) {
            stopForeground(item == null)
            foreground = false
        }
    }

    private fun radioNotification(): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle("FM ${FmRadio.state.frequency / 10.0} MHz")
            .setContentText(FmRadio.state.name)
            .setOngoing(true)
            .setContentIntent(PendingIntent.getActivity(this, 0, Intent(this, ShellActivity::class.java), PendingIntent.FLAG_UPDATE_CURRENT))
            .build()

    // the radio changes: keep the service in front while it plays
    private val radioChanged: () -> Unit = { changed() }

    // what is playing stops for the radio
    fun pause() {
        if (local.active) local.pause() else player.pause()
    }

    private fun notification(item: Item): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle(item.title)
            .setContentText(item.subtitle.ifEmpty { if (local.active) "Library" else "YouTube Music" })
            .setOngoing(true)
            .setContentIntent(PendingIntent.getActivity(this, 0, Intent(this, ShellActivity::class.java), PendingIntent.FLAG_UPDATE_CURRENT))
            .build()

    private val playerListener = object : Player.Listener {
        override fun onPlayerError(error: PlaybackException) {
            Log.e(TAG, "playback error ${error.errorCodeName}: ${error.cause}")
            when (error.errorCode) {
                PlaybackException.ERROR_CODE_IO_BAD_HTTP_STATUS,
                PlaybackException.ERROR_CODE_IO_NETWORK_CONNECTION_FAILED,
                PlaybackException.ERROR_CODE_IO_NETWORK_CONNECTION_TIMEOUT,
                PlaybackException.ERROR_CODE_IO_UNSPECIFIED -> if (exoStreaming && isOnline()) recover(error)
                else -> Unit
            }
        }

        override fun onVideoSizeChanged(videoSize: com.google.android.exoplayer2.video.VideoSize) {
            Log.i(TAG, "video ${videoSize.width}x${videoSize.height} at ${player.currentPosition}ms")
        }

        override fun onIsPlayingChanged(isPlaying: Boolean) {
            Log.i(TAG, "${if (isPlaying) "playing" else "not playing"} at ${player.currentPosition}ms")
            changed()
        }

        override fun onPlayWhenReadyChanged(playWhenReady: Boolean, reason: Int) = changed()

        override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) = changed()

        override fun onPlaybackStateChanged(state: Int) {
            val name = when (state) {
                Player.STATE_IDLE -> "idle"; Player.STATE_BUFFERING -> "buffering"
                Player.STATE_READY -> "ready"; else -> "ended"
            }
            Log.i(TAG, "state $name at ${player.currentPosition}ms")
        }
    }

    private fun isOnline(): Boolean {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        @Suppress("DEPRECATION")
        return cm.activeNetworkInfo?.isConnected == true
    }

    private val connectivityReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (local.active || !exoStreaming) return // the card does not care about Wi-Fi
            val online = isOnline()
            Log.i(TAG, "connectivity: ${if (online) "online" else "offline"}")
            // a load that failed while offline leaves the player in an error state: resume that too
            val failedWhileOffline = player.playerError != null
            if (!online && (player.playWhenReady || failedWhileOffline)) {
                pausedForNetwork = true
                player.pause()
            } else if (online && (pausedForNetwork || failedWhileOffline)) {
                pausedForNetwork = false
                if (player.playbackState == Player.STATE_IDLE) player.prepare()
                player.play()
            }
        }
    }
}
