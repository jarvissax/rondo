package com.jarvissax.rondo.ui.screens

import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.local.LocalLibrary
import com.jarvissax.rondo.local.Playlists
import com.jarvissax.rondo.net.Item
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.RowList
import com.jarvissax.rondo.ui.RowStyle
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.ShellStatusBar
import com.jarvissax.rondo.ui.Tiles
import com.jarvissax.rondo.ui.TvKeyboard
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.dpf
import com.jarvissax.rondo.ui.flp
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.rounded
import com.jarvissax.rondo.ui.text
import com.jarvissax.rondo.ui.visible

// Library › Search: the card's songs, videos, artists and albums, matched as you type (like the
// dashboard's search box). Show lists them; Menu goes back to the keyboard. With `addTo`, it picks
// songs for that playlist instead: center adds the song (or a whole artist or album).
class LibrarySearchScreen(shell: ShellActivity, private val addTo: String? = null) : Screen(shell) {
    private val query = StringBuilder()
    private lateinit var field: TextView
    private lateinit var summary: TextView
    private lateinit var keyboard: TvKeyboard
    private lateinit var hint: TextView
    private lateinit var results: RowList
    private var showing = false
    private var found = LocalLibrary.search("")
    private val added = HashSet<String>()
    private val adding = HashSet<String>() // writing to the card takes a second or two

