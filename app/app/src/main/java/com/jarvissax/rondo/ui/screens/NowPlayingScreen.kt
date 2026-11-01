package com.jarvissax.rondo.ui.screens

import android.graphics.Color
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import com.google.android.exoplayer2.Player
import com.jarvissax.rondo.Config
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.ui.ArtColor
import com.jarvissax.rondo.player.LyricsSource
import com.jarvissax.rondo.ui.IconView
import com.jarvissax.rondo.ui.LyricsView
import com.jarvissax.rondo.ui.isOnline
import com.jarvissax.rondo.ui.ImageLoader
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.ProgressBarView
import com.jarvissax.rondo.ui.RoundedImageView
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.ScrubBubble
import com.jarvissax.rondo.ui.Scrubber
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.dpf
import com.jarvissax.rondo.ui.flp
import com.jarvissax.rondo.ui.formatTime
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.rounded
import com.jarvissax.rondo.ui.show
import com.jarvissax.rondo.ui.text
import com.jarvissax.rondo.ui.visible

// Music now playing (NowPlaying.dc.html, redesigned 2026-10-30): the artwork over a blurred
// field of itself. Like an iPod the wheel is volume. Center switches to the lyrics and back;
// holding center opens options (scrub with the wheel, up next, shuffle, repeat); holding
// Next/Prev seeks faster the longer it is held (see Scrubber).
class NowPlayingScreen(shell: ShellActivity) : Screen(shell) {
    private lateinit var backdrop: ImageView
    private lateinit var art: RoundedImageView
    private lateinit var position: TextView
    private lateinit var title: TextView
    private lateinit var artist: TextView
    private lateinit var album: TextView
    private lateinit var nextTitle: TextView
    private lateinit var shuffle: IconView
    private lateinit var repeat: IconView
    private lateinit var source: TextView
    private lateinit var progress: ProgressBarView
    private lateinit var elapsed: TextView
    private lateinit var remaining: TextView
    private lateinit var stateIcon: IconView
    private lateinit var stateText: TextView
    private lateinit var bubble: ScrubBubble
    private val scrubber = Scrubber { player }
    private var shownId: String? = null
    private lateinit var page: LinearLayout
    private lateinit var lyricsPage: LinearLayout
    private lateinit var lyrics: LyricsView
    private lateinit var lyricsArt: RoundedImageView
    private lateinit var lyricsTitle: TextView
    private lateinit var lyricsArtist: TextView
    private lateinit var lyricsProgress: ProgressBarView
    private var lyricsFor: String? = null // song id the lyrics view holds
    private var opened = false
    private lateinit var options: LinearLayout
    private var optionCursor = 0
    private var artRetry = 0

