package com.jarvissax.rondo.ui.screens

import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.local.LocalLibrary
import com.jarvissax.rondo.local.Playlists
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.Tiles

// Music and videos on the microSD card, in the iPod's own words (Songs, Artists, Albums), plus
// search and playlists. Works with no network at all. Lists come from LocalLibrary; tags fill in
// while the menu is open.
class DeviceScreen(shell: ShellActivity) : ShellListScreen(shell, "Library") {
    private var loaded = false
    private var scanning = false

    override fun render() {
        if (!loaded) return list.showMessage("Looking at the card…")
        val songs = LocalLibrary.songs
        val videos = LocalLibrary.videos
        if (songs.isEmpty() && videos.isEmpty() && !scanning) return list.showMessage("No music or videos on the card.\nCopy files to the card over USB.")
        val artists = LocalLibrary.artists()
        val albums = LocalLibrary.albums()
        val playlists = runCatching { Playlists.all().size }.getOrDefault(0)
        list.setRows(
            listOf(
                Row.Entry("Shuffle songs", trailing = "${songs.size}", tile = Tiles.shuffle, enabled = songs.isNotEmpty(), onSelect = {
                    player?.playQueue(songs.shuffled().map(LocalLibrary::item), 0)
                    shell.push(NowPlayingScreen(shell))
                }),
                Row.Entry("Search", tile = Tiles.search, chevron = true, onSelect = { shell.push(LibrarySearchScreen(shell)) }),
                Row.Entry("Playlists", trailing = if (playlists > 0) "$playlists" else "", tile = Tiles.playlists, chevron = true, onSelect = { shell.push(PlaylistsScreen(shell)) }),
                Row.Entry("Songs", trailing = "${songs.size}", tile = Tiles.songs, chevron = true, enabled = songs.isNotEmpty(), onSelect = {
                    shell.push(LocalSongsScreen(shell, "Songs", songs, alphabetical = true))
                }),
                Row.Entry("Artists", trailing = "${artists.size}", tile = Tiles.artists, chevron = true, enabled = artists.isNotEmpty(), onSelect = {
                    shell.push(LocalGroupsScreen(shell, "Artists", artists, SearchScope.ARTISTS))
                }),
                Row.Entry("Albums", trailing = if (albums.isEmpty()) "reading tags…" else "${albums.size}", tile = Tiles.albums, chevron = true, enabled = albums.isNotEmpty(), onSelect = {
                    shell.push(LocalGroupsScreen(shell, "Albums", albums, SearchScope.ALBUMS))
                }),
                Row.Entry("Recently added", tile = Tiles.recent, chevron = true, enabled = songs.isNotEmpty(), onSelect = {
                    shell.push(LocalSongsScreen(shell, "Recently added", LocalLibrary.recent, alphabetical = false, showArtist = true))
                }),
                Row.Entry("Videos", trailing = "${videos.size}", tile = Tiles.videos, chevron = true, enabled = videos.isNotEmpty(), onSelect = {
                    shell.push(LocalVideosScreen(shell, videos))
                }),
                Row.Entry("Rescan the card", subtitle = "After copying files to it", trailing = if (scanning) "Scanning…" else "", tile = Tiles.rescan, onSelect = { rescan() }),
            ),
            keepSelection = true,
        )
    }

    override fun onShow() {
        LocalLibrary.onUpdate = { render() }
        launch {
            LocalLibrary.load(context)
            loaded = true
            render()
            LocalLibrary.readTags(context)
        }
    }

    // the row stays selectable while it scans: a disabled row would send the highlight to the top
    private fun rescan() {
        if (scanning) return
        scanning = true
        render()
        LocalLibrary.invalidate()
        launch {
            LocalLibrary.load(context)
            scanning = false
            render()
            LocalLibrary.readTags(context)
        }
    }

    override fun onHide() {
        LocalLibrary.onUpdate = null
    }
}

