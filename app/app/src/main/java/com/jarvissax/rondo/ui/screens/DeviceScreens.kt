package com.jarvissax.rondo.ui.screens

import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.local.LocalLibrary
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.Tiles

// Music and videos on the microSD card, in the iPod's own words (Songs, Artists, Albums). Works
// with no network at all. Lists come from LocalLibrary; tags fill in while the menu is open.
class DeviceScreen(shell: ShellActivity) : ShellListScreen(shell, "Library") {
    private var loaded = false

    override fun render() {
        if (!loaded) return list.showMessage("Looking at the card…")
        val songs = LocalLibrary.songs
        val videos = LocalLibrary.videos
        if (songs.isEmpty() && videos.isEmpty()) return list.showMessage("No music or videos on the card.\nCopy files to the card over USB.")
        val artists = LocalLibrary.artists()
        val albums = LocalLibrary.albums()
        list.setRows(
            listOf(
                Row.Entry("Shuffle songs", trailing = "${songs.size}", tile = Tiles.shuffle, enabled = songs.isNotEmpty(), onSelect = {
                    player?.playQueue(songs.shuffled().map(LocalLibrary::item), 0)
                    shell.push(NowPlayingScreen(shell))
                }),
                Row.Entry("Songs", trailing = "${songs.size}", tile = Tiles.songs, chevron = true, enabled = songs.isNotEmpty(), onSelect = {
                    shell.push(LocalSongsScreen(shell, "Songs", songs, alphabetical = true))
                }),
                Row.Entry("Artists", trailing = "${artists.size}", tile = Tiles.artists, chevron = true, enabled = artists.isNotEmpty(), onSelect = {
                    shell.push(LocalGroupsScreen(shell, "Artists", artists))
                }),
                Row.Entry("Albums", trailing = if (albums.isEmpty()) "reading tags…" else "${albums.size}", tile = Tiles.albums, chevron = true, enabled = albums.isNotEmpty(), onSelect = {
                    shell.push(LocalGroupsScreen(shell, "Albums", albums))
                }),
                Row.Entry("Videos", trailing = "${videos.size}", tile = Tiles.videos, chevron = true, enabled = videos.isNotEmpty(), onSelect = {
                    shell.push(LocalVideosScreen(shell, videos))
                }),
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

    override fun onHide() {
        LocalLibrary.onUpdate = null
    }
}

// Long alphabetical lists: Next/Prev jump to the next/previous first letter, like the iPod's
// letter overlay (814 songs would otherwise be ~270 fast detents).
abstract class LetterListScreen(shell: ShellActivity, title: String) : ShellListScreen(shell, title) {
    protected var letters: List<Pair<Int, Char>> = emptyList() // row index of each letter's first row

    protected fun indexLetters(rows: List<Row>) {
        letters = rows.mapIndexedNotNull { i, r -> (r as? Row.Entry)?.title?.firstOrNull()?.uppercaseChar()?.let { i to (if (it.isLetter()) it else '#') } }
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
) : LetterListScreen(shell, title) {
    override fun render() {
        val items = tracks.map(LocalLibrary::item)
        val songs = items.mapIndexed { i, item ->
            Row.Entry(
                title = item.title,
                subtitle = if (alphabetical) item.subtitle else item.subtitle.substringAfter(" · ", ""),
                trailing = durationText(item),
                thumb = item.thumb,
                onSelect = {
                    player?.playQueue(items, i)
                    shell.push(NowPlayingScreen(shell))
                },
                onLongSelect = { shell.push(SongMenuScreen(shell, item)) },
            )
        }
        val head = Row.Header(if (alphabetical && songs.size > 30) "${songs.size} songs · Next/Prev jump A–Z" else "${songs.size} songs")
        val rows = listOf(head) + songs
        list.setRows(rows)
        if (alphabetical) indexLetters(rows)
    }
}

class LocalGroupsScreen(shell: ShellActivity, title: String, private val groups: List<Pair<String, List<LocalLibrary.Track>>>) : LetterListScreen(shell, title) {
    override fun render() {
        val rows = groups.map { (name, tracks) ->
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
            videos.map { t ->
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