    override fun build(): View {
        // a rebuild (theme change) makes new views: they must load their artwork and lyrics again
        shownId = null
        lyricsFor = null
        val c = context
        val root = FrameLayout(c).apply { setBackgroundColor(Palette.dark) }
        backdrop = ImageView(c).apply { scaleType = ImageView.ScaleType.FIT_XY }
        root.addView(backdrop, flp(MATCH, MATCH))
        root.addView(View(c).apply { setBackgroundColor(Color.argb(120, 0, 0, 0)) }, flp(MATCH, MATCH))

        val page = LinearLayout(c).apply { orientation = LinearLayout.VERTICAL }
        val header = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(12), 0, c.dp(10), 0)
        }
        header.addView(c.text(11, Color.argb(200, 255, 255, 255), bold = true).apply { text = "NOW PLAYING" }, lp(WRAP, WRAP))
        position = c.text(11, Color.argb(160, 255, 255, 255))
        header.addView(position, lp(0, WRAP, 1f).apply { leftMargin = c.dp(8) })
        header.addView(c.statusIcons(Color.argb(200, 255, 255, 255)), lp(WRAP, WRAP))
        page.addView(header, lp(MATCH, c.dp(24)))

        val body = LinearLayout(c).apply { setPadding(c.dp(12), c.dp(4), c.dp(12), 0) }
        art = RoundedImageView(c, c.dpf(6f), Color.argb(70, 255, 255, 255))
        val artFrame = FrameLayout(c)
        artFrame.addView(IconView(c, IconView.Kind.NOTE, Color.argb(120, 255, 255, 255)), flp(c.dp(44), c.dp(44), Gravity.CENTER))
        artFrame.addView(art, flp(MATCH, MATCH))
        body.addView(artFrame, lp(c.dp(140), c.dp(140)).apply { rightMargin = c.dp(14) })

        val column = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, c.dp(2), 0, 0)
        }
        title = c.text(17, Palette.white, bold = true, lines = 2)
        artist = c.text(13, Color.argb(230, 255, 255, 255))
        album = c.text(11, Color.argb(160, 255, 255, 255))
        column.addView(title, lp(MATCH, WRAP))
        column.addView(artist, lp(MATCH, WRAP).apply { topMargin = c.dp(4) })
        column.addView(album, lp(MATCH, WRAP).apply { topMargin = c.dp(2) })
        column.addView(View(c), lp(MATCH, 0, 1f))
        column.addView(c.text(9, Color.argb(150, 255, 255, 255), bold = true).apply { text = "UP NEXT" }, lp(MATCH, WRAP))
        nextTitle = c.text(11, Color.argb(220, 255, 255, 255))
        column.addView(nextTitle, lp(MATCH, WRAP).apply { topMargin = c.dp(1) })
        val modes = LinearLayout(c).apply { gravity = Gravity.CENTER_VERTICAL }
        shuffle = IconView(c, IconView.Kind.SHUFFLE, Palette.darkFaint)
        repeat = IconView(c, IconView.Kind.REPEAT, Palette.darkFaint)
        modes.addView(shuffle, lp(c.dp(15), c.dp(13)).apply { rightMargin = c.dp(12) })
        modes.addView(repeat, lp(c.dp(15), c.dp(13)))
        source = c.text(10, Color.argb(170, 255, 255, 255)).apply { gravity = Gravity.END }
        modes.addView(source, lp(0, WRAP, 1f))
        column.addView(modes, lp(MATCH, WRAP).apply { topMargin = c.dp(8) })
        body.addView(column, lp(0, c.dp(140), 1f))
        page.addView(body, lp(MATCH, 0, 1f))

        val bottom = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(c.dp(10), 0, c.dp(10), c.dp(6))
        }
        progress = ProgressBarView(c, Color.argb(70, 255, 255, 255), Palette.white, thumbDp = 9, barDp = 3)
        bottom.addView(progress, lp(MATCH, c.dp(12)))
        val times = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(4), 0, c.dp(4), 0)
        }
        elapsed = c.text(11, Color.argb(220, 255, 255, 255))
        remaining = c.text(11, Color.argb(220, 255, 255, 255)).apply { gravity = Gravity.END }
        val state = LinearLayout(c).apply { gravity = Gravity.CENTER }
        stateIcon = IconView(c, IconView.Kind.PLAY, Palette.white)
        stateText = c.text(11, Palette.white)
        state.addView(stateIcon, lp(c.dp(9), c.dp(9)).apply { rightMargin = c.dp(5) })
        state.addView(stateText, lp(WRAP, WRAP))
        times.addView(elapsed, lp(0, WRAP, 1f))
        times.addView(state, lp(0, WRAP, 2f))
        times.addView(remaining, lp(0, WRAP, 1f))
        bottom.addView(times, lp(MATCH, WRAP).apply { topMargin = c.dp(2) })
        page.addView(bottom, lp(MATCH, WRAP))
        root.addView(page, flp(MATCH, MATCH))

        this.page = page
        lyricsPage = buildLyricsPage()
        root.addView(lyricsPage, flp(MATCH, MATCH))

        bubble = ScrubBubble(c)
        root.addView(bubble, flp(WRAP, WRAP, Gravity.CENTER))
        options = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, c.dp(5), 0, c.dp(5))
            background = rounded(Color.argb(242, 30, 30, 32), c.dpf(10f))
            visible(false)
        }
        root.addView(options, flp(c.dp(190), WRAP, Gravity.CENTER))
        // "Open lyrics automatically" (dashboard) applies when Now Playing opens, not on a redraw
        showLyrics(lyricsOn || (!opened && Config.behavior.lyricsOnOpen))
        opened = true
        return root
    }

    // Hold center: the less frequent things, one small panel (wheel picks, center chooses, Menu closes).
    private fun optionRows(): List<Pair<String, () -> Unit>> {
        val p = player
        val repeatName = when (p?.repeat) {
            Player.REPEAT_MODE_ALL -> "All"
            Player.REPEAT_MODE_ONE -> "One"
            else -> "Off"
        }
        return listOf(
            "Scrub through the song" to { closeOptions(); scrubber.begin(); Unit },
            "Up next" to { closeOptions(); shell.push(QueueScreen(shell)) },
            "Shuffle: ${if (p?.shuffle == true) "On" else "Off"}" to { p?.let { it.shuffle = !it.shuffle }; renderOptions() },
            "Repeat: $repeatName" to {
                p?.let {
                    it.repeat = when (it.repeat) {
                        Player.REPEAT_MODE_OFF -> Player.REPEAT_MODE_ALL
                        Player.REPEAT_MODE_ALL -> Player.REPEAT_MODE_ONE
                        else -> Player.REPEAT_MODE_OFF
                    }
                }
                renderOptions()
            },
        )
    }

    private fun openOptions() {
        scrubber.cancel()
        optionCursor = 0
        options.visible(true)
        renderOptions()
    }

    private fun closeOptions() = options.visible(false)

    private fun renderOptions() {
        val c = context
        options.removeAllViews()
        optionRows().forEachIndexed { i, (label, _) ->
            val on = i == optionCursor
            options.addView(c.text(13, if (on) Palette.dark else Palette.white, bold = on).apply {
                text = label
                gravity = Gravity.CENTER_VERTICAL
                setPadding(c.dp(12), 0, c.dp(12), 0)
                background = if (on) rounded(Palette.white, c.dpf(6f)) else null
            }, lp(MATCH, c.dp(28)).apply { setMargins(c.dp(5), 0, c.dp(5), 0) })
        }
    }

    // Lyrics page: a slim header with the song, the lyrics, a thin progress line.
    private fun buildLyricsPage(): LinearLayout {
        val c = context
        val p = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(90, 0, 0, 0)) // extra dimming: lyrics must read on any artwork
        }
        val header = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(12), 0, c.dp(10), 0)
        }
        lyricsArt = RoundedImageView(c, c.dpf(3f), Color.argb(70, 255, 255, 255))
        header.addView(lyricsArt, lp(c.dp(22), c.dp(22)).apply { rightMargin = c.dp(8) })
        val names = LinearLayout(c).apply { orientation = LinearLayout.VERTICAL }
        lyricsTitle = c.text(12, Palette.white, bold = true)
        lyricsArtist = c.text(10, Color.argb(170, 255, 255, 255))
        names.addView(lyricsTitle, lp(MATCH, WRAP))
        names.addView(lyricsArtist, lp(MATCH, WRAP))
        header.addView(names, lp(0, WRAP, 1f))
        header.addView(c.text(9, Color.argb(150, 255, 255, 255), bold = true).apply { text = "LYRICS" }, lp(WRAP, WRAP).apply { leftMargin = c.dp(8) })
        p.addView(header, lp(MATCH, c.dp(32)))
        lyrics = LyricsView(c, { player?.positionMs() ?: 0L }, { player?.durationMs() ?: 0L })
        p.addView(lyrics, lp(MATCH, 0, 1f))
        lyricsProgress = ProgressBarView(c, Color.argb(60, 255, 255, 255), Palette.white, barDp = 2)
        p.addView(lyricsProgress, lp(MATCH, c.dp(10)).apply { setMargins(c.dp(6), 0, c.dp(6), c.dp(4)) })
        return p
    }

    private fun showLyrics(on: Boolean) {
        lyricsOn = on
        page.visible(!on)
        lyricsPage.visible(on)
        if (on) loadLyrics()
    }

    private fun loadLyrics() {
        val item = player?.current ?: return lyrics.showMessage("Nothing playing")
        if (item.id == lyricsFor) return
        lyricsFor = item.id
        lyrics.showMessage("Finding lyrics…")
        launch {
            try {
                val found = LyricsSource.find(item)
                if (lyricsFor == item.id) lyrics.show(found)
            } catch (e: Exception) {
                if (lyricsFor != item.id) return@launch
                lyricsFor = null // try again next time
                lyrics.showMessage(if (!isOnline(context)) "Lyrics need Wi-Fi.\nConnect in Settings › Wi-Fi." else "Couldn't get lyrics: ${e.message}")
            }
        }
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean {
        if (options.visibility == View.VISIBLE) {
            val rows = optionRows()
            when (action) {
                WheelAction.Up -> optionCursor = (optionCursor + rows.size - 1) % rows.size
                WheelAction.Down -> optionCursor = (optionCursor + 1) % rows.size
                WheelAction.Select -> rows[optionCursor].second()
                WheelAction.Back, WheelAction.LongSelect -> closeOptions()
                else -> return false
            }
            if (options.visibility == View.VISIBLE) renderOptions()
            render()
            return true
        }
        val scrubbing = scrubber.active
        when (action) {
            WheelAction.Up, WheelAction.Down -> {
                val dir = if (action == WheelAction.Down) 1 else -1
                when {
                    scrubbing -> scrubber.turn(dir, steps)
                    lyricsOn && lyrics.scrollable -> lyrics.scrollBy(dir * steps)
                    else -> shell.changeVolume(dir)
                }
            }
            // center: Now Playing <-> lyrics (or, while scrubbing, jump)
            WheelAction.Select -> when {
                scrubbing -> scrubber.commit()
                else -> showLyrics(!lyricsOn)
            }
            WheelAction.LongSelect -> openOptions()
            WheelAction.Back -> when {
                scrubbing -> scrubber.cancel()
                lyricsOn -> showLyrics(false)
                else -> return false
            }
            WheelAction.Next -> if (scrubbing) scrubber.jump(+1) else return false
            WheelAction.Prev -> if (scrubbing) scrubber.jump(-1) else return false
            WheelAction.LongNext -> scrubber.hold(+1)
            WheelAction.LongPrev -> scrubber.hold(-1)
            WheelAction.SeekEnd -> scrubber.commit()
            else -> return false
        }
        render()
        return true
    }

    override fun onShow() = render()

    override fun onTick() {
        scrubber.tick()
        render()
    }

    private fun render() {
        val p = player ?: return
        val item = p.current
        if (item == null) {
            title.show("Nothing playing")
            artist.show("")
            album.show("")
            return
        }
        val parts = item.subtitle.split(" · ")
        val d = p.durationMs()
        // artwork and the blurred backdrop behind both pages; retried every few seconds while
        // missing (a fetch can fail on a Wi-Fi drop)
        if (item.id != shownId || (art.drawable == null && ++artRetry % 6 == 0)) {
            shownId = item.id
            backdrop.setImageDrawable(null)
            ImageLoader.load(art, largeThumb(item.thumb, 400), context.dp(140)) { bmp ->
                backdrop.setImageBitmap(ArtColor.blurred(bmp))
            }
        }
        if (scrubber.active) bubble.show(scrubber.target, scrubber.from) else bubble.hide()
        if (lyricsOn) {
            // only the page on screen is updated: the hidden one would still cost layout passes
            if (item.id != lyricsFor) loadLyrics() // next song
            lyricsTitle.show(item.title)
            lyricsArtist.show(parts.firstOrNull() ?: "")
            ImageLoader.load(lyricsArt, item.thumb, context.dp(22), context.dp(22))
            lyricsProgress.fraction = if (d > 0) p.positionMs() / d.toFloat() else 0f
            lyricsProgress.marker = scrubber.fraction
            return
        }
        title.show(item.title)
        artist.show(parts.firstOrNull() ?: "")
        album.show(parts.drop(1).joinToString(" · "))
        val queue = p.queue()
        position.show(if (queue.size > 1) "${p.currentIndex() + 1} of ${queue.size}" else "")
        shuffle.color = if (p.shuffle) Palette.white else Color.argb(90, 255, 255, 255)
        repeat.color = if (p.repeat != Player.REPEAT_MODE_OFF) Palette.white else Color.argb(90, 255, 255, 255)
        shuffle.invalidate()
        repeat.invalidate()
        source.show(if (p.isLocal) item.id.substringAfterLast('.').uppercase() + " · Library" else "Streaming · AAC")
        nextTitle.show(p.upNext?.let { "${it.title} · ${it.subtitle.substringBefore(" · ")}" } ?: "—")

        val pos = if (scrubber.active) scrubber.target else p.positionMs()
        progress.fraction = if (d > 0) p.positionMs() / d.toFloat() else 0f
        progress.marker = scrubber.fraction
        elapsed.show(formatTime(pos))
        // whole seconds of length minus whole seconds played: elapsed and remaining tick together
        // and always add up (from exact milliseconds, remaining ticked early and seemed to drop 2 s)
        remaining.show("-" + formatTime(((d / 1000 - pos / 1000) * 1000).coerceAtLeast(0)))
        val playing = p.isPlaying()
        stateIcon.kind = if (playing) IconView.Kind.PLAY else IconView.Kind.PAUSE
        stateIcon.invalidate()
        stateIcon.visible(!scrubber.active)
        stateText.show(
            when {
                scrubber.active -> "Turn to move · center jumps"
                p.isBuffering() -> "Loading"
                playing -> "Playing"
                else -> "Paused"
            },
        )
    }
}

// the lyrics page stays open for the next visit to Now Playing
private var lyricsOn = false
