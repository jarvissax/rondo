package com.jarvissax.rondo.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.View
import com.jarvissax.rondo.input.WheelAction

// On-screen keyboard navigated like a TV remote. The Y1's four buttons are the arrows while it
// is open: Next/Prev = right/left, Play = down, Menu = up (Menu on the top row closes it). The
// wheel walks the keys in reading order; center types the highlighted key.
// Three layouts so Wi-Fi passwords can be typed: lower case, upper case (ABC) and symbols (#+=).
// shell = true: the light (or dark) iPod-style look of the shell screens, with the action key in
// the accent color; otherwise YouTube's dark look.
class TvKeyboard(context: Context, private val actionLabel: String = "Search", private val shell: Boolean = false) : View(context) {
    data class Key(val label: String, val value: String, val row: Int, val col: Int, val span: Int = 1)

    private enum class Layout { LOWER, UPPER, SYMBOLS }

    private var layout = Layout.LOWER
    private var keys: List<Key> = build(layout)
    private var cursor = 0
    var onKey: ((String) -> Unit)? = null

    private val bg = Paint(Paint.ANTI_ALIAS_FLAG)
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER }
    private val rect = RectF()

    private fun build(l: Layout): List<Key> = buildList {
        fun row(r: Int, chars: String) = chars.forEachIndexed { i, c -> add(Key("$c", "$c", r, i)) }
        when (l) {
            Layout.LOWER -> {
                row(0, "abcdefghij"); row(1, "klmnopqrst"); row(2, "uvwxyz.-_@")
            }
            Layout.UPPER -> {
                row(0, "ABCDEFGHIJ"); row(1, "KLMNOPQRST"); row(2, "UVWXYZ'&!?")
            }
            Layout.SYMBOLS -> {
                row(0, "!@#$%^&*()"); row(1, "-_=+[]{};:"); row(2, "'\",./?<>\\|")
            }
        }
        row(3, "1234567890")
        if (l == Layout.SYMBOLS) {
            add(Key("abc", LOWER, 4, 0, 2))
            add(Key("~ `", "~", 4, 2, 1))
            add(Key("Space", " ", 4, 3, 3))
        } else {
            add(Key(if (l == Layout.LOWER) "ABC" else "abc", if (l == Layout.LOWER) UPPER else LOWER, 4, 0, 2))
            add(Key("#+=", SYMBOLS, 4, 2, 1))
            add(Key("Space", " ", 4, 3, 3))
        }
        add(Key("Del", DELETE, 4, 6, 2))
        add(Key(actionLabel, SEARCH, 4, 8, 2))
    }

    private fun switchTo(l: Layout) {
        val at = keys[cursor]
        layout = l
        keys = build(l)
        // stay on the same spot of the grid
        cursor = keys.indexOfFirst { it.row == at.row && at.col >= it.col && at.col < it.col + it.span }.coerceAtLeast(0)
    }

    // false = Menu on the top row: the screen closes the keyboard
    fun handle(action: WheelAction): Boolean {
        when (action) {
            WheelAction.Down -> cursor = (cursor + 1) % keys.size
            WheelAction.Up -> cursor = (cursor - 1 + keys.size) % keys.size
            WheelAction.Next, WheelAction.LongNext -> horizontal(+1)
            WheelAction.Prev, WheelAction.LongPrev -> horizontal(-1)
            WheelAction.PlayPause -> vertical(+1)
            WheelAction.Back -> if (keys[cursor].row == 0) return false else vertical(-1)
            WheelAction.Select -> when (val v = keys[cursor].value) {
                LOWER -> switchTo(Layout.LOWER)
                UPPER -> switchTo(Layout.UPPER)
                SYMBOLS -> switchTo(Layout.SYMBOLS)
                else -> onKey?.invoke(v)
            }
            else -> return false
        }
        invalidate()
        return true
    }

    private fun horizontal(dir: Int) {
        val k = keys[cursor]
        val sameRow = keys.filter { it.row == k.row }
        val i = sameRow.indexOf(k)
        cursor = keys.indexOf(sameRow[(i + dir + sameRow.size) % sameRow.size])
    }

    private fun vertical(dir: Int) {
        val k = keys[cursor]
        val row = k.row + dir
        if (row < 0 || row > 4) return
        val center = k.col + k.span / 2f
        val target = keys.filter { it.row == row }.minByOrNull { Math.abs(it.col + it.span / 2f - center) } ?: return
        cursor = keys.indexOf(target)
    }

    override fun onDraw(canvas: Canvas) {
        val cw = width / 10f
        val rh = height / 5f
        val gap = context.dp(2).toFloat()
        val radius = context.dp(4).toFloat()
        keys.forEachIndexed { i, k ->
            rect.set(k.col * cw + gap, k.row * rh + gap, (k.col + k.span) * cw - gap, (k.row + 1) * rh - gap)
            val on = i == cursor
            val special = k.value.length > 1 || k.value == DELETE || k.value == SEARCH
            bg.color = when {
                on -> if (shell) Palette.shellText else Palette.white
                k.value == SEARCH -> if (shell) Palette.accent else Palette.red
                special -> if (shell) mix(Palette.shellHeader, Palette.shellText, 0.12f) else Palette.darkPanelSelected
                else -> if (shell) Palette.shellHeader else Palette.darkRaised
            }
            canvas.drawRoundRect(rect, radius, radius, bg)
            label.color = when {
                on -> if (shell) Palette.shellBg else Palette.dark
                shell && k.value != SEARCH -> Palette.shellText
                else -> Palette.white
            }
            label.textSize = context.dp(if (k.label.length > 1) 12 else 15).toFloat()
            label.font(bold = on)
            canvas.drawText(k.label, rect.centerX(), rect.centerY() - (label.descent() + label.ascent()) / 2, label)
        }
    }

    companion object {
        const val DELETE = "\b"
        const val SEARCH = "\n" // the action key (Search, Join…)
        private const val LOWER = "\u0001lower"
        private const val UPPER = "\u0001upper"
        private const val SYMBOLS = "\u0001symbols"
    }
}
