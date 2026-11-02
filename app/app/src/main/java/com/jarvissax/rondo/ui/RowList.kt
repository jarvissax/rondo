package com.jarvissax.rondo.ui

import android.content.Context
import android.graphics.Color
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.jarvissax.rondo.input.WheelAction

enum class RowStyle { SHELL, MUSIC, VIDEO }

sealed class Row {
    data class Header(val title: String) : Row()

    class Entry(
        val title: String,
        val subtitle: String = "",
        val trailing: String = "",
        val thumb: String? = null,
        val chevron: Boolean = false,
        val enabled: Boolean = true,
        val tile: Tile? = null,
        val onSelect: (() -> Unit)? = null,
        val onLongSelect: (() -> Unit)? = null,
    ) : Row()
}

// The list every screen is built from. One selected row driven by the wheel, kept visible and
// centered past the edges; asks for more rows near the end (infinite scrolling).
class RowList(context: Context, private val style: RowStyle) : FrameLayout(context) {
    private var rows: MutableList<Row> = mutableListOf()
    var selected = -1
        private set

    // false while something else on the screen (a tab bar) has the wheel
    var active = true
        set(value) {
            field = value
            adapter.notifyDataSetChanged()
        }

    var onTopReached: (() -> Boolean)? = null
    var onNearEnd: (() -> Unit)? = null // load the next page; called when 5 rows from the end
    var onSelectionChanged: ((Row.Entry?) -> Unit)? = null

    private val recycler = RecyclerView(context)
    private val layoutManager = LinearLayoutManager(context)
    private val adapter = Adapter()
    private val empty = context.text(15, if (style == RowStyle.SHELL) Palette.shellMuted else Palette.darkMuted, lines = 4).apply {
        gravity = Gravity.CENTER
        setPadding(context.dp(20), 0, context.dp(20), 0)
    }

    init {
        recycler.layoutManager = layoutManager
        recycler.adapter = adapter
        recycler.isFocusable = false
        recycler.itemAnimator = null
        addView(recycler, flp(MATCH, MATCH))
        addView(empty, flp(MATCH, MATCH))
        showMessage("Loading…")
    }

    val isEmpty: Boolean get() = rows.isEmpty()

    fun showMessage(message: String) {
        rows = mutableListOf()
        selected = -1
        empty.text = message
        empty.visible(true)
        adapter.notifyDataSetChanged()
    }

    fun setRows(newRows: List<Row>, keepSelection: Boolean = false) {
        val previous = selected
        rows = newRows.toMutableList()
        empty.visible(rows.isEmpty())
        if (rows.isEmpty()) empty.text = "Nothing here yet"
        selected = if (keepSelection && previous in rows.indices && selectable(previous)) previous else firstSelectable()
        adapter.notifyDataSetChanged()
        post { ensureVisible() }
        onSelectionChanged?.invoke(selectedEntry())
    }

    fun appendRows(more: List<Row>) {
        if (more.isEmpty()) return
        val start = rows.size
        rows.addAll(more)
        adapter.notifyItemRangeInserted(start, more.size)
    }

    fun selectedEntry(): Row.Entry? = rows.getOrNull(selected) as? Row.Entry

    fun select(index: Int) {
        if (index in rows.indices && selectable(index)) {
            val old = selected
            selected = index
            adapter.notifyItemChanged(old)
            adapter.notifyItemChanged(index)
            ensureVisible()
            if (old != index) onSelectionChanged?.invoke(selectedEntry())
        }
    }

    private fun selectable(i: Int) = (rows.getOrNull(i) as? Row.Entry)?.enabled == true
    private fun firstSelectable() = rows.indices.firstOrNull { selectable(it) } ?: -1

    fun handle(action: WheelAction, steps: Int): Boolean = when (action) {
        WheelAction.Down -> {
            move(+1, steps)
            true
        }
        WheelAction.Up -> {
            if (selected == firstSelectable() || selected < 0) onTopReached?.invoke() ?: true else {
                move(-1, steps)
                true
            }
        }
        WheelAction.Select -> selectedEntry()?.onSelect?.let { it(); true } ?: false
        WheelAction.LongSelect -> selectedEntry()?.onLongSelect?.let { it(); true } ?: false
        else -> false
    }

