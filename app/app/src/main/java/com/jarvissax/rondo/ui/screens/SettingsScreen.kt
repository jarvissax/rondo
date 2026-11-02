package com.jarvissax.rondo.ui.screens

import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.provider.Settings
import android.view.View
import android.widget.LinearLayout
import com.jarvissax.rondo.BuildConfig
import com.jarvissax.rondo.input.WheelHaptics
import com.jarvissax.rondo.Config
import com.jarvissax.rondo.Prefs
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.player.QualityPreference
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.RowList
import com.jarvissax.rondo.ui.RowStyle
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.ShellStatusBar
import com.jarvissax.rondo.ui.Tiles
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.lp

// iPod-style full-width list in the shell's light look.
abstract class ShellListScreen(shell: ShellActivity, private val title: String) : Screen(shell) {
    protected lateinit var list: RowList
    private lateinit var status: ShellStatusBar

    // a theme change rebuilds the screen: stay on the same row (false: render picks the row itself)
    protected open val keepRowOnRebuild = true

    override fun build(): View {
        val c = context
        val before = if (::list.isInitialized) list.selected else -1
        val root = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Palette.shellBg)
        }
        status = ShellStatusBar(c, title)
        root.addView(status, lp(MATCH, c.dp(22)))
        list = RowList(c, RowStyle.SHELL).apply { setPadding(0, c.dp(3), 0, 0) }
        root.addView(list, lp(MATCH, 0, 1f))
        render()
        if (before >= 0 && keepRowOnRebuild) list.select(before)
        return root
    }

    abstract fun render()

    override fun onAction(action: WheelAction, steps: Int): Boolean = list.handle(action, steps)
    override fun onTick() = status.refresh(player?.isPlaying() == true)
}

class SettingsScreen(shell: ShellActivity) : ShellListScreen(shell, "Settings") {
    private var server = "Checking…"

    override fun render() {
        val c = context
        val quality = Prefs.videoQuality(c)?.let { "${it}p" } ?: "Auto"
        list.setRows(
            listOf(
                Row.Entry("Dashboard", tile = Tiles.dashboard, chevron = true, onSelect = { shell.push(DashboardScreen(shell)) }),
                Row.Entry("Wi-Fi", trailing = wifiName(c), tile = Tiles.wifi, chevron = true, onSelect = { shell.push(WifiScreen(shell)) }),
                Row.Entry("Bluetooth", trailing = bluetoothState(), tile = Tiles.bluetooth, chevron = true, onSelect = { shell.push(BluetoothScreen(shell)) }),
                Row.Entry("Theme", trailing = accentName(Config.theme.accent), tile = Tiles.theme, chevron = true, onSelect = { shell.push(ThemeScreen(shell)) }),
                Row.Entry("Video quality", trailing = quality, tile = Tiles.quality, chevron = true, onSelect = { shell.push(QualityChoiceScreen(shell)) }),
                Row.Entry("Brightness", trailing = "${brightnessPercent()}%", tile = Tiles.brightness, chevron = true, onSelect = { shell.push(BrightnessScreen(shell)) }),
                Row.Entry("Screen off after", trailing = timeoutName(screenTimeout()), tile = Tiles.timer, chevron = true, onSelect = { shell.push(TimeoutScreen(shell)) }),
                Row.Entry("Open lyrics automatically", trailing = onOff(Config.behavior.lyricsOnOpen), tile = Tiles.lyrics, onSelect = {
                    Config.update(Config.Change.BEHAVIOR) { d -> d.copy(behavior = d.behavior.copy(lyricsOnOpen = !d.behavior.lyricsOnOpen)) }
                    render()
                }),
                Row.Entry("Wheel acceleration", trailing = accelerationName(Config.behavior.wheelAcceleration), tile = Tiles.accel, chevron = true, onSelect = { shell.push(AccelerationScreen(shell)) }),
                Row.Entry("Wheel vibration", trailing = vibrationName(Config.behavior.wheelVibration), tile = Tiles.wheel, chevron = true, onSelect = { shell.push(VibrationScreen(shell)) }),
                Row.Entry("Click sound", trailing = onOff(clickSound(c)), tile = Tiles.sound, onSelect = {
                    setClickSound(c, !clickSound(c))
                    render()
                }),
                Row.Entry("Server", trailing = server, tile = Tiles.server, chevron = true, onSelect = { shell.push(ServerScreen(shell)) }),
                Row.Entry("About", trailing = "Rondo ${BuildConfig.VERSION_NAME}", tile = Tiles.about, chevron = true, onSelect = { shell.push(AboutScreen(shell)) }),
            ) + if (com.jarvissax.rondo.Power.available(c)) listOf(
                Row.Entry("Shut down", tile = Tiles.power, chevron = true, onSelect = { shell.push(PowerScreen(shell)) }),
            ) else emptyList(),
            keepSelection = true,
        )
    }