// Long alphabetical lists: Next/Prev jump to the next/previous first letter, like the iPod's
// letter overlay (814 songs would otherwise be ~270 fast detents).
abstract class LetterListScreen(shell: ShellActivity, title: String) : ShellListScreen(shell, title) {
    protected var letters: List<Pair<Int, Char>> = emptyList() // row index of each letter's first row

    protected fun indexLetters(rows: List<Row>) {
        letters = rows.mapIndexedNotNull { i, r -> (r as? Row.Entry)?.takeIf { it.tile != Tiles.search }?.title?.firstOrNull()?.uppercaseChar()?.let { i to (if (it.isLetter()) it else '#') } }
            .distinctBy { it.second }
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean {
        if (letters.size > 1 && (action == WheelAction.Next || action == WheelAction.Prev)) {
            val at = list.selected
            val target = if (action == WheelAction.Next) letters.firstOrNull { it.first > at } else letters.lastOrNull { it.first < at }
            target?.let { list.select(it.first) }
            return true
        }
        return super.onAction(action, steps)
    }
}

class LocalSongsScreen(
    shell: ShellActivity,
    title: String,
    private val tracks: List<LocalLibrary.Track>,
    private val alphabetical: Boolean,
    private val showArtist: Boolean = alphabetical, // false: an artist's songs show only the album
) : LetterListScreen(shell, title) {
    override fun render() {
        val onCard = LocalLibrary.byPath() // a song deleted meanwhile drops out
        val items = tracks.filter { it.path in onCard }.map(LocalLibrary::item)
        val songs = items.mapIndexed { i, item ->
            Row.Entry(
                title = item.title,
                subtitle = if (showArtist) item.subtitle else item.subtitle.substringAfter(" · ", ""),
                trailing = durationText(item),
                thumb = item.thumb,
                onSelect = {
                    player?.playQueue(items, i)
                    shell.push(NowPlayingScreen(shell))
                },
                onLongSelect = { shell.push(LocalSongMenuScreen(shell, item)) },
            )
        }
        val head = Row.Header(if (alphabetical && songs.size > 30) "${songs.size} songs · Next/Prev jump A–Z" else "${songs.size} songs")
        val rows = (if (alphabetical) listOf(searchRow(shell, SearchScope.SONGS)) else emptyList()) + listOf(head) + songs
        list.setRows(rows, keepSelection = true)
        if (alphabetical) indexLetters(rows)
    }

    // back from a song's options (it may be gone); the first show was already drawn by build()
    private var shown = false

    override fun onShow() {
        if (shown) render() else shown = true
    }
}

class LocalGroupsScreen(shell: ShellActivity, title: String, private val groups: List<Pair<String, List<LocalLibrary.Track>>>, private val scope: SearchScope) : LetterListScreen(shell, title) {
    override fun render() {
        val rows = listOf(searchRow(shell, scope)) + groups.map { (name, tracks) ->
            Row.Entry(
                title = name,
                trailing = "${tracks.size}",
                thumb = tracks.firstOrNull()?.let { LocalLibrary.item(it).thumb },
                chevron = true,
                onSelect = { shell.push(LocalSongsScreen(shell, name, tracks, alphabetical = false)) },
            )
        }
        list.setRows(rows)
        indexLetters(rows)
    }
}

class LocalVideosScreen(shell: ShellActivity, private val videos: List<LocalLibrary.Track>) : ShellListScreen(shell, "Videos") {
    override fun render() {
        list.setRows(
            listOf(searchRow(shell, SearchScope.VIDEOS)) + videos.map { t ->
                val item = LocalLibrary.item(t)
                Row.Entry(
                    title = item.title,
                    subtitle = item.subtitle,
                    trailing = durationText(item),
                    thumb = item.thumb,
                    onSelect = { shell.push(VideoPlayerScreen(shell, item)) },
                )
            },
        )
    }
}

// the first row of a long list: search just that list
fun searchRow(shell: ShellActivity, scope: SearchScope): Row.Entry =
    Row.Entry(scope.title, tile = Tiles.search, chevron = true, onSelect = { shell.push(LibrarySearchScreen(shell, scope = scope)) })
