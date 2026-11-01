package com.jarvissax.rondo.ui.screens

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.net.wifi.WifiManager
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.jarvissax.rondo.Config
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.player.FmRadio
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.local.LocalLibrary
import com.jarvissax.rondo.ui.ArtColor
import com.jarvissax.rondo.ui.IconView
import com.jarvissax.rondo.ui.ImageLoader
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.MusicLogo
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.ProgressBarView
import com.jarvissax.rondo.ui.RoundedImageView
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.RowList
import com.jarvissax.rondo.ui.RowStyle
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.ShellStatusBar
import com.jarvissax.rondo.ui.Tile
import com.jarvissax.rondo.ui.Tiles
import com.jarvissax.rondo.ui.VideoLogo
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.darker
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.dpf
import com.jarvissax.rondo.ui.flp
import com.jarvissax.rondo.ui.line
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.text
import com.jarvissax.rondo.ui.visible

// The launcher (Main.dc.html, redesigned 2026-10-30): iPod menu with colored tiles on the left;
// on the right a preview of the selected entry, like the iPod Classic's split screen. Now
// Playing previews the playing artwork on a field of its color.
class MainMenuScreen(shell: ShellActivity) : Screen(shell) {
    private enum class Entry { MUSIC, VIDEOS, DEVICE, NOW_PLAYING, RADIO, SETTINGS }

    private lateinit var status: ShellStatusBar
    private lateinit var list: RowList
    private lateinit var pane: FrameLayout
    private lateinit var glyphBox: FrameLayout
    private lateinit var art: RoundedImageView
    private lateinit var paneTitle: TextView
    private lateinit var paneText: TextView
    private lateinit var paneProgress: ProgressBarView
    private var entry = Entry.MUSIC
    private var shownArt: String? = null
    private var artColor: Int? = null