    override fun onShow() {
        render()
        checkServer()
        Bt.bindA2dp(context) { render() }
    }

    // on opening Settings, and on center: the row says Checking… until the server answers
    private fun checkServer() {
        server = "Checking…"
        render()
        launch {
            server = serverState()
            render()
        }
    }

    // What the server this Rondo uses says about itself (GET /healthz), and whether it accepts this
    // Rondo's token (GET /time): unreachable, token refused, playback failing (no yt-dlp, or the
    // server's own playback check failed), else OK, signed in or signed out (docs/dev/server-api.md).
    private suspend fun serverState(): String {
        if (!Api.configured) return "Not set up"
        val h = try {
            Api.health()
        } catch (e: Exception) {
            return "Unreachable"
        }
        try {
            Api.time()
        } catch (e: com.jarvissax.rondo.net.HttpException) {
            if (e.code == 401 || e.code == 403) return "Token refused"
        } catch (e: Exception) {
            return "Unreachable"
        }
        return when {
            h.ytdlp == null -> "yt-dlp missing"
            h.lastResolveOk == false -> "Playback failing"
            h.account.music || h.account.videos -> "OK · signed in"
            else -> "OK · signed out"
        }
    }

    private fun bluetoothState(): String = when (BluetoothAdapter.getDefaultAdapter()?.isEnabled) {
        true -> Bt.a2dp?.connectedDevices?.firstOrNull()?.let { Bt.name(it) } ?: "On"
        false -> "Off"
        null -> "—"
    }

    private fun brightnessPercent(): Int =
        (Settings.System.getInt(context.contentResolver, Settings.System.SCREEN_BRIGHTNESS, 128) * 100 + 127) / 255

    private fun screenTimeout(): Int = Settings.System.getInt(context.contentResolver, Settings.System.SCREEN_OFF_TIMEOUT, 60_000)
}

fun timeoutName(ms: Int): String = when {
    ms >= 60_000 -> "${ms / 60_000} min"
    else -> "${ms / 1000} s"
}

// A list of choices with the current one marked; picking one goes back.
abstract class ChoiceScreen<T>(shell: ShellActivity, title: String) : ShellListScreen(shell, title) {
    abstract val choices: List<Pair<String, T>>
    abstract fun current(): T
    abstract fun pick(value: T)
    open fun note(value: T): String = ""

    override fun render() {
        val now = current()
        list.setRows(
            choices.map { (name, value) ->
                Row.Entry(
                    title = name,
                    trailing = if (value == now) "Current" else note(value),
                    onSelect = {
                        pick(value)
                        shell.pop()
                    },
                )
            },
        )
        list.select(choices.indexOfFirst { it.second == now }.coerceAtLeast(0))
    }
}

class QualityChoiceScreen(shell: ShellActivity) : ChoiceScreen<Int?>(shell, "Video quality") {
    override val choices = listOf<Pair<String, Int?>>("Auto" to null, "720p" to 720, "480p" to 480, "360p" to 360)
    override fun current() = Prefs.videoQuality(context)
    override fun note(value: Int?) = if (value == null) "up to 360p" else ""
    override fun pick(value: Int?) {
        Prefs.setVideoQuality(context, value)
        QualityPreference.height = value
    }
}

// Battery: the backlight is the biggest draw after Wi-Fi.
class BrightnessScreen(shell: ShellActivity) : ChoiceScreen<Int>(shell, "Brightness") {
    override val choices = listOf(10, 25, 40, 60, 80, 100).map { "$it%" to it }
    override fun current(): Int {
        val v = Settings.System.getInt(context.contentResolver, Settings.System.SCREEN_BRIGHTNESS, 128)
        return choices.minByOrNull { Math.abs(it.second * 255 / 100 - v) }!!.second
    }
    override fun note(value: Int) = if (value <= 40) "saves battery" else ""
    override fun pick(value: Int) {
        val r = context.contentResolver
        Settings.System.putInt(r, Settings.System.SCREEN_BRIGHTNESS_MODE, Settings.System.SCREEN_BRIGHTNESS_MODE_MANUAL)
        Settings.System.putInt(r, Settings.System.SCREEN_BRIGHTNESS, (value * 255 / 100).coerceIn(10, 255))
    }
}

fun onOff(on: Boolean) = if (on) "On" else "Off"

// The tick on each wheel step: the Y1's system UI plays it for every app while Android's "sound
// effects" setting is on (Rockbox's setup script turned it off; docs/device/README.md). The Rondo
// firmware has no system UI, and Rondo plays it itself under the same setting (ShellActivity).
fun clickSound(c: android.content.Context): Boolean =
    Settings.System.getInt(c.contentResolver, Settings.System.SOUND_EFFECTS_ENABLED, 1) != 0

fun setClickSound(c: android.content.Context, on: Boolean) {
    Settings.System.putInt(c.contentResolver, Settings.System.SOUND_EFFECTS_ENABLED, if (on) 1 else 0)
}

