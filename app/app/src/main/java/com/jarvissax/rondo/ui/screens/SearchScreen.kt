package com.jarvissax.rondo.ui.screens

import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.ui.IconView
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.RowList
import com.jarvissax.rondo.ui.RowStyle
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.TvKeyboard
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.dpf
import com.jarvissax.rondo.ui.flp
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.text
import com.jarvissax.rondo.ui.visible

// Search for YouTube Music songs or YouTube videos, typed on a TV-style keyboard (TvKeyboard).
// Keyboard: Menu/Play/Prev/Next are the arrows, the wheel walks the keys, center types.
// Results: the wheel picks a row, center plays it, Menu goes back to the keyboard.
class SearchScreen(shell: ShellActivity, private val video: Boolean) : Screen(shell) {
    private val query = StringBuilder()
    private lateinit var queryView: TextView
    private lateinit var keyboard: TvKeyboard
    private lateinit var hint: TextView
    private lateinit var results: RowList
    private var showingResults = false
    private lateinit var pager: Pager
    private val found = mutableListOf<com.jarvissax.rondo.net.Item>() // every result loaded so far: the queue when a song is played

    override fun build(): View {
        val c = context
        val root = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Palette.dark)
        }
        val logo = if (video) com.jarvissax.rondo.ui.VideoLogo(c) else com.jarvissax.rondo.ui.MusicLogo(c)
        root.addView(c.darkHeader(if (video) "Search YouTube" else "Search YouTube Music", logo), lp(MATCH, c.dp(28)))

        val bar = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(10), 0, c.dp(10), 0)
            background = com.jarvissax.rondo.ui.rounded(Palette.darkRaised, c.dpf(14f))
        }
        bar.addView(IconView(c, IconView.Kind.SEARCH, Palette.darkMuted), lp(c.dp(12), c.dp(12)).apply { rightMargin = c.dp(8) })
        queryView = c.text(14, Palette.white)
        bar.addView(queryView, lp(0, WRAP, 1f))
        root.addView(bar, lp(MATCH, c.dp(28)).apply { setMargins(c.dp(8), c.dp(2), c.dp(8), c.dp(5)) })

        val area = FrameLayout(c)
        val keys = LinearLayout(c).apply { orientation = LinearLayout.VERTICAL }
        keyboard = TvKeyboard(c)
        keyboard.onKey = { onKey(it) }
        keys.addView(keyboard, lp(MATCH, 0, 1f).apply { setMargins(c.dp(5), 0, c.dp(5), 0) })
        hint = c.text(9, Palette.darkMuted).apply {
            // words, not arrow glyphs: Android 4.2's font may not have them
            text = "Menu up · Play down · Prev left · Next right · wheel: next key · center: type"
            gravity = Gravity.CENTER
        }
        keys.addView(hint, lp(MATCH, WRAP).apply { setMargins(0, c.dp(3), 0, c.dp(4)) })
        area.addView(keys, flp(MATCH, MATCH))
        results = RowList(c, if (video) RowStyle.VIDEO else RowStyle.MUSIC)
        results.visible(false)
        pager = Pager(results, ::launch) { next ->
            val page = Api.more(if (video) "/video/search" else "/music/search", next)
            found += page.items
            rowsFor(page.items) to page.next
        }
        area.addView(results, flp(MATCH, MATCH))
        root.addView(area, lp(MATCH, 0, 1f))
        render()
        return root
    }

    private fun onKey(value: String) {
        when (value) {
            TvKeyboard.DELETE -> if (query.isNotEmpty()) query.setLength(query.length - 1)
            TvKeyboard.SEARCH -> search()
            else -> if (query.length < 60) query.append(value)
        }
        render()
    }

    private fun render() {
        queryView.text = if (showingResults) query.toString() else "$query|"
        if (query.isEmpty() && !showingResults) {
            queryView.text = "|"
        }
    }

    private fun search() {
        val q = query.toString().trim()
        if (q.isEmpty()) return
        showResults(true)
        results.showMessage("Searching “$q”…")
        pager.reset(null)
        launch {
            try {
                val page = if (video) Api.videoSearch(q) else Api.musicSearch(q)
                found.clear()
                found += page.items
                if (page.items.isEmpty()) {
                    results.showMessage("Nothing found for “$q”")
                } else {
                    results.setRows(rowsFor(page.items))
                    pager.reset(page.next)
                }
            } catch (e: Exception) {
                results.showMessage("Search failed: ${e.message}")
            }
        }
    }

    private fun rowsFor(items: List<com.jarvissax.rondo.net.Item>) = if (video) videoRows(shell, items) else musicRows(shell, items) { found.toList() }

    private fun showResults(show: Boolean) {
        showingResults = show
        results.visible(show)
        keyboard.visible(!show)
        hint.visible(!show)
        render()
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean {
        if (showingResults) {
            if (action == WheelAction.Back) {
                showResults(false)
                return true
            }
            return results.handle(action, steps)
        }
        // keyboard open: it takes every control; false means Menu on its top row -> leave
        return keyboard.handle(action)
    }
}
