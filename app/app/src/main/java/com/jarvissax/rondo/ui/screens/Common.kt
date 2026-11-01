package com.jarvissax.rondo.ui.screens

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.net.HttpException
import com.jarvissax.rondo.net.Item
import com.jarvissax.rondo.player.PlayerService
import com.jarvissax.rondo.ui.ArtColor
import com.jarvissax.rondo.ui.IconView
import com.jarvissax.rondo.ui.ImageLoader
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.ProgressBarView
import com.jarvissax.rondo.ui.RoundedImageView
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.RowList
import com.jarvissax.rondo.ui.RowStyle
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.batteryLevel
import com.jarvissax.rondo.ui.darker
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.dpf
import com.jarvissax.rondo.ui.formatTime
import com.jarvissax.rondo.ui.isOnline
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.rowArt
import com.jarvissax.rondo.ui.text
import com.jarvissax.rondo.ui.visible

// Dark header of the YouTube / YouTube Music screens: optional logo, title, Wi-Fi and battery.
fun Context.darkHeader(title: String, logo: View? = null, titleColor: Int = Palette.white): LinearLayout {
    val bar = LinearLayout(this).apply {
        gravity = Gravity.CENTER_VERTICAL
        setPadding(dp(10), 0, dp(10), 0)
    }
    logo?.let { bar.addView(it, lp(dp(16), dp(16)).apply { rightMargin = dp(7) }) }
    bar.addView(text(14, titleColor, bold = true).apply { this.text = title }, lp(0, WRAP, 1f))
    bar.addView(statusIcons(Palette.darkMuted), lp(WRAP, WRAP))
    return bar
}

fun Context.statusIcons(color: Int, wifi: Boolean = true): LinearLayout = com.jarvissax.rondo.ui.StatusIcons(this, color, wifi)

// Background tinted by the selected artwork, like YouTube Music's ambient color: the color
// at the top fades into #0f0f0f. Swapped without a fade: a TransitionDrawable background stalls
// on its first frame on this ROM (the screen stayed at ~14 % brightness).
class Ambient(private val view: View) {
    private var shownColor: Int? = null
    private val colors = HashMap<String, Int>()

    init {
        view.background = ColorDrawable(Palette.dark)
    }

    fun follow(style: RowStyle, thumb: String?) {
        if (thumb == null) return set(null)
        val c = colors[thumb] ?: view.context.rowArt(style, thumb)?.let { ArtColor.dominant(it) }?.also { colors[thumb] = it }
        if (c != null) set(c)
    }

    fun set(color: Int?) {
        if (color == shownColor) return
        shownColor = color
        view.background = if (color == null) ColorDrawable(Palette.dark) else GradientDrawable(
            GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(darker(color, 0.55f), darker(color, 0.22f), Palette.dark),
        )
    }
}

// Mini player at the bottom of the music screens: artwork, title and artist, state, a thin
// progress line on top.
class MiniPlayer(context: Context) : LinearLayout(context) {
    private val art = RoundedImageView(context, context.dpf(3f), Palette.darkRaised)
    private val icon = IconView(context, IconView.Kind.PLAY, Palette.white)
    private val title = context.text(12, Palette.white, bold = true)
    private val artist = context.text(10, Palette.darkMuted)
    private val progress = ProgressBarView(context, Color.argb(60, 255, 255, 255), Palette.white, barDp = 2)
    private var shownThumb: String? = null

    init {
        orientation = VERTICAL
        setBackgroundColor(Color.argb(250, 33, 33, 33))
        addView(progress, lp(MATCH, context.dp(2)))
        val row = LinearLayout(context).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(context.dp(6), 0, context.dp(12), 0)
        }
        row.addView(art, lp(context.dp(24), context.dp(24)).apply { rightMargin = context.dp(8) })
        val column = LinearLayout(context).apply { orientation = VERTICAL }
        column.addView(title, lp(MATCH, WRAP))
        column.addView(artist, lp(MATCH, WRAP).apply { topMargin = context.dp(1) })
        row.addView(column, lp(0, WRAP, 1f))
        row.addView(icon, lp(context.dp(12), context.dp(12)).apply { leftMargin = context.dp(8) })
        addView(row, lp(MATCH, 0, 1f))
        visible(false)
    }

    fun update(player: PlayerService?) {
        val item = player?.current
        visible(item != null && !player.isVideo)
        if (item == null || player == null) return
        title.text = item.title
        artist.text = item.subtitle.substringBefore(" · ")
        icon.kind = if (player.isPlaying()) IconView.Kind.PAUSE else IconView.Kind.PLAY
        icon.invalidate()
        val d = player.durationMs()
        progress.fraction = if (d > 0) player.positionMs() / d.toFloat() else 0f
        if (item.thumb != shownThumb || art.drawable == null) {
            shownThumb = item.thumb
            ImageLoader.load(art, item.thumb, context.dp(24), context.dp(24))
        }
    }

    companion object {
        const val HEIGHT = 32
    }
}

