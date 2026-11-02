package com.jarvissax.rondo.ui.screens

import android.content.Context
import android.graphics.Color
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.google.android.exoplayer2.Player
import com.google.android.exoplayer2.ui.AspectRatioFrameLayout
import com.google.android.exoplayer2.ui.StyledPlayerView
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.net.Item
import com.jarvissax.rondo.net.ShortInfo
import com.jarvissax.rondo.ui.IconView
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.ProgressBarView
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.flp
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.text
import com.jarvissax.rondo.ui.visible

// YouTube Shorts (from YouTube Home's first row): one short at a time, looping, the vertical video
// on the left and what it is on the right. Next and Previous move between shorts (the swipe of the
// app), the wheel is the volume as in every player, center pauses, Menu leaves. The list is the
// server's /video/shorts: popular shorts, then YouTube's endless "watch next" chain. The next short's
// details and stream are asked for while this one plays, so moving on is quick.
class ShortsScreen(shell: ShellActivity) : Screen(shell) {
    private val items = mutableListOf<Item>()
    private val details = HashMap<String, ShortInfo>()
    private var next: String? = null
    private var loadingMore = false
    private var index = -1
    private var savedRepeat: Int? = null
    private var skippedAt = -1

    private lateinit var playerView: StyledPlayerView
    private lateinit var progress: ProgressBarView
    private lateinit var paused: IconView
    private lateinit var waiting: TextView
    private lateinit var title: TextView
    private lateinit var subtitle: TextView
    private lateinit var position: TextView