    override fun build(): View {
        val c = context
        val root = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Palette.shellBg)
        }
        root.addView(ShellStatusBar(c, if (addTo != null) "Add to $addTo" else "Search"), lp(MATCH, c.dp(22)))
        val bar = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(10), 0, c.dp(10), 0)
            background = rounded(Palette.shellHeader, c.dpf(8f), Palette.shellRowLine, 1)
        }
        bar.addView(com.jarvissax.rondo.ui.IconView(c, com.jarvissax.rondo.ui.IconView.Kind.SEARCH, Palette.shellMuted), lp(c.dp(11), c.dp(11)).apply { rightMargin = c.dp(7) })
        field = c.text(14, Palette.shellText)
        bar.addView(field, lp(0, WRAP, 1f))
        root.addView(bar, lp(MATCH, c.dp(26)).apply { setMargins(c.dp(8), c.dp(5), c.dp(8), 0) })
        summary = c.text(10, Palette.shellMuted).apply { gravity = Gravity.CENTER }
        root.addView(summary, lp(MATCH, WRAP).apply { setMargins(c.dp(8), c.dp(3), c.dp(8), c.dp(3)) })

        val area = FrameLayout(c)
        val keys = LinearLayout(c).apply { orientation = LinearLayout.VERTICAL }
        keyboard = TvKeyboard(c, actionLabel = "Show", shell = true)
        keyboard.onKey = { onKey(it) }
        keys.addView(keyboard, lp(MATCH, 0, 1f).apply { setMargins(c.dp(5), 0, c.dp(5), 0) })
        hint = c.text(9, Palette.shellMuted).apply {
            text = "wheel: next key · center: type · Menu up · Play down · Prev/Next left/right"
            gravity = Gravity.CENTER
        }
        keys.addView(hint, lp(MATCH, WRAP).apply { setMargins(0, c.dp(3), 0, c.dp(4)) })
        area.addView(keys, flp(MATCH, MATCH))
        results = RowList(c, RowStyle.SHELL)
        results.visible(false)
        area.addView(results, flp(MATCH, MATCH))
        root.addView(area, lp(MATCH, 0, 1f))
        render()
        return root
    }

    override fun onShow() {
        launch {
            LocalLibrary.load(context)
            found = LocalLibrary.search(query.toString())
            if (showing && found.isEmpty) showKeyboard() else if (showing) showResults()
            render()
        }
    }

    private fun onKey(key: String) {
        when (key) {
            TvKeyboard.DELETE -> if (query.isNotEmpty()) query.setLength(query.length - 1)
            TvKeyboard.SEARCH -> {
                if (!found.isEmpty) showResults()
                return
            }
            else -> if (query.length < 40) query.append(key)
        }
        found = LocalLibrary.search(query.toString())
        render()
    }

    private fun render() {
        field.text = if (showing) query.toString() else "$query|"
        val f = found
        val parts = if (addTo != null) listOf(f.artists.size to "artist", f.albums.size to "album", f.songs.size to "song")
        else listOf(f.artists.size to "artist", f.albums.size to "album", f.songs.size to "song", f.videos.size to "video")
        summary.text = when {
            query.isBlank() -> if (addTo != null) "Find songs to add, then press Show" else "Songs, artists, albums and videos on the card"
            f.isEmpty -> "Nothing on the card matches"
            else -> parts.filter { it.first > 0 }.joinToString(" · ") { (n, word) -> "$n $word" + if (n == 1) "" else "s" } + if (showing) " · Menu: back to typing" else " · Show to see them"
        }
    }

    private fun showResults() {
        showing = true
        keyboard.visible(false)
        hint.visible(false)
        results.visible(true)
        results.setRows(rows(), keepSelection = true)
        render()
    }

    private fun showKeyboard() {
        showing = false
        results.visible(false)
        keyboard.visible(true)
        hint.visible(true)
        render()
    }

    private fun rows(): List<Row> {
        val f = found
        val rows = mutableListOf<Row>()
        val songs = f.songs.map(LocalLibrary::item)
        if (f.artists.isNotEmpty()) rows += Row.Header("Artists")
        f.artists.forEach { (name, tracks) -> rows += group(name, tracks) }
        if (f.albums.isNotEmpty()) rows += Row.Header("Albums")
        f.albums.forEach { (name, tracks) -> rows += group(name, tracks) }
        if (songs.isNotEmpty()) rows += Row.Header("Songs")
        songs.forEachIndexed { i, item ->
            rows += Row.Entry(
                item.title, subtitle = item.subtitle, trailing = if (addTo != null && item.id in adding) "Adding…" else if (addTo != null && item.id in added) "Added" else durationText(item), thumb = item.thumb,
                onSelect = {
                    if (addTo != null) add(listOf(item.id)) else {
                        player?.playQueue(songs, i)
                        shell.push(NowPlayingScreen(shell))
                    }
                },
                onLongSelect = if (addTo != null) null else ({ shell.push(LocalSongMenuScreen(shell, item)) }),
            )
        }
        if (addTo == null && f.videos.isNotEmpty()) {
            rows += Row.Header("Videos")
            f.videos.map(LocalLibrary::item).forEach { item ->
                rows += Row.Entry(item.title, trailing = durationText(item), thumb = item.thumb, onSelect = { shell.push(VideoPlayerScreen(shell, item)) })
            }
        }
        return rows
    }

    private fun group(name: String, tracks: List<LocalLibrary.Track>): Row.Entry {
        val paths = tracks.map { it.path }
        val all = addTo != null && paths.all { it in added }
        return Row.Entry(
            name, trailing = if (addTo != null && paths.any { it in adding }) "Adding…" else if (all) "Added" else if (addTo != null) "Add ${tracks.size}" else "${tracks.size}",
            thumb = tracks.firstOrNull()?.let { LocalLibrary.item(it).thumb }, chevron = addTo == null,
            onSelect = { if (addTo != null) add(paths) else shell.push(LocalSongsScreen(shell, name, tracks, alphabetical = false, showArtist = true)) },
        )
    }

    private fun add(paths: List<String>) {
        val name = addTo ?: return
        if (paths.any { it in adding }) return
        adding += paths
        results.setRows(rows(), keepSelection = true)
        launch {
            val ok = runCatching { withContext(Dispatchers.IO) { Playlists.add(name, paths) } }
            adding -= paths.toSet()
            if (ok.isSuccess) added += paths
            results.setRows(rows(), keepSelection = true)
            ok.exceptionOrNull()?.let { summary.text = it.message }
        }
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean {
        if (showing) {
            if (action == WheelAction.Back) {
                showKeyboard()
                return true
            }
            return results.handle(action, steps)
        }
        return keyboard.handle(action)
    }
}

