package com.jarvissax.rondo.player

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioManager
import android.media.MediaPlayer
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import com.mediatek.FMRadio.FMRadioNative
import com.jarvissax.rondo.Config
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

// FM radio through MediaTek's own pieces (docs/device/README.md, FM radio): the chip through
// libfmjni (bound to com.mediatek.FMRadio.FMRadioNative), /dev/fm (the media group, from the
// ACCESS_MTK_MMHW permission), and the sound through a MediaPlayer on MediaTek's FM source and
// stream, as MediaTek's FM app does. Wired headphones are the antenna: without them it won't start.
// Frequencies are tenths of MHz (1017 = 101.7 MHz). Every chip call runs on one worker thread.
object FmRadio {
    private const val TAG = "Y1"
    // MediaTek's AudioManager.STREAM_FM: the radio has its own volume (the wheel sets it while the
    // radio plays). Its sound goes straight from the chip to the DAC over I2S, so AudioFlinger shows
    // no track for it.
    const val STREAM_FM = 10
    private const val FOR_PROPRIETARY = 5 // MediaTek's AudioSystem force-use slot for FM output
    private const val FORCE_NONE = 0
    private const val FORCE_SPEAKER = 1
    private const val SOURCE = "MEDIATEK://MEDIAPLAYER_PLAYERTYPE_FM"

    // the chip covers 76–108 MHz (MediaTek's config, logged at power-up); "world" 87.5–108 MHz in
    // 0.1 MHz steps serves Europe, Africa, Asia, Australia and the Americas, "japan" 76–95 MHz
    val MIN get() = if (Config.radio.band == "japan") 760 else 875
    val MAX get() = if (Config.radio.band == "japan") 950 else 1080

    data class State(
        val on: Boolean = false,
        val starting: Boolean = false,
        val frequency: Int = MIN,
        val muted: Boolean = false,
        val speaker: Boolean = false,
        val name: String = "", // RDS programme service name
        val text: String = "", // RDS radio text
        val signal: Int = 0, // 0..4
        val stereo: Boolean = false,
        val seeking: Boolean = false,
        val scanning: Boolean = false,
        val error: String? = null, // "headphones" | "chip"
    )

    @Volatile var state = State()
        private set

