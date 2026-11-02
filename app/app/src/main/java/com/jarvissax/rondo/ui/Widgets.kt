package com.jarvissax.rondo.ui

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Bitmap
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.drawable.BitmapDrawable
import android.net.ConnectivityManager
import android.os.BatteryManager
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView

fun batteryLevel(context: Context): Float {
    val i = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)) ?: return 0.8f
    return i.getIntExtra(BatteryManager.EXTRA_LEVEL, 80) / i.getIntExtra(BatteryManager.EXTRA_SCALE, 100).toFloat()
}

fun isOnline(context: Context): Boolean {
    val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    @Suppress("DEPRECATION")
    return cm.activeNetworkInfo?.isConnected == true
}

fun Context.line(color: Int): View = View(this).apply { setBackgroundColor(color) }

// iPod status bar: brushed gradient, play state, screen title, Wi-Fi and battery.
class ShellStatusBar(context: Context, title: String) : FrameLayout(context) {
    private val play = IconView(context, IconView.Kind.PLAY, Palette.playing)
    private val wifi = IconView(context, IconView.Kind.WIFI, Palette.shellIcon)
    private val battery = IconView(context, IconView.Kind.BATTERY, Palette.shellIcon)
    private val percent = context.text(11, Palette.shellIcon, bold = true)

    init {
        background = gradient(Palette.shellBarTop, Palette.shellBarBottom)
        addView(play, flp(context.dp(12), context.dp(12), Gravity.CENTER_VERTICAL or Gravity.START).apply { leftMargin = context.dp(10) })
        addView(context.text(15, Palette.shellText, bold = true).apply {
            text = title
            setShadowLayer(1f, 0f, 1f, Palette.shellTitleShadow)
        }, flp(WRAP, WRAP, Gravity.CENTER))
        val right = LinearLayout(context).apply { gravity = Gravity.CENTER_VERTICAL }
        right.addView(wifi, lp(context.dp(14), context.dp(14)).apply { rightMargin = context.dp(6) })
        right.addView(percent, lp(WRAP, WRAP).apply { rightMargin = context.dp(3) })
        right.addView(battery, lp(context.dp(24), context.dp(14)))
        addView(right, flp(WRAP, WRAP, Gravity.CENTER_VERTICAL or Gravity.END).apply { rightMargin = context.dp(10) })
        addView(context.line(Palette.shellBarLine), flp(MATCH, 1, Gravity.BOTTOM))
        refresh(false)
    }

    fun refresh(playing: Boolean) {
        play.visibility = if (playing) View.VISIBLE else View.INVISIBLE
        wifi.alpha = if (isOnline(context)) 1f else 0.25f
        val level = batteryLevel(context)
        battery.level = level
        percent.show("${Math.round(level * 100)}%")
    }
}

// Wi-Fi, battery percentage and battery icon for the dark headers; keeps itself current (every
// 30 s) while on screen, since Now Playing and the lyrics stay open for whole albums.
// Wi-Fi (dimmed when offline), battery percentage and battery. showWifi = false: battery only, for
// the YouTube / YouTube Music headers, whose pills fill the row
class StatusIcons(context: Context, color: Int, private val showWifi: Boolean = true) : LinearLayout(context) {
    private val wifi = IconView(context, IconView.Kind.WIFI, color)
    private val percent = context.text(10, color, bold = true)
    private val battery = IconView(context, IconView.Kind.BATTERY, color)
    private val tick = object : Runnable {
        override fun run() {
            refresh()
            postDelayed(this, 30_000)
        }
    }

    init {
        gravity = Gravity.CENTER_VERTICAL
        if (showWifi) addView(wifi, lp(context.dp(11), context.dp(11)).apply { rightMargin = context.dp(5) })
        addView(percent, lp(WRAP, WRAP).apply { rightMargin = context.dp(3) })
        addView(battery, lp(context.dp(18), context.dp(11)))
        refresh()
    }

    fun refresh() {
        wifi.alpha = if (isOnline(context)) 1f else 0.3f
        val level = batteryLevel(context)
        battery.level = level
        percent.show("${Math.round(level * 100)}%")
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        post(tick)
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(tick)
        super.onDetachedFromWindow()
    }