// Infinite scrolling: when the selection is 5 rows from the end, the next page is fetched and
// appended. A page that fails is retried on the next turn; an expired token (410) ends the list.
class Pager(
    private val list: RowList,
    private val launch: (suspend CoroutineScope.() -> Unit) -> Job,
    private val fetch: suspend (next: String) -> Pair<List<Row>, String?>,
) {
    private var next: String? = null
    private var loading = false
    private var generation = 0

    init {
        list.onNearEnd = { more() }
    }

    fun reset(next: String?) {
        this.next = next
        loading = false
        generation++
    }

    private fun more() {
        val token = next ?: return
        if (loading) return
        loading = true
        val gen = generation
        launch {
            try {
                val (rows, n) = fetch(token)
                if (gen != generation) return@launch
                next = n
                list.appendRows(rows)
            } catch (e: HttpException) {
                if (gen == generation && e.code == 410) next = null
            } catch (e: Exception) {
                // keep the token: the next turn of the wheel tries again
            } finally {
                if (gen == generation) loading = false
            }
        }
    }
}

fun durationText(item: Item): String = item.durationSec?.let { formatTime(it * 1000L) } ?: ""

private val CONTAINERS = setOf("playlist", "album", "artist")

// Opens anything a music list shows: songs (and music videos) play with their siblings as the
// queue, containers push their own screen.
fun openMusicItem(shell: ShellActivity, item: Item, siblings: List<Item>) {
    when (item.kind) {
        "playlist" -> shell.push(CollectionScreen(shell, item.id, item.title, album = false))
        "album" -> shell.push(CollectionScreen(shell, item.id, item.title, album = true))
        "artist" -> shell.push(ArtistScreen(shell, item.id, item.title))
        else -> {
            val playable = siblings.filter { it.kind !in CONTAINERS }
            shell.player?.playQueue(playable, playable.indexOf(item).coerceAtLeast(0))
            shell.push(NowPlayingScreen(shell))
        }
    }
}

// `queue` is what plays around a chosen song: by default the rows passed in, for a paged list
// everything loaded so far
fun musicRows(shell: ShellActivity, items: List<Item>, queue: () -> List<Item> = { items }): List<Row> = items.map { item ->
    Row.Entry(
        title = item.title,
        subtitle = item.subtitle,
        trailing = if (item.kind in CONTAINERS) "" else durationText(item),
        thumb = item.thumb,
        chevron = item.kind in CONTAINERS,
        onSelect = { openMusicItem(shell, item, queue()) },
        onLongSelect = if (item.kind in CONTAINERS) null else ({ shell.push(SongMenuScreen(shell, item)) }),
    )
}

fun videoRows(shell: ShellActivity, items: List<Item>): List<Row> = items.map { item ->
    Row.Entry(
        title = item.title,
        subtitle = item.subtitle,
        trailing = durationText(item),
        thumb = item.thumb,
        onSelect = { shell.push(VideoPlayerScreen(shell, item)) },
    )
}

// youtube music thumbnails are URLs like ...=w120-h120-l90-rj: ask Google for a bigger one for
// the large art views (the server still resizes it to the requested width)
fun largeThumb(thumb: String?, size: Int): String? {
    if (thumb == null) return null
    val prefix = "/thumb?u="
    if (!thumb.startsWith(prefix)) return thumb
    val url = java.net.URLDecoder.decode(thumb.removePrefix(prefix), "UTF-8")
    val bigger = url.replace(Regex("=w\\d+-h\\d+"), "=w$size-h$size")
    return prefix + java.net.URLEncoder.encode(bigger, "UTF-8")
}


// what a list says when it could not load: point at Settings › Wi-Fi when there is no network
fun Context.offlineOr(problem: String): String =
    if (!com.jarvissax.rondo.net.Api.configured) com.jarvissax.rondo.net.Api.NOT_SET_UP
    else if (!isOnline(this)) "No Wi-Fi connection.\nConnect in Settings › Wi-Fi, then press center." else "$problem\nPress center to try again"