    // a 9:16 box as tall as the screen
    private class Portrait(context: Context) : FrameLayout(context) {
        override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
            val h = MeasureSpec.getSize(heightMeasureSpec)
            val w = h * 9 / 16
            super.onMeasure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY))
        }
    }

    override fun build(): View {
        val c = context
        val root = LinearLayout(c).apply {
            setBackgroundColor(Palette.dark)
            keepScreenOn = true
        }
        val box = Portrait(c).apply { setBackgroundColor(Color.BLACK) }
        playerView = StyledPlayerView(c).apply {
            useController = false
            resizeMode = AspectRatioFrameLayout.RESIZE_MODE_FIT
            setShutterBackgroundColor(Color.BLACK)
            isFocusable = false
        }
        box.addView(playerView, flp(MATCH, MATCH))
        waiting = c.text(12, Palette.darkMuted).apply { text = "Loading…" }
        box.addView(waiting, flp(WRAP, WRAP, Gravity.CENTER))
        paused = IconView(c, IconView.Kind.PAUSE, Color.argb(220, 255, 255, 255))
        box.addView(paused, flp(c.dp(30), c.dp(30), Gravity.CENTER))
        progress = ProgressBarView(c, Color.argb(80, 255, 255, 255), Palette.red, barDp = 3)
        box.addView(progress, flp(MATCH, c.dp(8), Gravity.BOTTOM))
        root.addView(box, lp(WRAP, MATCH))

        val side = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(c.dp(11), c.dp(7), c.dp(10), c.dp(8))
        }
        val header = LinearLayout(c).apply { gravity = Gravity.CENTER_VERTICAL }
        header.addView(IconView(c, IconView.Kind.SHORTS, Palette.red), lp(c.dp(14), c.dp(14)).apply { rightMargin = c.dp(5) })
        header.addView(c.text(14, Palette.white, bold = true).apply { text = "Shorts" }, lp(0, WRAP, 1f))
        header.addView(c.statusIcons(Palette.darkMuted), lp(WRAP, WRAP))
        side.addView(header, lp(MATCH, WRAP))
        side.addView(View(c), lp(MATCH, 0, 1f))
        title = c.text(15, Palette.white, bold = true, lines = 4)
        side.addView(title, lp(MATCH, WRAP))
        subtitle = c.text(11, Palette.darkMuted, lines = 2)
        side.addView(subtitle, lp(MATCH, WRAP).apply { topMargin = c.dp(4) })
        side.addView(View(c), lp(MATCH, 0, 1f))
        position = c.text(10, Palette.darkMuted)
        side.addView(position, lp(MATCH, WRAP))
        side.addView(c.text(9, Palette.darkFaint, lines = 2).apply {
            text = "Next / Prev: another short\ncenter: pause · wheel: volume"
        }, lp(MATCH, WRAP).apply { topMargin = c.dp(3) })
        root.addView(side, lp(0, MATCH, 1f))
        render()
        load()
        return root
    }

    private fun load() {
        title.text = "Loading Shorts…"
        launch {
            try {
                val page = Api.videoFeed("/video/shorts")
                items += page.items
                next = page.next
                if (items.isEmpty()) return@launch message("No Shorts right now", "Try again later")
                if (index < 0 && player != null) play(0)
            } catch (e: Exception) {
                message("Couldn't load Shorts", context.offlineOr(e.message ?: ""))
            }
        }
    }

    private fun message(head: String, detail: String) {
        title.text = head
        subtitle.text = detail
        waiting.visible(false)
    }

    // more of the chain when four shorts are left
    private fun more() {
        val token = next ?: return
        if (loadingMore) return
        loadingMore = true
        launch {
            try {
                val page = Api.more("/video/shorts", token)
                items += page.items.filter { n -> items.none { it.id == n.id } }
                next = page.next
            } catch (e: Exception) {
                // keep the token: the next short tries again
            } finally {
                loadingMore = false
                render()
            }
        }
    }

    private fun play(i: Int) {
        val p = player ?: return
        if (i !in items.indices) return
        index = i
        val item = items[i]
        if (savedRepeat == null) savedRepeat = p.repeat
        playerView.player = p.player
        p.playVideo(item)
        p.repeat = Player.REPEAT_MODE_ONE // shorts loop, as in the app
        render()
        describe(item)
        items.getOrNull(i + 1)?.let { n ->
            describe(n)
            launch { runCatching { Api.warm(n.id) } }
        }
        if (items.size - i <= 4) more()
    }

    private fun describe(item: Item) {
        if (details.containsKey(item.id)) return
        launch {
            runCatching { Api.shortInfo(item.id) }.onSuccess {
                details[item.id] = it
                if (items.getOrNull(index)?.id == item.id) render()
            }
        }
    }

    private fun render() {
        val item = items.getOrNull(index) ?: return
        val d = details[item.id]
        title.text = d?.title?.ifEmpty { null } ?: item.title.ifEmpty { "…" }
        subtitle.text = d?.subtitle?.ifEmpty { null } ?: item.subtitle
        position.text = "${index + 1} of ${items.size}${if (next != null) "+" else ""}"
    }

    override fun onPlayerReady() {
        if (index < 0 && items.isNotEmpty()) play(0)
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean {
        val p = player
        when (action) {
            WheelAction.Up, WheelAction.Down -> shell.changeVolume(if (action == WheelAction.Down) 1 else -1)
            WheelAction.Next -> play(index + 1)
            WheelAction.Prev -> play(index - 1)
            WheelAction.Select, WheelAction.PlayPause -> p?.togglePlay()
            WheelAction.Back -> return false
            else -> Unit
        }
        onTick()
        return true
    }

    override fun onTick() {
        val p = player ?: return
        if (index < 0) return
        val d = p.durationMs()
        progress.fraction = if (d > 0) p.positionMs() / d.toFloat() else 0f
        val started = p.player.videoSize.width > 0
        waiting.visible(!started || (p.isBuffering() && p.positionMs() == 0L))
        paused.visible(started && !p.isPlaying() && !p.isBuffering())
        // a short that won't play (removed, age-restricted): on to the next one, once
        if (p.player.playerError != null && skippedAt != index) {
            skippedAt = index
            play(index + 1)
        }
    }

    override fun onDestroy() {
        playerView.player = null
        player?.let { p ->
            savedRepeat?.let { p.repeat = it }
            p.stopVideo()
        }
        super.onDestroy()
    }
}