    // A header that runs out of room (YouTube's three pills) drops the percentage and keeps the
    // battery, whose fill and color still show the level. Measuring can't tell (a view after a
    // weighted spacer is offered the whole row), so look once laid out; a rebuilt screen starts over.
    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        super.onLayout(changed, l, t, r, b)
        val row = parent as? View ?: return
        if (percent.visibility == View.VISIBLE && r > row.width - row.paddingRight) post { percent.visibility = View.GONE }
    }
}

// Colored rounded tile with a white glyph, for menu rows (gives the shell some color). The two
// YouTube services show their own logo on a white tile instead, like their app icons.
enum class Brand { YOUTUBE, YOUTUBE_MUSIC }

data class Tile(val kind: IconView.Kind, val top: Int, val bottom: Int, val brand: Brand? = null)

object Tiles {
    // top/bottom of the brand tiles: the menu preview's dark backdrop (the YouTube sections are dark)
    val music = Tile(IconView.Kind.NOTE, Color.parseColor("#3a3a3a"), Color.parseColor("#141414"), Brand.YOUTUBE_MUSIC)
    val video = Tile(IconView.Kind.PLAY, Color.parseColor("#3a3a3a"), Color.parseColor("#141414"), Brand.YOUTUBE)
    val device = Tile(IconView.Kind.FOLDER, Color.parseColor("#5fb7ff"), Color.parseColor("#1f73e0"))
    val nowPlaying = Tile(IconView.Kind.BARS, Color.parseColor("#7be07b"), Color.parseColor("#26a83b"))
    val settings = Tile(IconView.Kind.GEAR, Color.parseColor("#b8bcc4"), Color.parseColor("#6f747d"))
    val wifi = Tile(IconView.Kind.WIFI, Color.parseColor("#5fb7ff"), Color.parseColor("#1f73e0"))
    val bluetooth = Tile(IconView.Kind.BLUETOOTH, Color.parseColor("#6a8cff"), Color.parseColor("#2c4fd6"))
    val quality = Tile(IconView.Kind.PLAY, Color.parseColor("#ffb04d"), Color.parseColor("#f07a12"))
    val server = Tile(IconView.Kind.CLOUD, Color.parseColor("#7be07b"), Color.parseColor("#26a83b"))
    val about = Tile(IconView.Kind.INFO, Color.parseColor("#b8bcc4"), Color.parseColor("#6f747d"))
    val songs = Tile(IconView.Kind.NOTE, Color.parseColor("#ff5e7e"), Color.parseColor("#e0144c"))
    val artists = Tile(IconView.Kind.PERSON, Color.parseColor("#c58cff"), Color.parseColor("#7b3fe0"))
    val albums = Tile(IconView.Kind.DISC, Color.parseColor("#ffb04d"), Color.parseColor("#f07a12"))
    val dashboard = Tile(IconView.Kind.SCREEN, Color.parseColor("#3fd0c9"), Color.parseColor("#0f8f96"))
    val brightness = Tile(IconView.Kind.SUN, Color.parseColor("#ffd24d"), Color.parseColor("#f0a012"))
    val timer = Tile(IconView.Kind.TIMER, Color.parseColor("#9a8cff"), Color.parseColor("#5a3fe0"))
    val radio = Tile(IconView.Kind.RADIO, Color.parseColor("#ffb04d"), Color.parseColor("#f07a12"))
    val lyrics = Tile(IconView.Kind.NOTE, Color.parseColor("#ff8fc8"), Color.parseColor("#d6338a"))
    val sound = Tile(IconView.Kind.VOLUME, Color.parseColor("#7be07b"), Color.parseColor("#26a83b"))
    val accel = Tile(IconView.Kind.BARS, Color.parseColor("#6a8cff"), Color.parseColor("#2c4fd6"))
    val wheel = Tile(IconView.Kind.DISC, Color.parseColor("#6a8cff"), Color.parseColor("#2c4fd6")) // as .t-wheel in the dashboard
    val videos = Tile(IconView.Kind.PLAY, Color.parseColor("#ff6b5a"), Color.parseColor("#e01e1e"))
    val shuffle = Tile(IconView.Kind.SHUFFLE, Color.parseColor("#7be07b"), Color.parseColor("#26a83b"))
}