    private val worker = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "fm").apply { isDaemon = true } }
    private val main = Handler(Looper.getMainLooper())
    private val listeners = mutableListOf<() -> Unit>()
    private lateinit var app: Context
    private var player: MediaPlayer? = null
    private var rds = false
    private var poll: java.util.concurrent.ScheduledFuture<*>? = null

    fun init(context: Context) {
        app = context.applicationContext
        state = state.copy(frequency = Config.radio.frequency.coerceIn(MIN, MAX), speaker = Config.radio.speaker)
        // the antenna is the headphone cable: unplugged, the radio stops
        app.registerReceiver(object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                if (i.getIntExtra("state", 0) == 0 && (state.on || state.starting)) stop()
            }
        }, IntentFilter(Intent.ACTION_HEADSET_PLUG))
    }

    fun listen(l: () -> Unit) = synchronized(listeners) { listeners += l }
    fun unlisten(l: () -> Unit) = synchronized(listeners) { listeners -= l }

    private fun update(change: (State) -> State) {
        state = change(state)
        main.post { synchronized(listeners) { listeners.toList() }.forEach { it() } }
    }

    @Suppress("DEPRECATION")
    fun headphonesIn(): Boolean = (app.getSystemService(Context.AUDIO_SERVICE) as AudioManager).isWiredHeadsetOn

    fun start() {
        if (state.on || state.starting) return
        if (!headphonesIn()) return update { it.copy(error = "headphones") }
        update { it.copy(starting = true, error = null) }
        worker.execute {
            val f = state.frequency
            val ok = runCatching { FMRadioNative.opendev() && FMRadioNative.powerup(f / 10f) }
                .onFailure { Log.w(TAG, "fm: power up failed: $it") }.getOrDefault(false)
            if (!ok) {
                runCatching { FMRadioNative.closedev() }
                return@execute update { it.copy(starting = false, error = "chip") }
            }
            // as the stock player does: muted while the sound path starts (no pop), then unmuted
            runCatching { FMRadioNative.setmute(true) }
            rds = runCatching { FMRadioNative.isRDSsupport() == 1 }.getOrDefault(false)
            if (rds) runCatching { FMRadioNative.rdsset(true) }
            Log.i(TAG, "fm: on at ${f / 10f} MHz, chip ${runCatching { FMRadioNative.getchipid() }.getOrNull()?.let { Integer.toHexString(it) }}, rds $rds")
            val audio = java.util.concurrent.CountDownLatch(1)
            main.post {
                startAudio()
                audio.countDown()
            }
            audio.await(3, TimeUnit.SECONDS)
            runCatching { FMRadioNative.setmute(false) }
            update { it.copy(on = true, starting = false, muted = false, name = "", text = "") }
            poll = worker.scheduleWithFixedDelay(::readStatus, 1, 1, TimeUnit.SECONDS)
        }
    }

    fun stop() {
        poll?.cancel(false)
        poll = null
        main.post { stopAudio() }
        worker.execute {
            runCatching {
                FMRadioNative.powerdown(0)
                FMRadioNative.closedev()
            }.onFailure { Log.w(TAG, "fm: power down failed: $it") }
            update { it.copy(on = false, starting = false, seeking = false, scanning = false, name = "", text = "", signal = 0, stereo = false) }
        }
    }

    fun toggle() = if (state.on || state.starting) stop() else start()

    // the wheel: ±0.1 MHz a step, wrapping at the band's ends
    fun step(steps: Int) {
        val span = MAX - MIN + 1
        tune(MIN + ((state.frequency - MIN + steps) % span + span) % span)
    }

    fun tune(frequency: Int) {
        val f = frequency.coerceIn(MIN, MAX)
        update { it.copy(frequency = f, name = "", text = "") }
        remember(f)
        if (!state.on) return
        worker.execute { runCatching { FMRadioNative.tune(f / 10f) }.onFailure { Log.w(TAG, "fm: tune failed: $it") } }
    }

    fun seek(up: Boolean) {
        if (!state.on || state.seeking) return
        update { it.copy(seeking = true) }
        worker.execute {
            val found = runCatching { FMRadioNative.seek(state.frequency / 10f, up) }.getOrDefault(0f)
            val f = Math.round(found * 10)
            if (f in MIN..MAX) {
                runCatching { FMRadioNative.tune(f / 10f) }
                remember(f)
                update { it.copy(frequency = f, seeking = false, name = "", text = "") }
            } else {
                update { it.copy(seeking = false) }
            }
        }
    }

    // every station in the band (takes ~10-20 s); the result is remembered for the station list
    fun scan(done: (List<Int>) -> Unit = {}) {
        if (!state.on || state.scanning) return
        update { it.copy(scanning = true) }
        worker.execute {
            val raw = runCatching { FMRadioNative.autoscan() }.onFailure { Log.w(TAG, "fm: scan failed: $it") }.getOrNull()
            val found = raw?.map { it.toInt() }?.filter { it in MIN..MAX }?.distinct()?.sorted().orEmpty()
            Log.i(TAG, "fm: scan found ${found.size} stations")
            Config.update(Config.Change.RADIO) { d -> d.copy(radio = d.radio.copy(found = found)) }
            // the chip stops on the last station it found: back to the one we were on
            runCatching { FMRadioNative.tune(state.frequency / 10f) }
            update { it.copy(scanning = false) }
            main.post { done(found) }
        }
    }

    fun setBand(band: String) {
        Config.update(Config.Change.RADIO) { d -> d.copy(radio = d.radio.copy(band = band, found = emptyList())) }
        tune(state.frequency.coerceIn(MIN, MAX))
    }

    fun setMuted(muted: Boolean) {
        if (!state.on) return
        update { it.copy(muted = muted) }
        worker.execute { runCatching { FMRadioNative.setmute(muted) } }
    }

    // MediaTek's FM app switches the output with AudioSystem.setForceUse(FOR_PROPRIETARY, …): a
    // hidden API, called by reflection
    fun setSpeaker(on: Boolean) {
        update { it.copy(speaker = on) }
        Config.update(Config.Change.RADIO) { d -> d.copy(radio = d.radio.copy(speaker = on)) }
        if (state.on) applySpeaker(on)
    }

    private fun applySpeaker(on: Boolean) {
        runCatching {
            Class.forName("android.media.AudioSystem").getMethod("setForceUse", Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
                .invoke(null, FOR_PROPRIETARY, if (on) FORCE_SPEAKER else FORCE_NONE)
        }.onFailure { Log.w(TAG, "fm: speaker switch failed: $it") }
    }

    private fun remember(f: Int) {
        if (Config.radio.frequency != f) Config.update(Config.Change.RADIO) { d -> d.copy(radio = d.radio.copy(frequency = f)) }
    }

    private fun startAudio() {
        if (player != null) return
        runCatching {
            player = MediaPlayer().apply {
                setWakeMode(app, PowerManager.PARTIAL_WAKE_LOCK) // keeps playing with the screen off
                setDataSource(SOURCE)
                setAudioStreamType(STREAM_FM)
                setOnErrorListener { _, what, extra ->
                    Log.w(TAG, "fm: player error $what/$extra")
                    true
                }
                prepare()
                start()
            }
            applySpeaker(state.speaker)
        }.onFailure {
            Log.w(TAG, "fm: audio failed: $it")
            player?.release()
            player = null
        }
    }

    private fun stopAudio() {
        player?.let {
            runCatching { it.stop() }
            it.release()
        }
        player = null
        applySpeaker(false) // leave the system's own routing as we found it
    }

    // MediaTek's FM sound switch is one for the whole system, and any FM player that goes away turns
    // it off. The stock Innioasis app keeps an idle FM player from boot, so when its process ended
    // the radio went silent while still tuned (docs/device/README.md, FM radio). Rondo starts its
    // own player again: the old one is released first, since releasing it switches the sound off.
    private fun soundOn(): Boolean = runCatching {
        (app.getSystemService(Context.AUDIO_SERVICE) as AudioManager).getParameters("GetFmEnable").endsWith("=true")
    }.getOrDefault(true)

    private fun restartAudio() {
        Log.i(TAG, "fm: the sound was switched off elsewhere, starting it again")
        runCatching { FMRadioNative.setmute(true) }
        val audio = java.util.concurrent.CountDownLatch(1)
        main.post {
            stopAudio()
            startAudio()
            audio.countDown()
        }
        audio.await(3, TimeUnit.SECONDS)
        runCatching { FMRadioNative.setmute(state.muted) }
    }

    // once a second: the sound path, signal, stereo and RDS name / text
    private fun readStatus() {
        if (!state.on) return
        if (!state.seeking && !state.scanning && !soundOn()) restartAudio()
        runCatching {
            val rssi = FMRadioNative.readRssi()
            val signal = when {
                rssi >= -60 -> 4
                rssi >= -70 -> 3
                rssi >= -80 -> 2
                rssi >= -90 -> 1
                else -> 0
            }
            val stereo = FMRadioNative.stereoMono()
            var name = state.name
            var text = state.text
            if (rds) {
                FMRadioNative.readrds()
                FMRadioNative.getPS()?.let { rdsText(it) }?.takeIf { it.isNotEmpty() }?.let { name = it }
                FMRadioNative.getLRText()?.let { rdsText(it) }?.takeIf { it.isNotEmpty() }?.let { text = it }
            }
            if (name.isNotEmpty() && Config.radio.names[state.frequency] != name) {
                val f = state.frequency
                Config.update(Config.Change.RADIO) { d -> d.copy(radio = d.radio.copy(names = d.radio.names + (f to name))) }
            }
            if (signal != state.signal || stereo != state.stereo || name != state.name || text != state.text) {
                update { it.copy(signal = signal, stereo = stereo, name = name, text = text) }
            }
        }.onFailure { Log.w(TAG, "fm: status failed: $it") }
    }

    private fun rdsText(bytes: ByteArray): String =
        String(bytes, Charsets.ISO_8859_1).replace('\u0000', ' ').trim().replace(Regex("\\s+"), " ")
}
