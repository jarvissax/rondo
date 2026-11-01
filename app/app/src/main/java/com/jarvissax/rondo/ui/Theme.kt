package com.jarvissax.rondo.ui

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.math.roundToInt
import com.jarvissax.rondo.Config

// The panel is 480x360 on a 2.4" screen (~250 ppi) but reports mdpi, so mockup pixels came out
// tiny on the device. Everything sized through dp() is scaled up by UI_SCALE (1.4 by
// default; the dashboard offers 1.25 / 1.4 / 1.55).
val UI_SCALE: Float get() = Config.theme.textScale

object Palette {
    // iPod-style shell: brushed bars, a list, a selection bar in the accent color; light (the iPod
    // Classic look) or dark, chosen from the dashboard (Config.theme)
    private val darkShell get() = Config.theme.appearance == "dark"
    private fun pick(light: String, dark: String) = Color.parseColor(if (darkShell) dark else light)

    val shellBg get() = pick("#ffffff", "#121214")
    val shellBarTop get() = pick("#fbfbfb", "#2c2c31")
    val shellBarBottom get() = pick("#cfd1d6", "#17171a")
    val shellBarLine get() = pick("#9fa2a8", "#2e2e33")
    val shellRowLine get() = pick("#e6e6e6", "#232327")
    val shellHeader get() = pick("#f2f2f4", "#1b1b1e")
    val shellText get() = pick("#111111", "#f2f2f4")
    val shellMuted get() = pick("#7a7a7f", "#8e8e93")
    val shellDisabled get() = pick("#b0b0b4", "#4a4a4f")
    val shellChevron get() = pick("#9a9aa0", "#6e6e73")
    val shellIcon get() = pick("#3b3b40", "#d0d0d4")
    val shellTitleShadow get() = if (darkShell) Color.TRANSPARENT else Color.WHITE
    val accent get() = runCatching { Color.parseColor(Config.theme.accent) }.getOrDefault(Color.parseColor("#1c5ccc"))
    val accentTop get() = lit(accent) // the selection bar's lit top
    val accentBottom get() = accent
    val playing get() = accent

    // YouTube / YouTube Music
    val dark = Color.parseColor("#0f0f0f")
    val darkRaised = Color.parseColor("#232323")
    val darkBar = Color.parseColor("#181818")
    val darkPanel = Color.parseColor("#212121")
    val darkPanelSelected = Color.parseColor("#3a3a3a")
    val darkMuted = Color.parseColor("#aaaaaa")
    val darkFaint = Color.parseColor("#717171")
    val darkTrack = Color.parseColor("#4a4a4a")
    val focus = Color.parseColor("#f1f1f1") // focused row, YouTube-on-TV style
    val focusText = Color.parseColor("#0f0f0f")
    val focusMuted = Color.parseColor("#4d4d4d")
    val red = Color.parseColor("#ff0033")
    val overlay = Color.argb(184, 0, 0, 0)
    val white = Color.WHITE
}

fun Context.dp(v: Int): Int = (v * resources.displayMetrics.density * UI_SCALE).roundToInt()
fun Context.dpf(v: Float): Float = v * resources.displayMetrics.density * UI_SCALE
fun Context.screenWidth(): Int = resources.displayMetrics.widthPixels

fun Context.text(
    size: Int,
    color: Int,
    bold: Boolean = false,
    lines: Int = 1,
): TextView = TextView(this).apply {
    setTextSize(TypedValue.COMPLEX_UNIT_PX, dpf(size.toFloat()))
    setTextColor(color)
    font(bold) // bundled Roboto (Fonts.kt)
    includeFontPadding = false
    if (lines == 1) setSingleLine(true) else maxLines = lines
    ellipsize = android.text.TextUtils.TruncateAt.END
}

fun gradient(top: Int, bottom: Int, radius: Float = 0f): GradientDrawable =
    GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(top, bottom)).apply { cornerRadius = radius }

fun rounded(color: Int, radius: Float, strokeColor: Int? = null, strokePx: Int = 0): GradientDrawable = GradientDrawable().apply {
    setColor(color)
    cornerRadius = radius
    if (strokeColor != null) setStroke(strokePx, strokeColor)
}

fun lp(w: Int, h: Int, weight: Float = 0f) = LinearLayout.LayoutParams(w, h, weight)
fun flp(w: Int, h: Int, gravity: Int = -1) = FrameLayout.LayoutParams(w, h, gravity)

const val MATCH = ViewGroup.LayoutParams.MATCH_PARENT
const val WRAP = ViewGroup.LayoutParams.WRAP_CONTENT

fun View.visible(show: Boolean) {
    visibility = if (show) View.VISIBLE else View.GONE
}

fun formatTime(ms: Long): String {
    val s = (ms / 1000).coerceAtLeast(0)
    return if (s >= 3600) String.format("%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60) else String.format("%d:%02d", s / 60, s % 60)
}

// TextView.setText always lays the view out again, even for the same text; on the Y1 that
// layout pass, every 500 ms from the tick, stalls running animations. Set only real changes.
fun TextView.show(value: CharSequence) {
    if (!android.text.TextUtils.equals(text, value)) text = value
}

fun mix(a: Int, b: Int, t: Float): Int = Color.rgb(
    (Color.red(a) + (Color.red(b) - Color.red(a)) * t).toInt(),
    (Color.green(a) + (Color.green(b) - Color.green(a)) * t).toInt(),
    (Color.blue(a) + (Color.blue(b) - Color.blue(a)) * t).toInt(),
)

// The selection bar's lighter top for an accent: in HSL, hue -8, saturation +12, lightness up by
// min(20, (90 - l) * 0.45). Gives the original iPod pair #5aa8f6 -> #1c5ccc; the dashboard's
// preview (assets/dashboard/app.js accentTop) uses the same formula.
fun lit(color: Int): Int {
    val r = Color.red(color) / 255f
    val g = Color.green(color) / 255f
    val b = Color.blue(color) / 255f
    val max = maxOf(r, g, b)
    val min = minOf(r, g, b)
    val l = (max + min) / 2
    var h = 0f
    var s = 0f
    if (max != min) {
        val d = max - min
        s = if (l > 0.5f) d / (2 - max - min) else d / (max + min)
        h = when (max) {
            r -> (g - b) / d + (if (g < b) 6 else 0)
            g -> (b - r) / d + 2
            else -> (r - g) / d + 4
        } * 60
    }
    val h2 = ((h - 8) % 360 + 360) % 360
    val s2 = (s * 100 + 12).coerceIn(0f, 100f) / 100
    val l2 = (l * 100 + minOf(20f, maxOf(0f, 90 - l * 100) * 0.45f)).coerceIn(0f, 100f) / 100
    val a = s2 * minOf(l2, 1 - l2)
    fun f(n: Int): Int {
        val k = (n + h2 / 30) % 12
        return ((l2 - a * maxOf(-1f, minOf(k - 3, minOf(9 - k, 1f)))) * 255).toInt().coerceIn(0, 255)
    }
    return Color.rgb(f(0), f(8), f(4))
}