class IconTile(context: Context, tile: Tile) : FrameLayout(context) {
    init {
        when (tile.brand) {
            null -> {
                background = gradient(tile.top, tile.bottom, context.dpf(7f))
                addView(IconView(context, tile.kind, Color.WHITE), flp(context.dp(16), context.dp(16), Gravity.CENTER))
            }
            Brand.YOUTUBE -> {
                background = rounded(Color.WHITE, context.dpf(7f), Color.argb(28, 0, 0, 0), 1)
                addView(VideoLogo(context), flp(context.dp(19), context.dp(14), Gravity.CENTER))
            }
            Brand.YOUTUBE_MUSIC -> {
                background = rounded(Color.WHITE, context.dpf(7f), Color.argb(28, 0, 0, 0), 1)
                addView(MusicLogo(context), flp(context.dp(19), context.dp(19), Gravity.CENTER))
            }
        }
    }
}

// ImageView with rounded corners drawn through a BitmapShader (clipPath is not hardware
// accelerated on API 17). Center-crops like CENTER_CROP.
class RoundedImageView(context: Context, private val radiusPx: Float, private val placeholder: Int) : ImageView(context) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    private val rect = RectF()
    private val matrix2 = Matrix()

    override fun onDraw(canvas: Canvas) {
        rect.set(0f, 0f, width.toFloat(), height.toFloat())
        val bitmap = (drawable as? BitmapDrawable)?.bitmap
        if (bitmap == null) {
            paint.shader = null
            paint.color = placeholder
            canvas.drawRoundRect(rect, radiusPx, radiusPx, paint)
            return
        }
        val shader = BitmapShader(bitmap, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP)
        val scale = maxOf(width / bitmap.width.toFloat(), height / bitmap.height.toFloat())
        matrix2.setScale(scale, scale)
        matrix2.postTranslate((width - bitmap.width * scale) / 2, (height - bitmap.height * scale) / 2)
        shader.setLocalMatrix(matrix2)
        paint.color = Color.BLACK // opaque: a translucent placeholder color would fade the artwork
        paint.shader = shader
        canvas.drawRoundRect(rect, radiusPx, radiusPx, paint)
    }
}

// Color for backgrounds, taken from the artwork (average of a 12x12 sample, saturated).
object ArtColor {
    fun dominant(bitmap: Bitmap): Int {
        val small = Bitmap.createScaledBitmap(bitmap, 12, 12, true)
        var r = 0L
        var g = 0L
        var b = 0L
        for (x in 0 until 12) for (y in 0 until 12) {
            val c = small.getPixel(x, y)
            r += Color.red(c); g += Color.green(c); b += Color.blue(c)
        }
        val hsv = FloatArray(3)
        Color.RGBToHSV((r / 144).toInt(), (g / 144).toInt(), (b / 144).toInt(), hsv)
        hsv[1] = (hsv[1] * 1.4f).coerceAtMost(0.85f)
        hsv[2] = hsv[2].coerceIn(0.35f, 0.6f)
        return Color.HSVToColor(hsv)
    }

    // a soft color field from the artwork: shrink to a few pixels, the view scales it back up
    fun blurred(bitmap: Bitmap): Bitmap = Bitmap.createScaledBitmap(bitmap, 6, 6, true)
}

fun darker(color: Int, f: Float): Int = Color.rgb((Color.red(color) * f).toInt(), (Color.green(color) * f).toInt(), (Color.blue(color) * f).toInt())

