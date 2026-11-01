package com.jarvissax.rondo.ui

import android.animation.Animator
import android.animation.AnimatorListenerAdapter
import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.Shader
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.view.View
import android.view.animation.DecelerateInterpolator
import com.jarvissax.rondo.net.LyricLine
import com.jarvissax.rondo.net.Lyrics

// Lyrics that follow the song, easy to read and follow. Timed
// lyrics: the sung line is white, larger and bold at 40 % of the height, lines already sung fade
// out above, the next ones wait below in gray; the list glides to each new line exactly on its
// timestamp. Untimed lyrics scroll with the song's progress until the wheel takes over.
//
// Smoothness on the MT6572 (measured with dumpsys gfxinfo, 2026-10-31): drawing the text every
// frame cost 12-140 ms of GPU time per frame on Android 4.2, so the lines are drawn once, in
// software, into an ALPHA_8 bitmap covering three screens, and each animation frame only moves
// that bitmap (one textured quad). A change of sung line cross-fades the old and the new bitmap
// while they slide; the next line's bitmap is drawn ahead of time. The soft top and bottom edges
// are a gradient shader on the bitmap paint, fixed to the screen.
class LyricsView(context: Context, private val position: () -> Long, private val duration: () -> Long) : View(context) {
    private val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = context.dpf(15f)
        font()
    }
    private val bigPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = context.dpf(17.5f)
        font(bold = true) // a stroke: free in software, it is drawn once per line change
    }
    private val bitmapPaint = Paint(Paint.FILTER_BITMAP_FLAG).apply { color = Color.WHITE }
    private val gap = context.dpf(9f)
    private val side = context.dp(16)
    private val tag = Regex("^\\[[A-Za-z#]{1,10}:[^\\]]*]$")

    private var lines: List<LyricLine> = emptyList() // timed, or plain lines with t = 0
    private var timed = false
    private var layouts: List<StaticLayout> = emptyList()
    private var bigLayouts: List<StaticLayout> = emptyList() // the same lines, larger, for the sung one
    private var tops = FloatArray(0) // y of each line in the list
    private var slots = FloatArray(0) // room each line takes: the larger of its two sizes
    private var total = 0f
    private var current = -1
    private var offset = 0f // list y shown at the anchor
    private var message: String? = "Finding lyrics…"
    private var manual = false // untimed lyrics scrolled with the wheel: stop following the song

    // a rendered window of the list: list y `top` is the bitmap's first row
    private class Frame(val bitmap: Bitmap, val top: Float, val highlight: Int)
    private var shown: Frame? = null
    private var fading: Frame? = null // the previous highlight, fading out while sliding
    private var prepared: Frame? = null // the next line's, drawn ahead of time
    private val pool = ArrayList<Bitmap>()
    private var fade = 1f // 0..1 progress of the cross-fade
    private var slideFrom = 0f
    private var slideTo = 0f

    private val animator = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = 450
        interpolator = DecelerateInterpolator(1.6f)
        addUpdateListener {
            val f = it.animatedValue as Float
            // the highlight dissolves in the first 45 % of the time (~200 ms) while the slide eases
            // on: two sizes of the same line overlap only briefly
            val t = (it.animatedFraction / 0.45f).coerceAtMost(1f)
            fade = t * t * (3 - 2 * t)
            offset = slideFrom + (slideTo - slideFrom) * f
            invalidate()
        }
        addListener(object : AnimatorListenerAdapter() {
            override fun onAnimationEnd(animation: Animator) {
                fading?.let { recycle(it) }
                fading = null
                fade = 1f
                post { prepareNext() }
            }
        })
    }

    private val poll = object : Runnable {
        override fun run() {
            update(animate = true)
            postDelayed(this, nextDelay())
        }
    }

    fun showMessage(text: String) {
        message = text
        lines = emptyList()
        layouts = emptyList()
        clearFrames()
        invalidate()
    }

    fun show(lyrics: Lyrics) {
        val synced = lyrics.synced?.takeIf { it.isNotEmpty() }
        message = null
        timed = synced != null
        manual = false
        // plain text can still carry LRC tags like [by:…] or [ar:…]: not lyrics
        lines = synced ?: lyrics.plain?.lines()?.map { it.trim() }?.filterNot { tag.matches(it) }?.map { LyricLine(0, it) }?.dropWhile { it.text.isEmpty() } ?: emptyList()
        if (lines.isEmpty()) message = if (lyrics.instrumental) "Instrumental" else "No lyrics found for this song"
        current = -1
        relayout()
        update(animate = false)
        invalidate() // update() returns early when there are no lines: the message must still be drawn
    }

    private fun relayout() {
        clearFrames()
        val w = width - side * 2
        if (w <= 0 || lines.isEmpty()) {
            layouts = emptyList()
            return
        }
        // a pause between verses shows as three dots
        val texts = lines.map { l -> l.text.ifEmpty { if (timed) "•  •  •" else " " } }
        layouts = texts.map { layout(it, w) }
        bigLayouts = if (timed) texts.map { layout(it, w, bigPaint) } else layouts
        tops = FloatArray(layouts.size)
        slots = FloatArray(layouts.size) { maxOf(layouts[it].height, bigLayouts[it].height).toFloat() }
        var y = 0f
        layouts.indices.forEach { i ->
            tops[i] = y
            y += slots[i] + gap
        }
        total = y
    }

    @Suppress("DEPRECATION")
    private fun layout(text: String, w: Int, p: TextPaint = paint) = StaticLayout(text, p, w, Layout.Alignment.ALIGN_NORMAL, 1.1f, 0f, false)

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        pool.clear()
        shaderAt = Int.MIN_VALUE
        relayout()
        update(animate = false)
    }

    private val anchor: Float get() = if (timed) height * 0.40f else height * 0.15f
    private val window: Int get() = height * 3

    // list y that sits at the anchor: the middle of the sung line (before the first: the top)
    private fun anchorFor(i: Int): Float = if (i < 0) -slots[0] / 2f - gap * 2 else tops[i] + slots[i] / 2f

    private fun sungIndex(pos: Long): Int {
        var lo = 0
        var hi = lines.lastIndex
        var found = -1
        while (lo <= hi) {
            val mid = (lo + hi) ushr 1
            if (lines[mid].t <= pos) {
                found = mid
                lo = mid + 1
            } else {
                hi = mid - 1
            }
        }
        return found
    }

    // wake up right when the next line starts (timed), or every 100 ms (untimed)
    private fun nextDelay(): Long {
        if (!timed || lines.isEmpty()) return 100
        val next = lines.getOrNull(current + 1) ?: return 250
        return (next.t - position()).coerceIn(16, 250)
    }

    private fun update(animate: Boolean) {
        if (layouts.isEmpty() || height == 0) return
        if (!timed) {
            if (!manual) {
                val d = duration()
                val p = if (d > 0) position() / d.toFloat() else 0f
                val next = maxScroll() * p
                if (next == offset && shown != null) return
                offset = next
            }
            ensurePlainFrame()
            invalidate()
            return
        }
        val pos = position()
        val i = sungIndex(pos)
        if (i == current && shown != null) return
        val target = anchorFor(i)
        val jump = !animate || shown == null || Math.abs(target - offset) > height * 0.8f
        val ready = prepared?.takeIf { it.highlight == i && covers(it, target) }
        val frame = ready ?: render(i, target)
        if (ready == null) prepared?.let { recycle(it) }
        prepared = null
        current = i
        animator.cancel()
        fading?.let { recycle(it) }
        fading = null
        if (jump) {
            shown?.let { recycle(it) }
            shown = frame
            offset = target
            fade = 1f
            invalidate()
            post { prepareNext() }
        } else {
            fading = shown
            shown = frame
            slideFrom = offset
            slideTo = target
            fade = 0f
            animator.start()
        }
    }

    private fun maxScroll() = (total - height * 0.35f).coerceAtLeast(0f)

    // untimed: one window, drawn again when the scroll nears its edge
    private fun ensurePlainFrame() {
        val f = shown
        if (f != null && covers(f, offset)) return
        f?.let { recycle(it) }
        shown = render(-1, offset)
    }

    // the window holds what the screen shows with `at` at the anchor, plus a screen above and below
    private fun covers(f: Frame, at: Float): Boolean {
        val screenTop = at - anchor
        return screenTop >= f.top + height * 0.25f && screenTop + height <= f.top + window - height * 0.25f
    }

    private fun prepareNext() {
        if (!timed || layouts.isEmpty() || current + 1 > lines.lastIndex || animator.isRunning) return
        if (prepared?.highlight == current + 1) return
        prepared?.let { recycle(it) }
        prepared = render(current + 1, anchorFor(current + 1))
    }

    // the lines around `at`, with line `highlight` sung (timed), drawn once in software
    private fun render(highlight: Int, at: Float): Frame {
        val w = width
        val h = window
        val reused = pool.removeLastOrNull()
        val bitmap = if (reused != null && reused.width == w && reused.height == h) reused else Bitmap.createBitmap(w, h, Bitmap.Config.ALPHA_8)
        val canvas = Canvas(bitmap)
        canvas.drawColor(0, PorterDuff.Mode.CLEAR)
        val top = at - anchor - height
        layouts.indices.forEach { i ->
            val y = tops[i] - top
            if (y + slots[i] < 0 || y > h) return@forEach
            val sung = timed && i == highlight
            val l = if (sung) bigLayouts[i] else layouts[i]
            val alpha = when {
                !timed -> 225
                sung -> 255
                i < highlight -> (80 - (highlight - i - 1) * 20).coerceAtLeast(30) // sung: fade upwards
                else -> (125 - (i - highlight - 1) * 15).coerceAtLeast(60) // still to come
            }
            (if (sung) bigPaint else paint).color = Color.argb(alpha, 255, 255, 255)
            canvas.save()
            // centered in its slot, so the bigger sung line does not push its neighbours
            canvas.translate(side.toFloat(), y + (slots[i] - l.height) / 2f)
            l.draw(canvas)
            canvas.restore()
        }
        return Frame(bitmap, top, highlight)
    }

    // bitmaps are reused, never recycle()d: a display list may still draw one until the next frame
    private fun recycle(f: Frame) {
        if (pool.size < 3) pool += f.bitmap
    }

    private fun clearFrames() {
        animator.cancel()
        listOfNotNull(shown, fading, prepared).forEach { recycle(it) }
        shown = null
        fading = null
        prepared = null
    }

    // Edges fade over 28 dp at the top and the bottom. On 4.2 the GPU renderer applies a shader on
    // an ALPHA_8 bitmap in window coordinates, not the view's (the bottom fade sat 45 px too high,
    // hiding the last line), so the gradient is placed where this view is in the window.
    private var shaderAt = Int.MIN_VALUE
    private val where = IntArray(2)

    private fun edgeShader(canvas: Canvas) {
        getLocationInWindow(where)
        val y0 = if (canvas.isHardwareAccelerated) where[1] else 0
        if (y0 == shaderAt) return
        shaderAt = y0
        val h = height.toFloat()
        val edge = context.dpf(28f) / h.coerceAtLeast(1f)
        bitmapPaint.shader = LinearGradient(
            0f, y0.toFloat(), 0f, y0 + h,
            intArrayOf(Color.TRANSPARENT, Color.WHITE, Color.WHITE, Color.TRANSPARENT),
            floatArrayOf(0f, edge, 1f - edge, 1f), Shader.TileMode.CLAMP,
        )
    }

    override fun onDraw(canvas: Canvas) {
        val msg = message
        if (msg != null) {
            paint.color = Color.argb(200, 255, 255, 255)
            val l = layout(msg, width - side * 2)
            canvas.save()
            canvas.translate(side.toFloat(), (height - l.height) / 2f)
            l.draw(canvas)
            canvas.restore()
            return
        }
        edgeShader(canvas)
        fading?.let {
            bitmapPaint.alpha = ((1f - fade) * 255).toInt()
            canvas.drawBitmap(it.bitmap, 0f, anchor + it.top - offset, bitmapPaint)
        }
        shown?.let {
            bitmapPaint.alpha = if (fading != null) (fade * 255).toInt() else 255
            canvas.drawBitmap(it.bitmap, 0f, anchor + it.top - offset, bitmapPaint)
        }
    }

    // untimed lyrics only: the wheel reads through them (about two lines a detent, six when fast)
    val scrollable: Boolean get() = !timed && layouts.isNotEmpty()

    fun scrollBy(detents: Int) {
        if (!scrollable) return
        manual = true
        offset = (offset + detents * paint.fontSpacing * 2).coerceIn(0f, maxScroll())
        ensurePlainFrame()
        invalidate()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        post(poll)
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(poll)
        clearFrames()
        pool.clear()
        super.onDetachedFromWindow()
    }

    override fun onVisibilityChanged(changedView: View, visibility: Int) {
        super.onVisibilityChanged(changedView, visibility)
        removeCallbacks(poll)
        if (visibility == VISIBLE && isShown) post(poll)
    }
}