    private fun move(dir: Int, steps: Int) {
        var i = selected
        repeat(steps) {
            var j = i + dir
            while (j in rows.indices && !selectable(j)) j += dir
            if (j in rows.indices) i = j
        }
        select(i)
        if (dir > 0 && selected >= rows.size - 5) onNearEnd?.invoke()
    }

    private fun ensureVisible() {
        if (selected < 0) return
        val first = layoutManager.findFirstCompletelyVisibleItemPosition()
        val last = layoutManager.findLastCompletelyVisibleItemPosition()
        if (first == RecyclerView.NO_POSITION) {
            post { ensureVisible() }
            return
        }
        if (selected == firstSelectable()) {
            layoutManager.scrollToPositionWithOffset(0, 0)
        } else if (selected <= first || selected >= last) {
            layoutManager.scrollToPositionWithOffset(selected, (recycler.height - context.dp(RowViews.height(style))) / 2)
        }
    }

    private inner class Holder(val view: View) : RecyclerView.ViewHolder(view)

    private inner class Adapter : RecyclerView.Adapter<Holder>() {
        override fun getItemCount() = rows.size
        override fun getItemViewType(position: Int) = if (rows[position] is Row.Header) 0 else 1

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder =
            Holder(if (viewType == 0) RowViews.header(context, style) else RowViews.entry(context, style))

        override fun onBindViewHolder(holder: Holder, position: Int) {
            when (val row = rows[position]) {
                is Row.Header -> (holder.view.tag as TextView).text = if (style == RowStyle.SHELL) row.title.uppercase() else row.title
                is Row.Entry -> RowViews.bind(holder.view, style, row, active && position == selected)
            }
        }
    }
}

// Row views per style, built in code. Sizes are mockup units, scaled by UI_SCALE in dp().
private object RowViews {
    fun height(style: RowStyle) = when (style) {
        RowStyle.SHELL -> 36
        RowStyle.MUSIC -> 42
        RowStyle.VIDEO -> 66
    }

    const val MUSIC_ART = 34
    const val VIDEO_W = 104
    const val VIDEO_H = 58
    private const val TILE = "tile"

    class ShellTag(
        val title: TextView, val subtitle: TextView, val trailing: TextView, val chevron: IconView,
        val tileBox: FrameLayout, val art: RoundedImageView, val line: View,
    )
    class MediaTag(val card: LinearLayout, val title: TextView, val subtitle: TextView, val trailing: TextView, val art: RoundedImageView, val chevron: IconView, val artFrame: FrameLayout)

    fun header(c: Context, style: RowStyle): View {
        val shell = style == RowStyle.SHELL
        val t = c.text(if (shell) 11 else 15, if (shell) Palette.shellMuted else Palette.white, bold = true)
        val box = FrameLayout(c).apply {
            layoutParams = RecyclerView.LayoutParams(MATCH, c.dp(if (shell) 20 else 28))
            if (shell) setBackgroundColor(Palette.shellHeader)
        }
        box.addView(t, flp(WRAP, WRAP, Gravity.BOTTOM or Gravity.START).apply {
            leftMargin = c.dp(if (shell) 12 else 10)
            bottomMargin = c.dp(if (shell) 3 else 5)
        })
        box.tag = t
        return box
    }

    fun entry(c: Context, style: RowStyle): View = if (style == RowStyle.SHELL) shell(c) else media(c, style)

