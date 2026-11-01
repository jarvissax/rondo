package com.jarvissax.rondo.ui.screens

import android.view.View
import android.widget.LinearLayout
import com.google.android.exoplayer2.Player
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.net.Item
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.MusicLogo
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.RowList
import com.jarvissax.rondo.ui.RowStyle
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.lp

// Shared frame for the music sub-screens: header, music-row list on an artwork-tinted
// background, mini player.
abstract class MusicListScreen(shell: ShellActivity, private val title: String) : Screen(shell) {
    protected lateinit var list: RowList
    private lateinit var mini: MiniPlayer
    private lateinit var ambient: Ambient

    override fun build(): View {
        val c = context
        val root = LinearLayout(c).apply { orientation = LinearLayout.VERTICAL }
        ambient = Ambient(root)
        root.addView(c.darkHeader(title, MusicLogo(c)), lp(MATCH, c.dp(31)))
        list = RowList(c, RowStyle.MUSIC)
        list.onSelectionChanged = { ambient.follow(RowStyle.MUSIC, it?.thumb) }
        root.addView(list, lp(MATCH, 0, 1f))
        mini = MiniPlayer(c)
        root.addView(mini, lp(MATCH, c.dp(MiniPlayer.HEIGHT)))
        load()
        return root
    }

    protected abstract fun load()

    override fun onAction(action: WheelAction, steps: Int): Boolean = list.handle(action, steps)
    override fun onShow() = onTick()

    override fun onTick() {
        mini.update(player)
        ambient.follow(RowStyle.MUSIC, list.selectedEntry()?.thumb)
    }
}

// A playlist (pages of 100 appended as you scroll) or an album.
class CollectionScreen(shell: ShellActivity, private val id: String, title: String, private val album: Boolean) : MusicListScreen(shell, title) {
    private val songs = mutableListOf<Item>()

    override fun load() {
        val path = (if (album) "/music/album/" else "/music/playlist/") + java.net.URLEncoder.encode(id, "UTF-8")
        val pager = Pager(list, ::launch) { next ->
            val page = Api.more(path, next)
            songs += page.items
            musicRows(shell, page.items) { songs.toList() } to page.next
        }
        launch {
            try {
                val page = if (album) Api.musicAlbum(id) else Api.musicPlaylist(id)
                songs.clear()
                songs += page.items
                val head = if (songs.size > 1) listOf(
                    Row.Entry("Shuffle all", if (page.next != null) "${songs.size}+ songs" else "${songs.size} songs", onSelect = {
                        player?.playQueue(songs.shuffled(), 0)
                        shell.push(NowPlayingScreen(shell))
                    }),
                ) else emptyList()
                list.setRows(head + musicRows(shell, page.items) { songs.toList() })
                pager.reset(page.next)
            } catch (e: Exception) {
                list.showMessage("Couldn't load: ${e.message}")
            }
        }
    }
}

class ArtistScreen(shell: ShellActivity, private val id: String, title: String) : MusicListScreen(shell, title) {
    override fun load() {
        launch {
            try {
                val page = Api.musicArtist(id)
                list.setRows(page.sections.flatMap { s -> listOf(Row.Header(s.title)) + musicRows(shell, s.items) })
            } catch (e: Exception) {
                list.showMessage("Couldn't load: ${e.message}")
            }
        }
    }
}

// Long-press on a song (LongSelect): what the YouTube Music app offers in its menu.
class SongMenuScreen(shell: ShellActivity, private val song: Item) : MusicListScreen(shell, song.title) {
    private val local = song.id.startsWith("/")

    override fun load() {
        val rows = mutableListOf<Row>(
            Row.Entry("Play now", onSelect = { openMusicItem(shell, song, listOf(song)) }),
            Row.Entry("Play next", onSelect = { enqueue(next = true) }),
            Row.Entry("Add to queue", onSelect = { enqueue(next = false) }),
        )
        if (!local) rows += Row.Entry("Start radio", subtitle = "Songs like this one", onSelect = { startRadio() })
        list.setRows(rows)
    }

    private fun enqueue(next: Boolean) {
        val p = player ?: return
        val ok = if (next) p.playNext(song) else p.addToQueue(song)
        if (ok) shell.pop() else list.showMessage(
            if (local) "Songs on the card can't join an online queue.\nUse Play now to start a new one." else "Online songs can't join a queue from the card.\nUse Play now to start a new one.",
        )
    }

    private fun startRadio() {
        list.showMessage("Starting radio…")
        launch {
            try {
                val next = Api.musicNext(song.id)
                player?.playQueue(listOf(song) + next, 0)
                shell.pop()
                shell.push(NowPlayingScreen(shell))
            } catch (e: Exception) {
                list.showMessage("Couldn't start radio: ${e.message}")
            }
        }
    }
}

// Up next, with shuffle and repeat (NowPlaying long-press).
class QueueScreen(shell: ShellActivity) : MusicListScreen(shell, "Up next") {
    override fun load() = render()

    private fun render() {
        val p = player ?: return list.showMessage("Nothing playing")
        val repeatName = when (p.repeat) {
            Player.REPEAT_MODE_ALL -> "All"
            Player.REPEAT_MODE_ONE -> "One"
            else -> "Off"
        }
        val current = p.currentIndex()
        val rows = mutableListOf<Row>(
            Row.Entry("Shuffle", trailing = if (p.shuffle) "On" else "Off", onSelect = {
                p.shuffle = !p.shuffle
                render()
            }),
            Row.Entry("Repeat", trailing = repeatName, onSelect = {
                p.repeat = when (p.repeat) {
                    Player.REPEAT_MODE_OFF -> Player.REPEAT_MODE_ALL
                    Player.REPEAT_MODE_ALL -> Player.REPEAT_MODE_ONE
                    else -> Player.REPEAT_MODE_OFF
                }
                render()
            }),
            Row.Header("Queue · ${p.queue().size} songs"),
        )
        p.queue().forEachIndexed { i, item ->
            rows += Row.Entry(
                title = item.title,
                subtitle = item.subtitle,
                trailing = if (i == current) "Playing" else durationText(item),
                thumb = item.thumb,
                onSelect = {
                    p.jumpTo(i)
                    shell.pop()
                },
            )
        }
        list.setRows(rows, keepSelection = true)
    }

    override fun onPlayerReady() = render()
}