fun accelerationName(level: String): String = when (level) {
    "off" -> "Off"
    "fast" -> "Fast"
    else -> "Normal"
}

// Long fast spins move lists and the scrubber several steps a pulse (input/WheelInput.kt)
class AccelerationScreen(shell: ShellActivity) : ChoiceScreen<String>(shell, "Wheel acceleration") {
    override val choices = listOf("off", "normal", "fast").map { accelerationName(it) to it }
    override fun current() = Config.behavior.wheelAcceleration
    override fun note(value: String) = when (value) {
        "off" -> "one step a pulse"
        "fast" -> "after a short spin"
        else -> "after a long spin"
    }
    override fun pick(value: String) {
        Config.update(Config.Change.BEHAVIOR) { d -> d.copy(behavior = d.behavior.copy(wheelAcceleration = value)) }
    }
}

fun vibrationName(level: String): String = when (level) {
    "off" -> "Off"
    "light" -> "Light"
    "strong" -> "Strong"
    else -> "Normal"
}

// A buzz on each wheel step, like Rockbox's "wheel vibration intensity" (input/WheelHaptics.kt).
// Turning through this list already buzzes at the current level; picking one applies it at once.
class VibrationScreen(shell: ShellActivity) : ChoiceScreen<String>(shell, "Wheel vibration") {
    override val choices = WheelHaptics.LEVELS.map { vibrationName(it) to it }
    override fun current() = Config.behavior.wheelVibration
    override fun note(value: String) = if (value == "normal") "like Rockbox" else ""
    override fun pick(value: String) {
        Config.update(Config.Change.BEHAVIOR) { d -> d.copy(behavior = d.behavior.copy(wheelVibration = value)) }
    }
}

class TimeoutScreen(shell: ShellActivity) : ChoiceScreen<Int>(shell, "Screen off after") {
    override val choices = listOf(15_000, 30_000, 60_000, 120_000, 300_000, 600_000).map { timeoutName(it) to it }
    override fun current(): Int {
        val v = Settings.System.getInt(context.contentResolver, Settings.System.SCREEN_OFF_TIMEOUT, 60_000)
        return choices.minByOrNull { Math.abs(it.second - v) }!!.second
    }
    override fun note(value: Int) = if (value <= 30_000) "saves battery" else ""
    override fun pick(value: Int) {
        Settings.System.putInt(context.contentResolver, Settings.System.SCREEN_OFF_TIMEOUT, value)
    }
}

// Settings › About: what the dashboard's Overview shows about the device.
class AboutScreen(shell: ShellActivity) : ShellListScreen(shell, "About") {
    override fun render() {
        val c = context
        val battery = c.registerReceiver(null, android.content.IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val charging = (battery?.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
        val wifi = (c.applicationContext.getSystemService(android.content.Context.WIFI_SERVICE) as android.net.wifi.WifiManager).connectionInfo
        val ip = wifi?.ipAddress ?: 0
        val rows = mutableListOf<Row>(
            Row.Entry("Rondo", trailing = BuildConfig.VERSION_NAME, tile = Tiles.about),
            Row.Entry("Model", trailing = "Innioasis Y1"),
            Row.Entry("Battery", trailing = "${Math.round(com.jarvissax.rondo.ui.batteryLevel(c) * 100)}%" + if (charging) " · charging" else ""),
        )
        listOf("/storage/sdcard0" to "Card", "/storage/sdcard1" to "Internal storage").forEach { (path, name) ->
            val stat = runCatching { android.os.StatFs(path) }.getOrNull() ?: return@forEach
            @Suppress("DEPRECATION")
            val total = stat.blockCount.toLong() * stat.blockSize
            @Suppress("DEPRECATION")
            val free = stat.availableBlocks.toLong() * stat.blockSize
            if (total > 0) rows += Row.Entry(name, trailing = "${bytes(free)} free of ${bytes(total)}")
        }
        rows += Row.Entry("Wi-Fi", trailing = wifiName(c))
        if (ip != 0) rows += Row.Entry("IP address", trailing = "${ip and 0xff}.${ip shr 8 and 0xff}.${ip shr 16 and 0xff}.${ip shr 24 and 0xff}")
        rows += Row.Entry("Server", trailing = Api.base.removePrefix("https://").removePrefix("http://").ifEmpty { "Not set up" })
        list.setRows(rows)
    }

    private var ticks = 0

    override fun onTick() {
        super.onTick()
        if (++ticks % 20 == 0) render() // battery and Wi-Fi, every 10 s
    }
}

fun bytes(n: Long): String = when {
    n >= 1L shl 30 -> String.format("%.1f GB", n / (1L shl 30).toDouble())
    n >= 1L shl 20 -> String.format("%.0f MB", n / (1L shl 20).toDouble())
    else -> "${n / 1024} KB"
}