    private fun shell(c: Context): View {
        val frame = FrameLayout(c).apply { layoutParams = RecyclerView.LayoutParams(MATCH, c.dp(height(RowStyle.SHELL))) }
        val row = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(8), 0, c.dp(10), 0)
        }
        val tileBox = FrameLayout(c)
        val art = RoundedImageView(c, c.dpf(4f), Palette.shellRowLine)
        tileBox.addView(art, flp(MATCH, MATCH))
        row.addView(tileBox, lp(c.dp(26), c.dp(26)).apply { rightMargin = c.dp(9) })
        val column = LinearLayout(c).apply { orientation = LinearLayout.VERTICAL }
        val title = c.text(15, Palette.shellText)
        val subtitle = c.text(11, Palette.shellMuted)
        column.addView(title, lp(MATCH, WRAP))
        column.addView(subtitle, lp(MATCH, WRAP).apply { topMargin = c.dp(1) })
        row.addView(column, lp(0, WRAP, 1f))
        val trailing = c.text(12, Palette.shellMuted)
        val chevron = IconView(c, IconView.Kind.CHEVRON, Palette.shellChevron)
        row.addView(trailing, lp(WRAP, WRAP).apply { leftMargin = c.dp(6) })
        row.addView(chevron, lp(c.dp(8), c.dp(12)).apply { leftMargin = c.dp(5) })
        frame.addView(row, flp(MATCH, MATCH))
        val line = c.line(Palette.shellRowLine)
        frame.addView(line, flp(MATCH, 1, Gravity.BOTTOM).apply { leftMargin = c.dp(8) })
        frame.tag = ShellTag(title, subtitle, trailing, chevron, tileBox, art, line)
        return frame
    }

    private fun media(c: Context, style: RowStyle): View {
        val video = style == RowStyle.VIDEO
        val frame = FrameLayout(c).apply { layoutParams = RecyclerView.LayoutParams(MATCH, c.dp(height(style))) }
        val card = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(4), 0, c.dp(8), 0)
        }
        val artFrame = FrameLayout(c)
        val art = RoundedImageView(c, c.dpf(if (video) 5f else 3f), Palette.darkRaised)
        artFrame.addView(art, flp(MATCH, MATCH))
        val trailing = c.text(if (video) 9 else 11, if (video) Palette.white else Palette.darkMuted, bold = video)
        if (video) {
            trailing.background = rounded(Color.argb(220, 0, 0, 0), c.dpf(3f))
            trailing.setPadding(c.dp(3), c.dp(1), c.dp(3), c.dp(1))
            artFrame.addView(trailing, flp(WRAP, WRAP, Gravity.END or Gravity.BOTTOM).apply { setMargins(0, 0, c.dp(3), c.dp(3)) })
        }
        card.addView(artFrame, if (video) lp(c.dp(VIDEO_W), c.dp(VIDEO_H)) else lp(c.dp(MUSIC_ART), c.dp(MUSIC_ART)))
        (artFrame.layoutParams as LinearLayout.LayoutParams).rightMargin = c.dp(if (video) 9 else 8)
        val column = LinearLayout(c).apply { orientation = LinearLayout.VERTICAL }
        val title = c.text(if (video) 12 else 13, Palette.white, lines = if (video) 2 else 1)
        val subtitle = c.text(if (video) 10 else 11, Palette.darkMuted, lines = 1)
        column.addView(title, lp(MATCH, WRAP))
        column.addView(subtitle, lp(MATCH, WRAP).apply { topMargin = c.dp(2) })
        card.addView(column, lp(0, WRAP, 1f))
        if (!video) card.addView(trailing, lp(WRAP, WRAP).apply { leftMargin = c.dp(6) })
        val chevron = IconView(c, IconView.Kind.CHEVRON, Palette.darkFaint)
        card.addView(chevron, lp(c.dp(7), c.dp(11)).apply { leftMargin = c.dp(5) })
        frame.addView(card, flp(MATCH, MATCH).apply { setMargins(c.dp(5), c.dp(2), c.dp(5), c.dp(2)) })
        frame.tag = MediaTag(card, title, subtitle, trailing, art, chevron, artFrame)
        return frame
    }

    fun bind(view: View, style: RowStyle, row: Row.Entry, isSelected: Boolean) {
        val c = view.context
        when (val t = view.tag) {
            is ShellTag -> {
                t.title.text = row.title
                t.title.setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, c.dpf(if (row.subtitle.isEmpty()) 15f else 13.5f))
                t.title.font(isSelected)
                t.subtitle.text = row.subtitle
                t.subtitle.visible(row.subtitle.isNotEmpty())
                t.trailing.text = row.trailing
                t.chevron.visible(row.chevron)
                // a colored tile, or the artwork (songs on the card), or nothing
                t.tileBox.removeViews(1, t.tileBox.childCount - 1)
                t.tileBox.visible(row.tile != null || row.thumb != null)
                t.art.visible(row.tile == null && row.thumb != null)
                if (row.tile != null) {
                    t.tileBox.addView(IconTile(c, row.tile), flp(MATCH, MATCH))
                    ImageLoader.load(t.art, null, 0)
                } else if (row.thumb != null) {
                    ImageLoader.load(t.art, row.thumb, c.dp(26), c.dp(26))
                }
                t.tileBox.alpha = if (row.enabled) 1f else 0.4f
                view.background = if (isSelected) gradient(Palette.accentTop, Palette.accentBottom) else null
                t.line.visible(!isSelected)
                t.title.setTextColor(if (isSelected) Palette.white else if (!row.enabled) Palette.shellDisabled else Palette.shellText)
                t.subtitle.setTextColor(if (isSelected) Color.argb(220, 255, 255, 255) else Palette.shellMuted)
                t.trailing.setTextColor(if (isSelected) Palette.white else if (!row.enabled) Palette.shellDisabled else Palette.shellMuted)
                t.chevron.color = if (isSelected) Palette.white else Palette.shellChevron
                t.chevron.invalidate()
            }
            is MediaTag -> {
                t.title.text = row.title
                t.subtitle.text = row.subtitle
                t.subtitle.visible(row.subtitle.isNotEmpty())
                t.trailing.text = row.trailing
                t.trailing.visible(row.trailing.isNotEmpty())
                t.chevron.visible(row.chevron && style == RowStyle.MUSIC)
                t.card.background = if (isSelected) rounded(Palette.focus, c.dpf(7f)) else null
                t.title.setTextColor(if (isSelected) Palette.focusText else Palette.white)
                t.title.font(isSelected)
                t.subtitle.setTextColor(if (isSelected) Palette.focusMuted else Palette.darkMuted)
                if (style == RowStyle.MUSIC) t.trailing.setTextColor(if (isSelected) Palette.focusMuted else Palette.darkMuted)
                t.chevron.color = if (isSelected) Palette.focusMuted else Palette.darkFaint
                t.chevron.invalidate()
                // a colored tile instead of artwork (YouTube Home's Shorts row), under the length badge
                t.artFrame.findViewWithTag<View>(TILE)?.let { t.artFrame.removeView(it) }
                if (row.tile != null) {
                    t.artFrame.addView(IconTile(c, row.tile).apply { tag = TILE }, 1, flp(MATCH, MATCH))
                    ImageLoader.load(t.art, null, 0)
                } else if (style == RowStyle.VIDEO) {
                    ImageLoader.load(t.art, row.thumb, c.dp(VIDEO_W), c.dp(VIDEO_H))
                } else {
                    ImageLoader.load(t.art, row.thumb, c.dp(MUSIC_ART), c.dp(MUSIC_ART))
                }
            }
        }
    }
}

// what the artwork of a row is cached as (for colors taken from the selected row's artwork)
fun Context.rowArt(style: RowStyle, thumb: String?): android.graphics.Bitmap? = when (style) {
    RowStyle.SHELL -> ImageLoader.cached(thumb, dp(26), dp(26))
    RowStyle.MUSIC -> ImageLoader.cached(thumb, dp(RowViews.MUSIC_ART), dp(RowViews.MUSIC_ART))
    RowStyle.VIDEO -> ImageLoader.cached(thumb, dp(RowViews.VIDEO_W), dp(RowViews.VIDEO_H))
}