// Thin progress bar with optional buffered part, round thumb and a scrub marker.
class ProgressBarView(
    context: Context,
    private val trackColor: Int,
    private val playedColor: Int,
    private val bufferedColor: Int? = null,
    private val thumbDp: Int = 0,
    private val barDp: Int = 4,
) : View(context) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    var fraction = 0f
        set(value) {
            field = value.coerceIn(0f, 1f)
            invalidate()
        }
    var buffered = 0f
        set(value) {
            field = value.coerceIn(0f, 1f)
            invalidate()
        }
    var marker: Float? = null // scrub target, drawn as a larger ring
        set(value) {
            field = value?.coerceIn(0f, 1f)
            invalidate()
        }

    override fun onDraw(canvas: Canvas) {
        val bar = context.dpf(barDp.toFloat())
        val r = context.dpf(thumbDp.toFloat()) / 2
        val left = maxOf(r, context.dpf(6f))
        val right = width - left
        val top = (height - bar) / 2
        val x = left + (right - left) * fraction
        paint.style = Paint.Style.FILL
        paint.color = trackColor
        canvas.drawRoundRect(RectF(left, top, right, top + bar), bar / 2, bar / 2, paint)
        bufferedColor?.let {
            paint.color = it
            canvas.drawRect(x, top, left + (right - left) * maxOf(buffered, fraction), top + bar, paint)
        }
        paint.color = playedColor
        canvas.drawRoundRect(RectF(left, top, x, top + bar), bar / 2, bar / 2, paint)
        if (r > 0) canvas.drawCircle(x, height / 2f, r, paint)
        marker?.let { m ->
            val mx = left + (right - left) * m
            paint.color = Color.WHITE
            canvas.drawCircle(mx, height / 2f, maxOf(r, context.dpf(6f)) * 1.25f, paint)
            paint.color = Palette.red
            canvas.drawCircle(mx, height / 2f, maxOf(r, context.dpf(6f)) * 0.7f, paint)
        }
    }
}

// Volume panel shown over any screen while the wheel changes STREAM_MUSIC.
class VolumeOverlay(context: Context) : FrameLayout(context) {
    private val bar = ProgressBarView(context, Palette.darkTrack, Palette.white, barDp = 5)
    private val label = context.text(15, Palette.white, bold = true)
    private val hide = Runnable { visibility = GONE }

    init {
        val panel = LinearLayout(context).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(context.dp(14), 0, context.dp(14), 0)
            background = rounded(Color.argb(235, 33, 33, 33), context.dpf(10f))
        }
        panel.addView(IconView(context, IconView.Kind.VOLUME, Palette.white), lp(context.dp(22), context.dp(22)).apply { rightMargin = context.dp(10) })
        panel.addView(bar, lp(0, context.dp(14), 1f))
        panel.addView(label, lp(context.dp(30), WRAP).apply { leftMargin = context.dp(8) })
        label.gravity = Gravity.END
        addView(panel, flp(context.dp(230), context.dp(50), Gravity.CENTER))
        visibility = GONE
    }

    fun show(level: Int, max: Int) {
        bar.fraction = level / max.toFloat()
        label.text = "$level"
        visibility = VISIBLE
        removeCallbacks(hide)
        postDelayed(hide, 1500)
    }
}

// Big time preview while scrubbing ("1:23:45", "+1:30").
class ScrubBubble(context: Context) : LinearLayout(context) {
    private val time = context.text(26, Palette.white, bold = true)
    private val delta = context.text(14, Palette.darkMuted)

    init {
        orientation = VERTICAL
        gravity = Gravity.CENTER
        setPadding(context.dp(16), context.dp(8), context.dp(16), context.dp(8))
        background = rounded(Color.argb(230, 20, 20, 20), context.dpf(10f))
        addView(time, lp(WRAP, WRAP))
        addView(delta, lp(WRAP, WRAP).apply { topMargin = context.dp(2) })
        visibility = GONE
    }

    fun show(targetMs: Long, fromMs: Long) {
        time.text = formatTime(targetMs)
        val d = targetMs - fromMs
        delta.text = (if (d >= 0) "+" else "−") + formatTime(Math.abs(d)) + "   center to jump"
        visibility = VISIBLE
    }

    fun hide() {
        visibility = GONE
    }
}

// Rounded "pill" tab (YouTube style): white when active, dark otherwise; ring when focused.
class Pill(context: Context, label: String) : TextView(context) {
    var active = false
        set(value) {
            field = value
            restyle()
        }
    var focusedRing = false
        set(value) {
            field = value
            restyle()
        }

