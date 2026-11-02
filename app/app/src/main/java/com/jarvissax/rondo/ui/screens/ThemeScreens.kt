package com.jarvissax.rondo.ui.screens

import android.graphics.Color
import com.jarvissax.rondo.Config
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.ui.IconView
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.Tile
import com.jarvissax.rondo.ui.Tiles
import com.jarvissax.rondo.ui.lit

// Settings › Theme: what the dashboard's Theme page changes (Config.theme), on the device. Every
// change redraws the screens at once (ShellActivity.rebuildScreens).
class ThemeScreen(shell: ShellActivity) : ShellListScreen(shell, "Theme") {
    override fun render() {
        val t = Config.theme
        list.setRows(
            listOf(
                Row.Entry("Accent color", trailing = accentName(t.accent), tile = swatch(t.accent), chevron = true, onSelect = { shell.push(AccentScreen(shell)) }),
                Row.Entry("Appearance", trailing = if (t.appearance == "dark") "Dark" else "Light", tile = Tiles.theme, onSelect = {
                    updateTheme { it.copy(appearance = if (it.appearance == "dark") "light" else "dark") }
                }),
                Row.Entry("Text size", trailing = textSizeName(t.textScale), tile = Tiles.lyrics, chevron = true, onSelect = { shell.push(TextSizeScreen(shell)) }),
                Row.Entry("Main menu", trailing = "${t.menu.count { it.visible }} of ${t.menu.size} shown", tile = Tiles.accel, chevron = true, onSelect = { shell.push(MenuOrderScreen(shell)) }),
            ),
            keepSelection = true,
        )
    }
}

fun updateTheme(change: (Config.Theme) -> Config.Theme) =
    Config.update(Config.Change.THEME) { d -> d.copy(theme = change(d.theme)) }

fun accentName(hex: String): String = Config.PRESETS.firstOrNull { it.second.equals(hex, ignoreCase = true) }?.first ?: "Custom"

// a tile in that color, with the selection bar's lit top
fun swatch(hex: String, glyph: Boolean = false): Tile {
    val c = runCatching { Color.parseColor(hex) }.getOrDefault(Color.GRAY)
    return Tile(IconView.Kind.CHECK, lit(c), c, glyph = glyph)
}

fun textSizeName(scale: Float): String = when {
    scale < 1.3f -> "Small"
    scale > 1.5f -> "Large"
    else -> "Medium"
}

// the dashboard's presets; a color picked there (any color) shows as Custom on top
class AccentScreen(shell: ShellActivity) : ShellListScreen(shell, "Accent color") {
    override fun render() {
        val now = Config.theme.accent
        val custom = Config.PRESETS.none { it.second.equals(now, ignoreCase = true) }
        val rows = mutableListOf<Row>()
        if (custom) rows += Row.Entry("Custom", subtitle = now.uppercase(), trailing = "Current", tile = swatch(now, glyph = true), enabled = false)
        Config.PRESETS.forEach { (name, hex) ->
            val current = hex.equals(now, ignoreCase = true)
            rows += Row.Entry(name, trailing = if (current) "Current" else "", tile = swatch(hex, glyph = current), onSelect = {
                updateTheme { it.copy(accent = hex) }
            })
        }
        list.setRows(rows, keepSelection = true)
    }
}

class TextSizeScreen(shell: ShellActivity) : ChoiceScreen<Float>(shell, "Text size") {
    override val choices = Config.TEXT_SCALES.map { textSizeName(it) to it }
    override fun current() = Config.theme.textScale
    override fun note(value: Float) = if (value == Config.Theme().textScale) "standard" else ""
    override fun pick(value: Float) = updateTheme { it.copy(textScale = value) }
}

// Center shows or hides the highlighted entry (Settings always stays); Next and Previous move it
// down and up, like dragging it on the dashboard.
class MenuOrderScreen(shell: ShellActivity) : ShellListScreen(shell, "Main menu") {
    private var focus: String? = null
    override val keepRowOnRebuild = false

    override fun render() {
        val menu = Config.theme.menu
        val rows = listOf<Row>(Row.Header("Center: show or hide · Next/Prev: move")) + menu.map { m ->
            val locked = m.id == "settings"
            Row.Entry(
                Config.MENU_LABELS[m.id] ?: m.id,
                trailing = if (locked) "Always shown" else if (m.visible) "Shown" else "Hidden",
                onSelect = {
                    focus = m.id
                    if (!locked) updateTheme { t -> t.copy(menu = t.menu.map { if (it.id == m.id) it.copy(visible = !it.visible) else it }) }
                },
            )
        }
        list.setRows(rows)
        list.select(1 + menu.indexOfFirst { it.id == focus }.coerceAtLeast(0))
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean {
        if (action != WheelAction.Next && action != WheelAction.Prev) return super.onAction(action, steps)
        val menu = Config.theme.menu
        val at = list.selected - 1
        val to = at + if (action == WheelAction.Next) 1 else -1
        if (at !in menu.indices || to !in menu.indices) return true
        focus = menu[at].id
        updateTheme { t -> t.copy(menu = t.menu.toMutableList().apply { add(to, removeAt(at)) }) }
        return true
    }
}
