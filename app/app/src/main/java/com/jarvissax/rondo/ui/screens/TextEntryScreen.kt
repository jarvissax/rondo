package com.jarvissax.rondo.ui.screens

import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.ShellStatusBar
import com.jarvissax.rondo.ui.TvKeyboard
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.dpf
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.rounded
import com.jarvissax.rondo.ui.text

// One line of text typed on the wheel keyboard, in the shell's look: playlist and station names, a
// server's address and connect code. `onDone` answers null when it took the text (the screen
// closes, then `then` runs, for a next step) or a short problem to show above the keyboard.
// Menu on the keyboard's top row leaves without saving.
class TextEntryScreen(
    shell: ShellActivity,
    private val title: String,
    initial: String = "",
    private val action: String = "Done",
    private val note: String = "",
    private val maxLength: Int = 60,
    private val shown: (String) -> String = { it }, // how the typed text is displayed (a code as 123 456)
    private val onDone: suspend (String) -> String?,
    private val then: ((String) -> Unit)? = null,
) : Screen(shell) {
    private val value = StringBuilder(initial)
    private lateinit var field: TextView
    private lateinit var message: TextView
    private lateinit var keyboard: TvKeyboard
    private var working = false

    override fun build(): View {
        val c = context
        val root = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Palette.shellBg)
        }
        root.addView(ShellStatusBar(c, title), lp(MATCH, c.dp(22)))
        val bar = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(10), 0, c.dp(10), 0)
            background = rounded(Palette.shellHeader, c.dpf(8f), Palette.shellRowLine, 1)
        }
        field = c.text(14, Palette.shellText)
        bar.addView(field, lp(0, WRAP, 1f))
        root.addView(bar, lp(MATCH, c.dp(26)).apply { setMargins(c.dp(8), c.dp(5), c.dp(8), 0) })
        message = c.text(10, Palette.shellMuted).apply { gravity = Gravity.CENTER }
        root.addView(message, lp(MATCH, WRAP).apply { setMargins(c.dp(8), c.dp(3), c.dp(8), c.dp(3)) })
        keyboard = TvKeyboard(c, actionLabel = action, shell = true)
        keyboard.onKey = { onKey(it) }
        root.addView(keyboard, lp(MATCH, 0, 1f).apply { setMargins(c.dp(5), 0, c.dp(5), 0) })
        root.addView(c.text(9, Palette.shellMuted).apply {
            text = "wheel: next key · center: type · Menu up · Play down · Prev/Next left/right"
            gravity = Gravity.CENTER
        }, lp(MATCH, WRAP).apply { setMargins(0, c.dp(3), 0, c.dp(4)) })
        render(note)
        return root
    }

    private fun onKey(key: String) {
        if (working) return
        when (key) {
            TvKeyboard.DELETE -> if (value.isNotEmpty()) value.setLength(value.length - 1)
            TvKeyboard.SEARCH -> return finish()
            else -> if (value.length < maxLength) value.append(key)
        }
        render(note)
    }

    private fun finish() {
        val text = value.toString().trim()
        working = true
        render("Just a moment…")
        launch {
            val problem = try {
                onDone(text)
            } catch (e: Exception) {
                e.message ?: "That didn't work"
            }
            working = false
            if (problem == null) {
                shell.pop()
                then?.invoke(text)
            } else {
                render(problem)
            }
        }
    }

    private fun render(line: String) {
        field.text = shown(value.toString()) + "|"
        message.text = line
        message.setTextColor(if (line == note || working) Palette.shellMuted else Palette.accent)
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean = working || keyboard.handle(action)
}
