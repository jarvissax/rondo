package com.jarvissax.rondo.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.View

// The mockups' SVG icons, drawn in a 24-unit box. Android 4.2's fonts lack most symbol glyphs.
class IconView(context: Context, var kind: Kind, var color: Int) : View(context) {
    enum class Kind { CHEVRON, PLAY, PAUSE, BATTERY, WIFI, SEARCH, NOTE, SHUFFLE, REPEAT, VOLUME, CHECK, BLUETOOTH, GEAR, FOLDER, BARS, INFO, PERSON, DISC, SUN, TIMER, SCREEN, CLOUD, RADIO, POWER, THEME, PLAYLIST, PLUS, TRASH, REFRESH, SHORTS }

    var level = 0.8f // battery fill, 0..1
        set(value) {
            field = value
            invalidate()
        }

    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val path = Path()

    // Android 4.2's GPU drawing renders a path or circle at its own 24-unit size and then enlarges
    // the result, so a large glyph (the main menu pane's 70 dp one) came out blurry. Drawn in
    // software, the shapes are rendered at their final size.
    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        val large = minOf(w, h) > 24
        if (large != (layerType == LAYER_TYPE_SOFTWARE)) setLayerType(if (large) LAYER_TYPE_SOFTWARE else LAYER_TYPE_NONE, null)
    }

    override fun onDraw(canvas: Canvas) {
        val s = minOf(width, height) / 24f
        val ox = (width - 24 * s) / 2
        val oy = (height - 24 * s) / 2
        canvas.save()
        canvas.translate(ox, oy)
        canvas.scale(s, s)
        stroke.color = color
        stroke.strokeWidth = 2.2f
        fill.color = color
        path.reset()
        when (kind) {
            Kind.CHEVRON -> {
                path.moveTo(8f, 3f); path.lineTo(16f, 12f); path.lineTo(8f, 21f)
                stroke.strokeWidth = 3f
                canvas.drawPath(path, stroke)
            }
            Kind.PLAY -> {
                path.moveTo(5f, 3f); path.lineTo(5f, 21f); path.lineTo(21f, 12f); path.close()
                canvas.drawPath(path, fill)
            }
            Kind.PAUSE -> {
                canvas.drawRect(5f, 3f, 10f, 21f, fill)
                canvas.drawRect(14f, 3f, 19f, 21f, fill)
            }
            Kind.BATTERY -> {
                stroke.strokeWidth = 1.4f
                canvas.drawRoundRect(RectF(0.7f, 6.5f, 20.5f, 17.5f), 2f, 2f, stroke)
                canvas.drawRect(21.5f, 9.5f, 23.5f, 14.5f, fill)
                // the charge itself in green / amber / red by level; the dashboard matches
                fill.color = batteryColor(level)
                canvas.drawRect(2.3f, 8f, 2.3f + 16.6f * level.coerceIn(0f, 1f), 16f, fill)
            }
            Kind.WIFI -> {
                stroke.strokeWidth = 2.2f
                canvas.drawArc(RectF(1f, 5f, 23f, 27f), 225f, 90f, false, stroke)
                canvas.drawArc(RectF(5f, 9f, 19f, 23f), 225f, 90f, false, stroke)
                canvas.drawArc(RectF(9f, 13f, 15f, 19f), 225f, 90f, false, stroke)
                canvas.drawCircle(12f, 19.5f, 1.6f, fill)
            }
            Kind.SEARCH -> {
                stroke.strokeWidth = 2.6f
                canvas.drawCircle(10f, 10f, 6.5f, stroke)
                canvas.drawLine(15f, 15f, 21f, 21f, stroke)
            }
            Kind.NOTE -> {
                stroke.strokeWidth = 1.8f
                path.moveTo(9f, 18f); path.lineTo(9f, 5f); path.lineTo(21f, 3f); path.lineTo(21f, 16f)
                canvas.drawPath(path, stroke)
                canvas.drawCircle(6f, 18f, 3f, stroke)
                canvas.drawCircle(18f, 16f, 3f, stroke)
            }
            Kind.SHUFFLE -> {
                stroke.strokeWidth = 1.8f
                path.moveTo(2f, 6f); path.lineTo(6f, 6f); path.lineTo(16f, 18f); path.lineTo(21f, 18f)
                path.moveTo(2f, 18f); path.lineTo(6f, 18f); path.lineTo(16f, 6f); path.lineTo(21f, 6f)
                path.moveTo(18f, 3f); path.lineTo(21f, 6f); path.lineTo(18f, 9f)
                path.moveTo(18f, 15f); path.lineTo(21f, 18f); path.lineTo(18f, 21f)
                canvas.drawPath(path, stroke)
            }
            Kind.REPEAT -> {
                stroke.strokeWidth = 1.8f
                path.moveTo(3f, 11f); path.lineTo(3f, 8f); path.lineTo(18f, 8f)
                path.moveTo(15f, 5f); path.lineTo(18f, 8f); path.lineTo(15f, 11f)
                path.moveTo(21f, 13f); path.lineTo(21f, 16f); path.lineTo(6f, 16f)
                path.moveTo(9f, 13f); path.lineTo(6f, 16f); path.lineTo(9f, 19f)
                canvas.drawPath(path, stroke)
            }
            Kind.VOLUME -> {
                path.moveTo(3f, 9f); path.lineTo(7f, 9f); path.lineTo(12f, 4f); path.lineTo(12f, 20f); path.lineTo(7f, 15f); path.lineTo(3f, 15f); path.close()
                canvas.drawPath(path, fill)
                stroke.strokeWidth = 2f
                canvas.drawArc(RectF(10f, 7f, 20f, 17f), -55f, 110f, false, stroke)
            }
            Kind.CHECK -> {
                path.moveTo(2f, 12.5f); path.lineTo(9f, 19.5f); path.lineTo(22f, 5f)
                stroke.strokeWidth = 3f
                canvas.drawPath(path, stroke)
            }
            Kind.BLUETOOTH -> {
                stroke.strokeWidth = 2f
                path.moveTo(6f, 7f); path.lineTo(17f, 17f); path.lineTo(12f, 21f); path.lineTo(12f, 3f); path.lineTo(17f, 7f); path.lineTo(6f, 17f)
                canvas.drawPath(path, stroke)
            }
            Kind.FOLDER -> {
                path.moveTo(2f, 6f); path.lineTo(9f, 6f); path.lineTo(11f, 8.5f); path.lineTo(22f, 8.5f); path.lineTo(22f, 19f); path.lineTo(2f, 19f); path.close()
                canvas.drawPath(path, fill)
            }
            Kind.BARS -> {
                canvas.drawRect(3f, 10f, 7f, 21f, fill)
                canvas.drawRect(10f, 4f, 14f, 21f, fill)
                canvas.drawRect(17f, 13f, 21f, 21f, fill)
            }
            Kind.INFO -> {
                stroke.strokeWidth = 2.2f
                canvas.drawCircle(12f, 12f, 9.5f, stroke)
                canvas.drawCircle(12f, 7.5f, 1.5f, fill)
                canvas.drawRect(10.8f, 10.5f, 13.2f, 17.5f, fill)
            }
            Kind.PERSON -> {
                canvas.drawCircle(12f, 8f, 4.5f, fill)
                path.moveTo(3.5f, 21f); path.cubicTo(4f, 14f, 20f, 14f, 20.5f, 21f); path.close()
                canvas.drawPath(path, fill)
            }
            Kind.DISC -> {
                stroke.strokeWidth = 3f
                canvas.drawCircle(12f, 12f, 8.5f, stroke)
                canvas.drawCircle(12f, 12f, 2.2f, fill)
            }
            Kind.SUN -> {
                stroke.strokeWidth = 2.2f
                canvas.drawCircle(12f, 12f, 4.5f, fill)
                for (i in 0 until 8) {
                    val a = Math.toRadians(i * 45.0)
                    canvas.drawLine(
                        (12 + 7.5 * Math.cos(a)).toFloat(), (12 + 7.5 * Math.sin(a)).toFloat(),
                        (12 + 10.5 * Math.cos(a)).toFloat(), (12 + 10.5 * Math.sin(a)).toFloat(), stroke,
                    )
                }
            }
            Kind.TIMER -> {
                stroke.strokeWidth = 2.2f
                canvas.drawCircle(12f, 13f, 8.5f, stroke)
                canvas.drawLine(12f, 13f, 12f, 8f, stroke)
                canvas.drawLine(12f, 13f, 15.5f, 15f, stroke)
                canvas.drawLine(9.5f, 2.5f, 14.5f, 2.5f, stroke)
            }
            Kind.RADIO -> {
                // FM radio: an antenna over a box with a speaker and a tuning dial
                stroke.strokeWidth = 2f
                canvas.drawLine(6f, 8f, 17f, 2.5f, stroke)
                canvas.drawRoundRect(RectF(2.5f, 8f, 21.5f, 21f), 3f, 3f, stroke)
                canvas.drawCircle(8.5f, 14.5f, 3.2f, fill)
                canvas.drawRect(14f, 11.5f, 18.5f, 13.1f, fill)
                canvas.drawRect(14f, 15.9f, 18.5f, 17.5f, fill)
            }
            Kind.CLOUD -> {
                // the music server: a cloud of three puffs on a flat base
                canvas.drawCircle(7.5f, 14.5f, 4.5f, fill)
                canvas.drawCircle(12.5f, 10.5f, 6f, fill)
                canvas.drawCircle(17.5f, 14f, 4.5f, fill)
                canvas.drawRoundRect(RectF(7.5f, 12.5f, 17.5f, 19f), 1f, 1f, fill)
            }
            Kind.SCREEN -> {
                stroke.strokeWidth = 2.2f
                canvas.drawRoundRect(RectF(2.5f, 4f, 21.5f, 16.5f), 2f, 2f, stroke)
                canvas.drawLine(12f, 16.5f, 12f, 20f, stroke)
                canvas.drawLine(7.5f, 20.5f, 16.5f, 20.5f, stroke)
            }
            Kind.GEAR -> {
                // thick ring with square teeth (the thin-rayed version read as a sun)
                stroke.strokeWidth = 4f
                canvas.drawCircle(12f, 12f, 5.5f, stroke)
                stroke.strokeCap = Paint.Cap.BUTT
                stroke.strokeWidth = 4.2f
                for (i in 0 until 8) {
                    val a = Math.toRadians(i * 45.0 + 22.5)
                    canvas.drawLine(
                        (12 + 6.5 * Math.cos(a)).toFloat(), (12 + 6.5 * Math.sin(a)).toFloat(),
                        (12 + 10.5 * Math.cos(a)).toFloat(), (12 + 10.5 * Math.sin(a)).toFloat(), stroke,
                    )
                }
                stroke.strokeCap = Paint.Cap.ROUND
            }
            Kind.POWER -> {
                stroke.strokeWidth = 2.4f
                canvas.drawArc(RectF(4f, 4.5f, 20f, 20.5f), -55f, 290f, false, stroke)
                canvas.drawLine(12f, 2.5f, 12f, 11.5f, stroke)
            }
            Kind.THEME -> {
                // a circle half filled, as on the dashboard's Theme page
                stroke.strokeWidth = 2.2f
                canvas.drawCircle(12f, 12f, 8.5f, stroke)
                canvas.drawArc(RectF(3.5f, 3.5f, 20.5f, 20.5f), -90f, 180f, true, fill)
            }
            Kind.PLAYLIST -> {
                stroke.strokeWidth = 2.2f
                canvas.drawLine(3f, 5.5f, 15f, 5.5f, stroke)
                canvas.drawLine(3f, 10.5f, 15f, 10.5f, stroke)
                canvas.drawLine(3f, 15.5f, 10f, 15.5f, stroke)
                stroke.strokeWidth = 1.8f
                canvas.drawLine(20f, 7f, 20f, 18f, stroke)
                canvas.drawCircle(17f, 18f, 3f, fill)
            }
            Kind.PLUS -> {
                stroke.strokeWidth = 2.6f
                canvas.drawLine(12f, 4.5f, 12f, 19.5f, stroke)
                canvas.drawLine(4.5f, 12f, 19.5f, 12f, stroke)
            }
            Kind.TRASH -> {
                stroke.strokeWidth = 2f
                canvas.drawLine(4f, 7f, 20f, 7f, stroke)
                path.moveTo(9.5f, 7f); path.lineTo(9.5f, 4.5f); path.lineTo(14.5f, 4.5f); path.lineTo(14.5f, 7f)
                path.moveTo(6.5f, 7f); path.lineTo(7.4f, 20f); path.lineTo(16.6f, 20f); path.lineTo(17.5f, 7f)
                canvas.drawPath(path, stroke)
                canvas.drawLine(10.2f, 11f, 10.2f, 16.5f, stroke)
                canvas.drawLine(13.8f, 11f, 13.8f, 16.5f, stroke)
            }
            Kind.REFRESH -> {
                stroke.strokeWidth = 2.2f
                canvas.drawArc(RectF(4f, 4f, 20f, 20f), -45f, 315f, false, stroke)
                path.moveTo(20f, 3.8f); path.lineTo(20f, 8.5f); path.lineTo(15.3f, 8.5f)
                canvas.drawPath(path, stroke)
            }
            Kind.SHORTS -> {
                // a tall rounded screen with a play triangle: a short vertical video
                stroke.strokeWidth = 2.2f
                canvas.drawRoundRect(RectF(5.5f, 2f, 18.5f, 22f), 3.5f, 3.5f, stroke)
                path.moveTo(10f, 8f); path.lineTo(10f, 16f); path.lineTo(16f, 12f); path.close()
                canvas.drawPath(path, fill)
            }
        }
        canvas.restore()
    }
}

// battery charge color: green from 50 %, amber from 20 %, red below (dashboard: battColor in app.js)
fun batteryColor(level: Float): Int = android.graphics.Color.parseColor(
    when {
        level >= 0.5f -> "#34c759"
        level >= 0.2f -> "#ff9f0a"
        else -> "#ff3b30"
    },
)
