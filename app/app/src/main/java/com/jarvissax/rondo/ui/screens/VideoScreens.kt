package com.jarvissax.rondo.ui.screens

import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.google.android.exoplayer2.ui.AspectRatioFrameLayout
import com.google.android.exoplayer2.ui.StyledPlayerView
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.net.Accounts
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.net.Item
import com.jarvissax.rondo.player.QualityPreference
import com.jarvissax.rondo.ui.IconView
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.Pill
import com.jarvissax.rondo.ui.ProgressBarView
import com.jarvissax.rondo.ui.RowList
import com.jarvissax.rondo.ui.RowStyle
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.ScrubBubble
import com.jarvissax.rondo.ui.Scrubber
import com.jarvissax.rondo.ui.VideoLogo
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.dpf
import com.jarvissax.rondo.ui.flp
import com.jarvissax.rondo.ui.formatTime
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.rounded
import com.jarvissax.rondo.ui.text
import com.jarvissax.rondo.ui.visible

// YouTube home (VideoHome.dc.html, redesigned 2026-10-30): pill tabs Home / Subscriptions /
// History / Search, thumbnail rows on a background tinted by the selected thumbnail; every feed
// scrolls forever. The pills are reached by turning the wheel up past the first row.
class VideoScreen(shell: ShellActivity) : Screen(shell) {
    // Subscriptions and History only when the server has an account (Accounts.kt)
    private val tabs = if (Accounts.videos) listOf("Home", "Subscriptions", "History") else listOf("Home")
    private val feeds = listOf("/video/home", "/video/subscriptions", "/video/history")
    private val searchIndex get() = tabs.size
    private var activeTab = 0
    private var focusedTab: Int? = null // the pills, then the search button at searchIndex
    private val pills = mutableListOf<Pill>()
    private lateinit var searchPill: FrameLayout
    private lateinit var list: RowList
    private lateinit var ambient: Ambient
    private lateinit var pager: Pager

    override fun build(): View {
        val c = context
        val root = LinearLayout(c).apply { orientation = LinearLayout.VERTICAL }
        ambient = Ambient(root)
        val header = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(10), 0, c.dp(10), 0)
        }
        header.addView(VideoLogo(c), lp(c.dp(22), c.dp(15)).apply { rightMargin = c.dp(9) })
        tabs.forEach { name ->
            val p = Pill(c, name)
            pills += p
            header.addView(p, lp(WRAP, c.dp(21)).apply { rightMargin = c.dp(5) })
        }
        searchPill = FrameLayout(c)
        searchPill.addView(IconView(c, IconView.Kind.SEARCH, Palette.white), flp(c.dp(11), c.dp(11), Gravity.CENTER))
        header.addView(searchPill, lp(c.dp(21), c.dp(21)))
        header.addView(View(c), lp(0, 1, 1f))
        // battery only, at least 10dp from the search button: the pills leave no room for Wi-Fi
        header.addView(c.statusIcons(Palette.darkMuted, wifi = false), lp(WRAP, WRAP).apply { leftMargin = c.dp(10) })
        root.addView(header, lp(MATCH, c.dp(31)))

        list = RowList(c, RowStyle.VIDEO)
        list.onTopReached = {
            focusedTab = activeTab
            renderTabs()
            true
        }
        list.onSelectionChanged = { ambient.follow(RowStyle.VIDEO, it?.thumb) }
        pager = Pager(list, ::launch) { next ->
            val page = Api.more(feeds[activeTab], next)
            videoRows(shell, page.items) to page.next
        }
        root.addView(list, lp(MATCH, 0, 1f))
        renderTabs()
        load(0)
        return root
    }

    private fun renderTabs() {
        list.active = focusedTab == null
        pills.forEachIndexed { i, p ->
            p.active = i == activeTab
            p.focusedRing = i == focusedTab
        }
        searchPill.background = rounded(Color.argb(60, 255, 255, 255), context.dpf(10.5f), if (focusedTab == searchIndex) Palette.red else null, context.dp(2))
    }

    private fun load(tab: Int) {
        activeTab = tab
        renderTabs()
        list.showMessage("Loading…")
        pager.reset(null)
        launch {
            try {
                val page = Api.videoFeed(feeds[tab])
                list.setRows(videoRows(shell, page.items))
                pager.reset(page.next)
            } catch (e: Exception) {
                list.showMessage(context.offlineOr("Couldn't load ${tabs[tab]}: ${e.message}"))
            }
        }
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean {
        val f = focusedTab ?: return if (list.isEmpty && action == WheelAction.Select) {
            load(activeTab)
            true
        } else {
            list.handle(action, steps)
        }
        when (action) {
            WheelAction.Up -> focusedTab = (f + searchIndex) % (searchIndex + 1)
            WheelAction.Down -> focusedTab = (f + 1) % (searchIndex + 1)
            WheelAction.Select -> {
                focusedTab = null
                if (f == searchIndex) shell.push(SearchScreen(shell, video = true)) else load(f)
            }
            else -> return false
        }
        renderTabs()
        return true
    }

    override fun onTick() = ambient.follow(RowStyle.VIDEO, list.selectedEntry()?.thumb)
}