    override fun build(): View {
        val c = context
        val root = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Palette.shellBg)
        }
        status = ShellStatusBar(c, "Rondo")
        root.addView(status, lp(MATCH, c.dp(22)))

        val body = LinearLayout(c)
        list = RowList(c, RowStyle.SHELL).apply { setPadding(0, c.dp(3), 0, 0) }
        // order and visibility come from the theme (dashboard); Settings is always there
        val rows = mapOf(
            "music" to Row.Entry("YouTube Music", tile = Tiles.music, chevron = true, onSelect = { shell.push(MusicScreen(shell)) }),
            "videos" to Row.Entry("YouTube", tile = Tiles.video, chevron = true, onSelect = { shell.push(VideoScreen(shell)) }),
            "device" to Row.Entry("Library", tile = Tiles.device, chevron = true, onSelect = { shell.push(DeviceScreen(shell)) }),
            "nowPlaying" to Row.Entry("Now Playing", tile = Tiles.nowPlaying, chevron = true, onSelect = { openNowPlaying(shell) }),
            "radio" to Row.Entry("FM Radio", tile = Tiles.radio, chevron = true, onSelect = { shell.push(FmScreen(shell)) }),
            "settings" to Row.Entry("Settings", tile = Tiles.settings, chevron = true, onSelect = { shell.push(SettingsScreen(shell)) }),
        )
        list.onSelectionChanged = { e ->
            entry = Entry.entries.firstOrNull { it.label() == e?.title } ?: Entry.MUSIC
            if (::pane.isInitialized) renderPane() // the first call comes before the pane is built
        }
        list.setRows(Config.theme.menu.filter { it.visible }.mapNotNull { rows[it.id] })
        body.addView(list, lp(c.dp(190), MATCH))
        body.addView(c.line(Palette.shellBarLine), lp(1, MATCH))

        pane = FrameLayout(c)
        val column = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(c.dp(10), c.dp(8), c.dp(10), c.dp(8))
        }
        val visual = FrameLayout(c)
        glyphBox = FrameLayout(c)
        art = RoundedImageView(c, c.dpf(6f), Color.argb(60, 255, 255, 255))
        visual.addView(glyphBox, flp(MATCH, MATCH))
        visual.addView(art, flp(MATCH, MATCH))
        column.addView(visual, lp(c.dp(98), c.dp(98)))
        paneTitle = c.text(14, Palette.white, bold = true, lines = 2).apply { gravity = Gravity.CENTER }
        paneText = c.text(11, Color.argb(215, 255, 255, 255), lines = 2).apply { gravity = Gravity.CENTER }
        column.addView(paneTitle, lp(MATCH, WRAP).apply { topMargin = c.dp(9) })
        column.addView(paneText, lp(MATCH, WRAP).apply { topMargin = c.dp(2) })
        paneProgress = ProgressBarView(c, Color.argb(70, 255, 255, 255), Palette.white, barDp = 2)
        column.addView(paneProgress, lp(c.dp(98), c.dp(8)).apply { topMargin = c.dp(6) })
        pane.addView(column, flp(MATCH, MATCH))
        body.addView(pane, lp(0, MATCH, 1f))
        root.addView(body, lp(MATCH, 0, 1f))
        renderPane()
        return root
    }

    private fun Entry.label() = when (this) {
        Entry.MUSIC -> "YouTube Music"
        Entry.VIDEOS -> "YouTube"
        Entry.DEVICE -> "Library"
        Entry.NOW_PLAYING -> "Now Playing"
        Entry.RADIO -> "FM Radio"
        Entry.SETTINGS -> "Settings"
    }

    private fun tileFor(e: Entry): Tile = when (e) {
        Entry.MUSIC -> Tiles.music
        Entry.VIDEOS -> Tiles.video
        Entry.DEVICE -> Tiles.device
        Entry.NOW_PLAYING -> Tiles.nowPlaying
        Entry.RADIO -> Tiles.radio
        Entry.SETTINGS -> Tiles.settings
    }

    private fun renderPane() {
        val c = context
        val item = player?.current
        val showPlaying = entry == Entry.NOW_PLAYING && item != null
        art.visible(showPlaying)
        glyphBox.visible(!showPlaying)
        paneProgress.visible(showPlaying)
        if (showPlaying && item != null) {
            paneTitle.text = item.title
            // the artist is about the song, so it stays; the static entries have no hint line
            paneText.text = item.subtitle.substringBefore(" · ")
            paneText.visible(paneText.text.isNotEmpty())
            if (item.thumb != shownArt || art.drawable == null) {
                shownArt = item.thumb
                artColor = null
                ImageLoader.load(art, largeThumb(item.thumb, 300), c.dp(98)) { bmp ->
                    artColor = ArtColor.dominant(bmp)
                    pane.background = paneGradient(artColor!!)
                }
            }
            pane.background = paneGradient(artColor ?: Color.parseColor("#3a3f4a"))
            return
        }
        val tile = tileFor(entry)
        pane.background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(tile.top, tile.bottom, darker(tile.bottom, 0.7f)))
        glyphBox.removeAllViews()
        val glyph: View = when (entry) {
            Entry.MUSIC -> MusicLogo(c)
            Entry.VIDEOS -> VideoLogo(c)
            else -> IconView(c, tile.kind, Color.WHITE)
        }
        val size = if (entry == Entry.VIDEOS) flp(c.dp(80), c.dp(56), Gravity.CENTER) else flp(c.dp(70), c.dp(70), Gravity.CENTER)
        glyphBox.addView(glyph, size)
        glyphBox.background = if (entry == Entry.MUSIC || entry == Entry.VIDEOS) null else
            GradientDrawable().apply {
                setColor(Color.argb(45, 255, 255, 255))
                cornerRadius = c.dpf(22f)
            }
        paneTitle.text = when (entry) {
            Entry.MUSIC -> "YouTube Music"
            Entry.VIDEOS -> "YouTube"
            Entry.DEVICE -> "Library"
            Entry.NOW_PLAYING -> if (FmRadio.state.on) "FM ${fmName(FmRadio.state.frequency)}" else "Nothing playing"
            Entry.RADIO -> if (FmRadio.state.on) "FM ${fmName(FmRadio.state.frequency)}" else "FM Radio"
            Entry.SETTINGS -> "Settings"
        }
        paneText.visible(false) // no hint lines under the title
    }

    private fun paneGradient(color: Int) =
        GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(darker(color, 1.0f), darker(color, 0.45f)))

    override fun onAction(action: WheelAction, steps: Int): Boolean = list.handle(action, steps) || action == WheelAction.Back

    override fun onShow() {
        onTick()
        // whether the server has an account (Accounts.kt): YouTube and YouTube Music open with the
        // right tabs. Offline, the last known answer stands.
        launch { runCatching { Api.health() } }
        // read the card early, so Library opens at once
        launch {
            LocalLibrary.load(context)
            LocalLibrary.readTags(context)
        }
    }

    override fun onTick() {
        status.refresh(player?.isPlaying() == true)
        if (entry == Entry.NOW_PLAYING) renderPane()
        if (entry == Entry.NOW_PLAYING) {
            val p = player ?: return
            val d = p.durationMs()
            paneProgress.fraction = if (d > 0) p.positionMs() / d.toFloat() else 0f
        }
    }
}

fun openNowPlaying(shell: ShellActivity) {
    // the radio is what's playing
    if (FmRadio.state.on && shell.player?.isPlaying() != true) return shell.push(FmScreen(shell))
    val p = shell.player ?: return
    val item = p.current ?: return
    if (p.isVideo) shell.push(VideoPlayerScreen(shell, item)) else shell.push(NowPlayingScreen(shell))
}

fun wifiName(c: Context): String {
    val wifi = c.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    if (!wifi.isWifiEnabled) return "Off"
    return wifi.connectionInfo?.ssid?.trim('"')?.takeIf { it.isNotEmpty() && it != "<unknown ssid>" && it != "0x" } ?: "Not connected"
}
