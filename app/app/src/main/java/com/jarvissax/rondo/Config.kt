package com.jarvissax.rondo

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.File
import kotlinx.serialization.ExperimentalSerializationApi
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonNames
import kotlinx.serialization.json.Json

// What the owner can change from the dashboard (docs/dev/dashboard-api.md) and the device: the theme of
// the iPod-style screens, a few behaviors, and which music server to use. Kept in
// files/config.json; every change is applied at once (theme changes redraw the screens).
object Config {
    @Serializable
    data class MenuEntry(val id: String, val visible: Boolean = true)

    @Serializable
    data class Theme(
        val accent: String = "#1c5ccc", // iPod blue
        val appearance: String = "light", // light | dark
        val textScale: Float = 1.4f,
        val menu: List<MenuEntry> = MENU_IDS.map { MenuEntry(it) },
    )

    @Serializable
    data class Behavior(
        val lyricsOnOpen: Boolean = false,
        val wheelAcceleration: String = "normal", // off | normal | fast
        val wheelVibration: String = "normal", // off | light | normal | strong (input/WheelHaptics.kt)
    )

    // the Rondo server this device is paired with (from the dashboard's Server page); null = none yet
    @Serializable
    data class Server(val url: String? = null, val token: String? = null)

    // FM radio (player/FmRadio.kt): the last station (tenths of MHz), speaker or headphones, saved
    // stations, and what the last scan found
    @Serializable
    data class Preset(val frequency: Int, val name: String = "")

    @Serializable
    data class Radio(
        val frequency: Int = 1000,
        val speaker: Boolean = false,
        val presets: List<Preset> = emptyList(),
        val found: List<Int> = emptyList(),
        val band: String = "world", // world 87.5–108 MHz | japan 76–95 MHz (the chip covers 76–108)
        val names: Map<Int, String> = emptyMap(), // RDS names heard, by frequency
    )

    // "behaviour" is how it was spelled before 2026-10-31: files saved then still load
    @OptIn(ExperimentalSerializationApi::class)
    @Serializable
    data class Data(
        val theme: Theme = Theme(),
        @JsonNames("behaviour") val behavior: Behavior = Behavior(),
        val server: Server = Server(),
        val radio: Radio = Radio(),
    )

    // the default main menu order: Now Playing, Library, YouTube, YouTube Music,
    // FM Radio, Settings
    val MENU_IDS = listOf("nowPlaying", "device", "videos", "music", "radio", "settings")
    val TEXT_SCALES = listOf(1.25f, 1.4f, 1.55f)
    val PRESETS = listOf(
        "iPod Blue" to "#1c5ccc",
        "Rondo Red" to "#ff3b4f",
        "Mint" to "#11a683",
        "Violet" to "#7b4fe0",
        "Amber" to "#e8860c",
        "Graphite" to "#4a4f59",
    )

    enum class Change { THEME, BEHAVIOR, SERVER, RADIO }

    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }
    private val main = Handler(Looper.getMainLooper())
    private lateinit var file: File
    private val listeners = mutableListOf<(Change) -> Unit>()

    @Volatile
    var data = Data()
        private set

    val theme get() = data.theme
    val behavior get() = data.behavior
    val server get() = data.server
    val radio get() = data.radio

    fun load(context: Context) {
        file = File(context.filesDir, "config.json")
        data = runCatching { json.decodeFromString(Data.serializer(), file.readText()) }.getOrDefault(Data())
        data = data.copy(theme = sanitize(data.theme))
    }

    // keeps a menu with every id once and Settings visible, whatever was stored or sent
    fun sanitize(t: Theme): Theme {
        val seen = t.menu.filter { it.id in MENU_IDS }.distinctBy { it.id }
        // an entry added in a later version goes just before Settings, not after it
        val added = MENU_IDS.filter { id -> seen.none { it.id == id } }.map { MenuEntry(it) }
        val settingsAt = seen.indexOfFirst { it.id == "settings" }.takeIf { it >= 0 } ?: seen.size
        val menu = (seen.take(settingsAt) + added.filter { it.id != "settings" } + seen.drop(settingsAt) + added.filter { it.id == "settings" })
            .map { if (it.id == "settings") it.copy(visible = true) else it }
        val accent = t.accent.takeIf { runCatching { android.graphics.Color.parseColor(it) }.isSuccess } ?: Theme().accent
        val scale = TEXT_SCALES.minByOrNull { Math.abs(it - t.textScale) } ?: 1.4f
        return t.copy(accent = accent, appearance = if (t.appearance == "dark") "dark" else "light", textScale = scale, menu = menu)
    }

    // from any thread; listeners run on the main thread
    fun update(change: Change, transform: (Data) -> Data) {
        synchronized(this) {
            val next = transform(data)
            data = next.copy(theme = sanitize(next.theme))
            runCatching { file.writeText(json.encodeToString(Data.serializer(), data)) }
                .onFailure { Log.w("Y1", "config: not saved: $it") }
        }
        main.post { listeners.toList().forEach { it(change) } }
    }

    fun listen(l: (Change) -> Unit) {
        listeners += l
    }

    fun unlisten(l: (Change) -> Unit) {
        listeners -= l
    }
}