// Library › Playlists: New playlist, then every playlist on the card. Hold center for its options.
class PlaylistsScreen(shell: ShellActivity) : ShellListScreen(shell, "Playlists") {
    override fun render() {
        val all = runCatching { Playlists.all() }.getOrDefault(emptyList())
        val rows = mutableListOf<Row>(
            Row.Entry("New playlist", tile = Tiles.add, onSelect = {
                shell.push(TextEntryScreen(shell, "New playlist", action = "Create", maxLength = 60,
                    onDone = { name -> runCatching { withContext(Dispatchers.IO) { Playlists.create(name) } }.exceptionOrNull()?.message },
                    then = { name -> shell.push(PlaylistScreen(shell, Playlists.clean(name))) }))
            }),
        )
        if (all.isEmpty()) rows += Row.Header("No playlists yet")
        all.forEach { p ->
            rows += Row.Entry(p.name, trailing = "${p.paths.size}", tile = Tiles.playlists, chevron = true,
                onSelect = { shell.push(PlaylistScreen(shell, p.name)) },
                onLongSelect = { shell.push(PlaylistMenuScreen(shell, p.name)) })
        }
        list.setRows(rows, keepSelection = true)
    }

    override fun onShow() = render()
}

// One playlist: shuffle it, add songs (search), then its songs in order. Hold center on a song to
// move or remove it.
class PlaylistScreen(shell: ShellActivity, private val name: String) : ShellListScreen(shell, name) {
    override fun render() {
        val p = runCatching { Playlists.get(name) }.getOrNull() ?: return list.showMessage("This playlist is gone")
        val onCard = LocalLibrary.byPath()
        val items = p.paths.mapNotNull { onCard[it] }.map(LocalLibrary::item)
        val rows = mutableListOf<Row>(
            Row.Entry("Shuffle", trailing = "${items.size}", tile = Tiles.shuffle, enabled = items.isNotEmpty(), onSelect = {
                player?.playQueue(items.shuffled(), 0)
                shell.push(NowPlayingScreen(shell))
            }),
            Row.Entry("Add songs", tile = Tiles.add, chevron = true, onSelect = { shell.push(LibrarySearchScreen(shell, addTo = name)) }),
        )
        rows += Row.Header(when {
            items.isEmpty() -> "No songs yet · Add songs, or hold center on any song"
            else -> "${items.size} song${if (items.size == 1) "" else "s"} · hold center: move or remove"
        })
        items.forEachIndexed { i, item ->
            rows += Row.Entry(item.title, subtitle = item.subtitle, trailing = durationText(item), thumb = item.thumb,
                onSelect = {
                    player?.playQueue(items, i)
                    shell.push(NowPlayingScreen(shell))
                },
                onLongSelect = { shell.push(LocalSongMenuScreen(shell, item, playlist = name)) })
        }
        val missing = p.paths.size - items.size
        if (missing > 0) rows += Row.Entry("$missing song${if (missing == 1) " isn't" else "s aren't"} on the card", subtitle = "Copy them back, or they're skipped", enabled = false)
        list.setRows(rows, keepSelection = true)
    }

    override fun onShow() {
        launch {
            LocalLibrary.load(context)
            render()
        }
    }
}

class PlaylistMenuScreen(shell: ShellActivity, private val name: String) : ShellListScreen(shell, name) {
    override fun render() {
        list.setRows(listOf(
            Row.Entry("Rename", chevron = true, onSelect = {
                shell.push(TextEntryScreen(shell, "Rename “$name”", initial = name, action = "Save", maxLength = 60, onDone = { typed ->
                    runCatching { withContext(Dispatchers.IO) { Playlists.update(name, newName = typed) } }.exceptionOrNull()?.message
                }, then = { shell.pop() }))
            }),
            Row.Entry("Delete playlist", subtitle = "The songs stay on the card", tile = Tiles.delete, onSelect = {
                shell.push(ConfirmScreen(shell, "Delete “$name”?", "The songs stay on the card.", "Delete playlist") {
                    runCatching { Playlists.delete(name) }
                    shell.popTo(PlaylistsScreen::class.java)
                })
            }),
        ))
    }
}