    init {
        text = label
        setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, context.dpf(12.5f))
        gravity = Gravity.CENTER
        setPadding(context.dp(10), 0, context.dp(10), 0)
        setSingleLine(true)
        includeFontPadding = false
        restyle()
    }

    private fun restyle() {
        setTextColor(if (active) Palette.dark else Palette.white)
        font(active)
        background = rounded(
            if (active) Palette.white else Color.argb(60, 255, 255, 255), context.dpf(12f),
            if (focusedRing) Palette.red else null, context.dp(2),
        )
    }
}

private val BRAND_RED = Color.parseColor("#ff0000")

// YouTube Music logo, from the official icon's 176-unit grid: red disc (r 88), white ring (r 44,
// 4 wide), white play triangle. Centered in the view at its smaller side.
class MusicLogo(context: Context) : View(context) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val path = android.graphics.Path()
    override fun onDraw(canvas: Canvas) {
        val s = minOf(width, height).toFloat()
        val u = s / 176f
        canvas.save()
        canvas.translate((width - s) / 2, (height - s) / 2)
        paint.style = Paint.Style.FILL
        paint.color = BRAND_RED
        canvas.drawCircle(88 * u, 88 * u, 88 * u, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = maxOf(4 * u, 1.25f) // at row size the ring would be thinner than a pixel
        paint.color = Color.WHITE
        canvas.drawCircle(88 * u, 88 * u, 44 * u, paint)
        paint.style = Paint.Style.FILL
        path.reset()
        path.moveTo(72 * u, 111 * u); path.lineTo(111 * u, 87 * u); path.lineTo(72 * u, 65 * u); path.close()
        canvas.drawPath(path, paint)
        canvas.restore()
    }
}

// YouTube logo, the official icon's outline (28.57 × 20 grid): red play button, white triangle.
// Fitted into the view, centered.
class VideoLogo(context: Context) : View(context) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val button = android.graphics.Path()
    private val play = android.graphics.Path()

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        val k = minOf(w / 28.57f, h / 20f)
        val m = Matrix().apply { setScale(k, k); postTranslate((w - 28.57f * k) / 2, (h - 20f * k) / 2) }
        BUTTON.transform(m, button)
        PLAY.transform(m, play)
    }

    override fun onDraw(canvas: Canvas) {
        paint.color = BRAND_RED
        canvas.drawPath(button, paint)
        paint.color = Color.WHITE
        canvas.drawPath(play, paint)
    }

    private companion object {
        val BUTTON = android.graphics.Path().apply {
            moveTo(27.9727f, 3.12324f)
            cubicTo(27.6435f, 1.89323f, 26.6768f, 0.926623f, 25.4468f, 0.597366f)
            cubicTo(23.2197f, 0f, 14.285f, 0f, 14.285f, 0f)
            cubicTo(14.285f, 0f, 5.35042f, 0f, 3.12323f, 0.597366f)
            cubicTo(1.89323f, 0.926623f, 0.926623f, 1.89323f, 0.597366f, 3.12324f)
            cubicTo(0f, 5.35042f, 0f, 10f, 0f, 10f)
            cubicTo(0f, 10f, 0f, 14.6496f, 0.597366f, 16.8768f)
            cubicTo(0.926623f, 18.1068f, 1.89323f, 19.0734f, 3.12323f, 19.4026f)
            cubicTo(5.35042f, 20f, 14.285f, 20f, 14.285f, 20f)
            cubicTo(14.285f, 20f, 23.2197f, 20f, 25.4468f, 19.4026f)
            cubicTo(26.6768f, 19.0734f, 27.6435f, 18.1068f, 27.9727f, 16.8768f)
            cubicTo(28.5701f, 14.6496f, 28.5701f, 10f, 28.5701f, 10f)
            cubicTo(28.5701f, 10f, 28.5677f, 5.35042f, 27.9727f, 3.12324f)
            close()
        }
        val PLAY = android.graphics.Path().apply {
            moveTo(11.4253f, 14.2854f); lineTo(18.8477f, 10.0004f); lineTo(11.4253f, 5.71533f); close()
        }
    }
}
