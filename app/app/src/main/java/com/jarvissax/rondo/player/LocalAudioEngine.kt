package com.jarvissax.rondo.player

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioManager
import android.media.MediaPlayer
import android.os.PowerManager
import android.util.Log
import com.google.android.exoplayer2.Player
import com.jarvissax.rondo.net.Item

// Music from the microSD card. Android 4.2 has no FLAC decoder ExoPlayer can use (MediaCodec
// gets one in 8.1) and the card is 814 FLAC files (2026-10-30), but the platform MediaPlayer
// plays FLAC, so local music goes through it. Gapless: the next track is prepared and chained
// with setNextMediaPlayer (API 16).
class LocalAudioEngine(private val context: Context, private val onChange: () -> Unit) {
    private var items: List<Item> = emptyList()
    private var order: List<Int> = emptyList() // play order: indexes into items
    private var pos = 0 // position in order
    private var current: MediaPlayer? = null
    private var next: MediaPlayer? = null
    private var prepared = false
    private var wantPlaying = false

    var shuffle = false
        set(value) {
            field = value
            rebuildOrder(keepCurrent = true)
        }
    var repeat = Player.REPEAT_MODE_OFF
        set(value) {
            field = value
            prepareNext()
        }

    val active: Boolean get() = items.isNotEmpty()
    val item: Item? get() = order.getOrNull(pos)?.let { items[it] }
    val upNext: Item? get() = nextPos()?.let { items[order[it]] }
    val queue: List<Item> get() = items
    val index: Int get() = order.getOrNull(pos) ?: -1

    private val audio = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val focus = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_LOSS, AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> pause()
        }
    }
    private val noisy = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) = pause() // headphones unplugged
    }
    private var noisyRegistered = false

    fun play(list: List<Item>, start: Int) {
        stop()
        items = list
        order = list.indices.toList()
        pos = start.coerceIn(0, list.lastIndex)
        if (shuffle) rebuildOrder(keepCurrent = true)
        wantPlaying = true
        startCurrent()
    }

    private fun rebuildOrder(keepCurrent: Boolean) {
        if (items.isEmpty()) return
        val now = order.getOrNull(pos) ?: 0
        order = if (shuffle) listOf(now) + items.indices.filter { it != now }.shuffled() else items.indices.toList()
        pos = if (keepCurrent) order.indexOf(now) else 0
        prepareNext()
    }

    private fun nextPos(): Int? = when {
        repeat == Player.REPEAT_MODE_ONE -> pos
        pos + 1 < order.size -> pos + 1
        repeat == Player.REPEAT_MODE_ALL && order.isNotEmpty() -> 0
        else -> null
    }

    private fun create(item: Item): MediaPlayer = MediaPlayer().apply {
        setAudioStreamType(AudioManager.STREAM_MUSIC)
        setWakeMode(context, PowerManager.PARTIAL_WAKE_LOCK)
        setDataSource(item.id)
    }

    private fun startCurrent() {
        releasePlayers()
        val it = item ?: return
        prepared = false
        val mp = try {
            create(it)
        } catch (e: Exception) {
            Log.e("Y1", "local: cannot open ${it.id}: $e")
            skipBroken()
            return
        }
        current = mp
        mp.setOnPreparedListener {
            prepared = true
            if (wantPlaying) begin()
            prepareNext()
            onChange()
        }
        mp.setOnCompletionListener { onCompleted(it) }
        mp.setOnErrorListener { _, what, extra ->
            Log.e("Y1", "local: playback error $what/$extra on ${item?.id}")
            skipBroken()
            true
        }
        mp.prepareAsync()
        onChange()
    }

    private fun skipBroken() {
        val n = nextPos()
        if (n == null || n == pos) {
            stop()
        } else {
            pos = n
            startCurrent()
        }
    }

    private fun prepareNext() {
        next?.release()
        next = null
        val mp = current ?: return
        if (!prepared) return
        val n = nextPos() ?: return
        try {
            val nmp = create(items[order[n]])
            nmp.prepare() // local file: fast
            nmp.setOnCompletionListener { onCompleted(it) }
            mp.setNextMediaPlayer(nmp)
            next = nmp
        } catch (e: Exception) {
            Log.w("Y1", "local: next track not prepared: $e")
        }
    }

    private fun onCompleted(mp: MediaPlayer) {
        if (mp !== current) return
        val n = nextPos()
        val chained = next
        if (n != null && chained != null) {
            // the chained player already started (gapless)
            mp.release()
            current = chained
            current?.setOnErrorListener { _, _, _ -> skipBroken(); true }
            next = null
            pos = n
            prepared = true
            prepareNext()
        } else if (n != null) {
            pos = n
            startCurrent()
        } else {
            wantPlaying = false
            abandon()
        }
        onChange()
    }

    private fun begin() {
        audio.requestAudioFocus(focus, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN)
        if (!noisyRegistered) {
            context.registerReceiver(noisy, IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY))
            noisyRegistered = true
        }
        current?.start()
    }

    private fun abandon() {
        audio.abandonAudioFocus(focus)
        if (noisyRegistered) {
            context.unregisterReceiver(noisy)
            noisyRegistered = false
        }
    }

    fun isPlaying(): Boolean = runCatching { current?.isPlaying == true }.getOrDefault(false)
    fun isBuffering(): Boolean = active && !prepared
    fun position(): Long = if (prepared) runCatching { current?.currentPosition?.toLong() ?: 0L }.getOrDefault(0L) else 0L
    fun duration(): Long = if (prepared) runCatching { current?.duration?.toLong() ?: 0L }.getOrDefault(0L) else ((item?.durationSec ?: 0) * 1000L)

    fun play() {
        wantPlaying = true
        if (current == null) startCurrent() else if (prepared) begin()
        onChange()
    }

    fun pause() {
        wantPlaying = false
        runCatching { if (current?.isPlaying == true) current?.pause() }
        onChange()
    }

    fun toggle() = if (isPlaying()) pause() else play()

    fun seekTo(ms: Long) {
        if (prepared) runCatching { current?.seekTo(ms.coerceIn(0, duration()).toInt()) }
    }

    fun next() {
        if (pos + 1 < order.size || repeat == Player.REPEAT_MODE_ALL) {
            pos = if (pos + 1 < order.size) pos + 1 else 0
            startCurrent()
        }
    }

    fun previous() {
        if (position() > 3_000 || pos == 0) seekTo(0) else {
            pos -= 1
            startCurrent()
        }
    }

    fun jumpTo(itemIndex: Int) {
        val p = order.indexOf(itemIndex)
        if (p >= 0) {
            pos = p
            wantPlaying = true
            startCurrent()
        }
    }

    fun insertNext(item: Item) {
        if (items.isEmpty()) return play(listOf(item), 0)
        items = items + item
        val newIndex = items.lastIndex
        order = order.toMutableList().apply { add(pos + 1, newIndex) }
        prepareNext()
    }

    fun append(item: Item) {
        if (items.isEmpty()) return play(listOf(item), 0)
        items = items + item
        order = order + items.lastIndex
        prepareNext()
    }

    private fun releasePlayers() {
        runCatching { next?.release() }
        runCatching { current?.release() }
        next = null
        current = null
        prepared = false
    }

    fun stop() {
        wantPlaying = false
        releasePlayers()
        items = emptyList()
        order = emptyList()
        pos = 0
        abandon()
        onChange()
    }
}