// Pick a playlist for one song (or start a new one with it).
class AddToPlaylistScreen(shell: ShellActivity, private val paths: List<String>) : ShellListScreen(shell, "Add to playlist") {
    override fun render() {
        val all = runCatching { Playlists.all() }.getOrDefault(emptyList())
        val rows = mutableListOf<Row>(
            Row.Entry("New playlist", tile = Tiles.add, onSelect = {
                shell.push(TextEntryScreen(shell, "New playlist", action = "Create", maxLength = 60,
                    onDone = { name -> runCatching { withContext(Dispatchers.IO) { Playlists.create(name, paths) } }.exceptionOrNull()?.message },
                    then = { shell.pop() }))
            }),
        )
        all.forEach { p ->
            val has = paths.all { it in p.paths }
            rows += Row.Entry(p.name, trailing = if (has) "Added" else "${p.paths.size}", tile = Tiles.playlists, enabled = !has, onSelect = {
                runCatching { Playlists.add(p.name, paths) }
                shell.pop()
            })
        }
        list.setRows(rows)
    }
}

// Hold center on a song on the card: play it, queue it, put it in a playlist, or delete it. From a
// playlist it also moves the song or takes it out.
class LocalSongMenuScreen(shell: ShellActivity, private val song: Item, private val playlist: String? = null) : ShellListScreen(shell, song.title) {
    private var note: String? = null

    override fun render() {
        note?.let { return list.showMessage(it) }
        val rows = mutableListOf<Row>(
            Row.Entry("Play now", onSelect = { openMusicItem(shell, song, listOf(song)) }),
            Row.Entry("Play next", onSelect = { enqueue(next = true) }),
            Row.Entry("Add to queue", onSelect = { enqueue(next = false) }),
            Row.Entry("Add to playlist", tile = Tiles.playlists, chevron = true, onSelect = { shell.push(AddToPlaylistScreen(shell, listOf(song.id))) }),
        )
        playlist?.let { name ->
            val paths = runCatching { Playlists.get(name)?.paths }.getOrNull().orEmpty()
            val at = paths.indexOf(song.id)
            rows += Row.Header("In “$name”")
            rows += Row.Entry("Move up", enabled = at > 0, onSelect = { move(name, paths, at, at - 1) })
            rows += Row.Entry("Move down", enabled = at in 0 until paths.lastIndex, onSelect = { move(name, paths, at, at + 1) })
            rows += Row.Entry("Remove from playlist", onSelect = {
                runCatching { Playlists.update(name, paths = paths - song.id) }
                shell.pop()
            })
        }
        rows += Row.Entry("Delete from card", subtitle = "Can't be undone", tile = Tiles.delete, onSelect = {
            shell.push(ConfirmScreen(shell, "Delete this song?", "“${song.title}” will be removed from the card.", "Delete") {
                launch {
                    val ok = LocalLibrary.delete(context, song.id)
                    shell.pop()
                    if (ok) shell.pop() else { note = "Couldn't delete it"; render() }
                }
            })
        })
        list.setRows(rows, keepSelection = true)
    }

    private fun move(name: String, paths: List<String>, from: Int, to: Int) {
        runCatching { Playlists.update(name, paths = paths.toMutableList().apply { add(to, removeAt(from)) }) }
        shell.pop()
    }

    private fun enqueue(next: Boolean) {
        val p = player ?: return
        val ok = if (next) p.playNext(song) else p.addToQueue(song)
        if (ok) shell.pop() else {
            note = "Songs on the card can't join an online queue.\nUse Play now to start a new one."
            render()
        }
    }
}

// Are you sure? Cancel is first, so a stray center press keeps things.
class ConfirmScreen(shell: ShellActivity, title: String, private val question: String, private val action: String, private val onConfirm: () -> Unit) :
    ShellListScreen(shell, title) {
    override fun render() {
        list.setRows(listOf(
            Row.Header(question),
            Row.Entry("Cancel", onSelect = { shell.pop() }),
            Row.Entry(action, tile = Tiles.delete, onSelect = onConfirm),
        ))
    }
}
