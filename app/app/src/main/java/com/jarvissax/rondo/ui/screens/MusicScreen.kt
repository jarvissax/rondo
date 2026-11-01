package com.jarvissax.rondo.ui.screens

import android.graphics.Color
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.net.Accounts
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.net.Section
import com.jarvissax.rondo.ui.IconView
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.MusicLogo
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.Pill
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.RowList
import com.jarvissax.rondo.ui.RowStyle
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.dpf
import com.jarvissax.rondo.ui.flp
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.rounded
import com.jarvissax.rondo.ui.text

// YouTube Music (MusicHome.dc.html, redesigned 2026-10-30): logo, pill tabs, a background tinted
// by the selected artwork, mini player. The pills are reached by turning the wheel up past the
// first row; there the wheel moves between them and center opens one. Home scrolls forever.
class MusicScreen(shell: ShellActivity) : Screen(shell) {
    // Library only when the server has an account (Accounts.kt); signed out it's Home and search
    private val tabNames = if (Accounts.music) listOf("Home", "Library") else listOf("Home")
    private val searchIndex get() = tabNames.size
    private var activeTab = 0
    private var focusedTab: Int? = null // the pills, then the search button at searchIndex
    private val pills = mutableListOf<Pill>()
    private lateinit var searchPill: FrameLayout
    private lateinit var list: RowList
    private lateinit var mini: MiniPlayer
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
        header.addView(MusicLogo(c), lp(c.dp(18), c.dp(18)).apply { rightMargin = c.dp(6) })
        header.addView(c.text(15, Palette.white, bold = true).apply { text = "Music" }, lp(WRAP, WRAP).apply { rightMargin = c.dp(10) })
        tabNames.forEach { name ->
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

        list = RowList(c, RowStyle.MUSIC)
        list.onTopReached = {
            focusedTab = activeTab
            renderTabs()
            true
        }
        list.onSelectionChanged = { ambient.follow(RowStyle.MUSIC, it?.thumb) }
        pager = Pager(list, ::launch) { next ->
            val page = Api.moreSections("/music/home", next)
            sectionRows(page.sections) to page.next
        }
        root.addView(list, lp(MATCH, 0, 1f))
        mini = MiniPlayer(c)
        root.addView(mini, lp(MATCH, c.dp(MiniPlayer.HEIGHT)))
        renderTabs()
        load(0)
        return root
    }

    private fun sectionRows(sections: List<Section>): List<Row> =
        sections.flatMap { s -> listOf(Row.Header(s.title)) + musicRows(shell, s.items) }

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
                if (tab == 0) {
                    val home = Api.musicHome()
                    list.setRows(sectionRows(home.sections))
                    pager.reset(home.next)
                } else {
                    list.setRows(listOf(Row.Header("Playlists")) + musicRows(shell, Api.musicLibrary().playlists))
                }
            } catch (e: Exception) {
                list.showMessage(context.offlineOr("Couldn't load ${tabNames[tab]}: ${e.message}"))
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
                if (f == searchIndex) shell.push(SearchScreen(shell, video = false)) else load(f)
            }
            else -> return false
        }
        renderTabs()
        return true
    }

    override fun onShow() = onTick()

    override fun onTick() {
        mini.update(player)
        ambient.follow(RowStyle.MUSIC, list.selectedEntry()?.thumb) // artwork that arrived after the selection
    }
}