// Full-screen video (VideoPlayer.dc.html): 16:9 letterboxed, overlays for 3 s after any input.
// Wheel = volume. Center = scrub (the wheel then moves through the video in steps that scale
// with its length, center again jumps); Next/Prev = ±10 s, or ±10 % while scrubbing; holding
// Next/Prev seeks faster the longer it is held; holding center = quality; Menu leaves.
// The screen stays on only here (everything else lets it sleep to save battery).
class VideoPlayerScreen(shell: ShellActivity, private val item: Item) : Screen(shell) {
    private lateinit var playerView: StyledPlayerView
    private lateinit var top: LinearLayout
    private lateinit var bottom: LinearLayout
    private lateinit var progress: ProgressBarView
    private lateinit var time: TextView
    private lateinit var stateIcon: IconView
    private lateinit var quality: TextView
    private lateinit var hint: TextView
    private lateinit var panel: LinearLayout
    private lateinit var bubble: ScrubBubble
    private val panelRows = mutableListOf<Pair<View, IconView>>()
    private val scrubber = Scrubber { player }
    private var panelCursor = 0
    private var started = false
    private var overlayUntil = 0L
    private val local = item.id.startsWith("/")

    private val choices: List<Int?> = listOf(null, 720, 480, 360)

    override fun build(): View {
        val c = context
        val root = FrameLayout(c).apply {
            setBackgroundColor(Color.BLACK)
            keepScreenOn = true
        }
        playerView = StyledPlayerView(c).apply {
            useController = false
            resizeMode = AspectRatioFrameLayout.RESIZE_MODE_FIT
            setShutterBackgroundColor(Color.BLACK)
            isFocusable = false
        }
        root.addView(playerView, flp(MATCH, MATCH))
        if (started) playerView.player = player?.player // rebuilt (theme change) while playing

        top = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(Color.argb(215, 0, 0, 0), Color.TRANSPARENT))
            setPadding(c.dp(10), c.dp(7), c.dp(10), c.dp(14))
        }
        top.addView(c.text(13, Palette.white, bold = true, lines = 2).apply { text = item.title }, lp(MATCH, WRAP))
        top.addView(c.text(10, Color.argb(200, 255, 255, 255)).apply { text = item.subtitle }, lp(MATCH, WRAP).apply { topMargin = c.dp(2) })
        root.addView(top, flp(MATCH, WRAP, Gravity.TOP))

        bottom = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.BOTTOM
            background = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(Color.TRANSPARENT, Color.argb(225, 0, 0, 0)))
            setPadding(c.dp(8), c.dp(16), c.dp(10), c.dp(5))
        }
        progress = ProgressBarView(c, Color.argb(90, 255, 255, 255), Palette.red, Color.argb(130, 255, 255, 255), thumbDp = 9, barDp = 3)
        bottom.addView(progress, lp(MATCH, c.dp(12)))
        val row = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(4), 0, 0, 0)
        }
        stateIcon = IconView(c, IconView.Kind.PLAY, Palette.white)
        row.addView(stateIcon, lp(c.dp(9), c.dp(9)).apply { rightMargin = c.dp(7) })
        time = c.text(11, Palette.white)
        row.addView(time, lp(WRAP, WRAP))
        hint = c.text(10, Color.argb(170, 255, 255, 255)).apply { gravity = Gravity.CENTER }
        row.addView(hint, lp(0, WRAP, 1f).apply { setMargins(c.dp(8), 0, c.dp(8), 0) })
        quality = c.text(10, Palette.white, bold = true).apply {
            background = rounded(Color.argb(70, 255, 255, 255), c.dpf(3f))
            setPadding(c.dp(4), c.dp(1), c.dp(4), c.dp(1))
        }
        row.addView(quality, lp(WRAP, WRAP))
        bottom.addView(row, lp(MATCH, WRAP).apply { topMargin = c.dp(2) })
        root.addView(bottom, flp(MATCH, WRAP, Gravity.BOTTOM))

        panel = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, c.dp(5), 0, c.dp(5))
            background = rounded(Color.argb(240, 33, 33, 33), c.dpf(8f))
        }
        panel.addView(c.text(10, Palette.darkMuted, bold = true).apply {
            text = "QUALITY"
            setPadding(c.dp(12), c.dp(3), c.dp(12), c.dp(5))
        }, lp(MATCH, WRAP))
        choices.forEach { h ->
            val r = LinearLayout(c).apply {
                gravity = Gravity.CENTER_VERTICAL
                setPadding(c.dp(12), 0, c.dp(12), 0)
            }
            r.addView(c.text(13, Palette.white).apply { text = h?.let { "${it}p" } ?: "Auto" }, lp(0, WRAP, 1f))
            val check = IconView(c, IconView.Kind.CHECK, Palette.white)
            r.addView(check, lp(c.dp(12), c.dp(10)))
            panel.addView(r, lp(MATCH, c.dp(26)))
            panelRows += r to check
        }
        panel.visible(false)
        root.addView(panel, flp(c.dp(130), WRAP, Gravity.CENTER))

        bubble = ScrubBubble(c)
        root.addView(bubble, flp(WRAP, WRAP, Gravity.CENTER))
        showOverlay()
        return root
    }

    override fun onPlayerReady() = start()
    override fun onShow() = start()

    private fun start() {
        val p = player ?: return
        playerView.player = p.player
        if (!started) {
            started = true
            p.playVideo(item)
        }
        onTick()
    }

    private fun showOverlay() {
        overlayUntil = System.currentTimeMillis() + 3_000
    }

    private fun renderPanel() {
        val current = QualityPreference.height
        panelRows.forEachIndexed { i, (r, check) ->
            r.background = if (i == panelCursor) rounded(Color.argb(70, 255, 255, 255), context.dpf(4f)) else null
            check.visibility = if (choices[i] == current) View.VISIBLE else View.INVISIBLE
        }
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean {
        val p = player
        showOverlay()
        if (panel.visibility == View.VISIBLE) {
            when (action) {
                WheelAction.Up -> panelCursor = (panelCursor + choices.size - 1) % choices.size
                WheelAction.Down -> panelCursor = (panelCursor + 1) % choices.size
                WheelAction.Select -> {
                    p?.setQuality(choices[panelCursor])
                    panel.visible(false)
                }
                WheelAction.Back -> panel.visible(false)
                else -> return false
            }
            renderPanel()
            onTick()
            return true
        }
        val scrubbing = scrubber.active
        when (action) {
            WheelAction.Up, WheelAction.Down -> {
                val dir = if (action == WheelAction.Down) 1 else -1
                if (scrubbing) scrubber.turn(dir, steps) else shell.changeVolume(dir)
            }
            WheelAction.Select -> if (scrubbing) scrubber.commit() else scrubber.begin()
            WheelAction.LongSelect -> if (!local) {
                scrubber.cancel()
                panelCursor = choices.indexOf(QualityPreference.height).coerceAtLeast(0)
                renderPanel()
                panel.visible(true)
            }
            WheelAction.Back -> if (scrubbing) scrubber.cancel() else return false
            WheelAction.Next -> if (scrubbing) scrubber.jump(+1) else p?.seekBy(10_000)
            WheelAction.Prev -> if (scrubbing) scrubber.jump(-1) else p?.seekBy(-10_000)
            WheelAction.LongNext -> scrubber.hold(+1)
            WheelAction.LongPrev -> scrubber.hold(-1)
            WheelAction.SeekEnd -> scrubber.commit()
            WheelAction.PlayPause -> p?.togglePlay()
        }
        onTick()
        return true
    }

    override fun onTick() {
        val p = player ?: return
        scrubber.tick()
        val exo = p.player
        val d = p.durationMs()
        val pos = p.positionMs()
        progress.fraction = if (d > 0) pos / d.toFloat() else 0f
        progress.buffered = if (d > 0) p.bufferedMs() / d.toFloat() else 0f
        progress.marker = scrubber.fraction
        time.text = "${formatTime(if (scrubber.active) scrubber.target else pos)} / ${formatTime(d)}"
        if (scrubber.active) bubble.show(scrubber.target, scrubber.from) else bubble.hide()
        stateIcon.kind = if (p.isPlaying()) IconView.Kind.PLAY else IconView.Kind.PAUSE
        stateIcon.invalidate()
        hint.text = when {
            scrubber.active -> "center jumps · Menu cancels"
            local -> "center: scrub"
            else -> "center: scrub · hold center: quality"
        }
        val h = exo.videoFormat?.height
        quality.text = when {
            local -> h?.let { "${it}p" } ?: ""
            QualityPreference.height == null && h != null -> "Auto · ${h}p"
            h != null -> "${h}p"
            else -> ""
        }
        quality.visible(quality.text.isNotEmpty())
        val keep = panel.visibility == View.VISIBLE || scrubber.active || !p.isPlaying() || p.isBuffering()
        val show = keep || System.currentTimeMillis() < overlayUntil
        top.visible(show)
        bottom.visible(show)
    }

    override fun onDestroy() {
        playerView.player = null
        player?.stopVideo()
        super.onDestroy()
    }
}
