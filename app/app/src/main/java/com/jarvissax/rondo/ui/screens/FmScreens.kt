package com.jarvissax.rondo.ui.screens

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.jarvissax.rondo.Config
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.player.FmRadio
import com.jarvissax.rondo.ui.Fonts
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.dpf
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.text
import com.jarvissax.rondo.ui.visible

fun fmName(f: Int) = "${f / 10}.${f % 10}"

// The radio's dial: the band 87.5–108 MHz with a tick every 0.5 MHz, labels every 4 MHz, saved
// stations as dots and a needle at the station. Brighter while the wheel tunes.
class FmDial(context: Context) : View(context) {
    var frequency = FmRadio.MIN
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }
    var tuning = false
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }
    var saved: List<Int> = emptyList()
        set(value) {
            field = value
            invalidate()
        }
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        typeface = Fonts.regular
    }

    override fun onDraw(canvas: Canvas) {
        val pad = context.dpf(8f)
        val w = width - 2 * pad
        val x = { f: Int -> pad + w * (f - FmRadio.MIN) / (FmRadio.MAX - FmRadio.MIN).toFloat() }
        val base = height * 0.62f
        val alpha = if (tuning) 230 else 140
        label.textSize = context.dpf(9f)
        label.color = Color.argb(alpha, 255, 255, 255)
        paint.color = Color.argb(alpha, 255, 255, 255)
        paint.strokeWidth = context.dpf(1f)
        val japan = FmRadio.MIN < 875
        var f = (FmRadio.MIN + 4) / 5 * 5
        while (f <= FmRadio.MAX) {
            val major = if (japan) f % 20 == 0 else f % 40 == 0
            val half = f % 10 != 0
            val len = context.dpf(if (major) 9f else if (half) 3f else 5f)
            canvas.drawLine(x(f), base - len, x(f), base, paint)
            if (major) canvas.drawText((f / 10).toString(), x(f), base + context.dpf(11f), label)
            f += 5
        }
        paint.color = Color.argb(alpha, 255, 255, 255)
        canvas.drawLine(pad, base, pad + w, base, paint)
        paint.color = Color.argb(200, 255, 176, 77)
        saved.forEach { s -> canvas.drawCircle(x(s), base - context.dpf(13f), context.dpf(2f), paint) }
        // the needle
        paint.color = if (tuning) Color.rgb(255, 176, 77) else Palette.red
        val nx = x(frequency)
        canvas.drawRoundRect(RectF(nx - context.dpf(1.5f), base - context.dpf(18f), nx + context.dpf(1.5f), base + context.dpf(2f)), context.dpf(1.5f), context.dpf(1.5f), paint)
    }
}

// Four bars for the signal (0..4)
class SignalBars(context: Context) : View(context) {
    var level = 0
        set(value) {
            if (field != value) {
                field = value
                invalidate()
            }
        }
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

    override fun onDraw(canvas: Canvas) {
        val gap = width / 12f
        val bw = (width - 3 * gap) / 4
        for (i in 0 until 4) {
            paint.color = if (i < level) Color.argb(220, 255, 255, 255) else Color.argb(60, 255, 255, 255)
            val h = height * (i + 1) / 4f
            val l = i * (bw + gap)
            canvas.drawRoundRect(RectF(l, height - h, l + bw, height.toFloat()), gap / 2, gap / 2, paint)
        }
    }
}

// FM radio, laid out like Now Playing. The wheel is the volume, as on Now
// Playing; center switches it to tuning (0.1 MHz a step) and back; Next/Prev seek to the next
// station, held they fine-tune; Play/Pause mutes; hold center for the options; Menu leaves (the
// radio keeps playing). Opening the screen turns the radio on and pauses music.
class FmScreen(shell: ShellActivity) : Screen(shell) {
    private lateinit var freq: TextView
    private lateinit var unit: TextView
    private lateinit var name: TextView
    private lateinit var detail: TextView
    private lateinit var dial: FmDial
    private lateinit var bars: SignalBars
    private lateinit var info: TextView
    private lateinit var hint: TextView
    private var tuning = false

    private val changed: () -> Unit = { render() }

    override fun build(): View {
        val c = context
        val root = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(Color.rgb(58, 42, 18), Color.rgb(24, 20, 14), Palette.dark))
        }
        val header = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(12), 0, c.dp(10), 0)
        }
        header.addView(c.text(11, Color.argb(200, 255, 255, 255), bold = true).apply { text = "FM RADIO" }, lp(0, WRAP, 1f))
        header.addView(c.statusIcons(Color.argb(200, 255, 255, 255)), lp(WRAP, WRAP))
        root.addView(header, lp(MATCH, c.dp(24)))

        val center = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(c.dp(14), c.dp(6), c.dp(14), 0)
        }
        val row = LinearLayout(c).apply { gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL }
        freq = c.text(44, Palette.white, bold = true)
        unit = c.text(14, Color.argb(190, 255, 255, 255), bold = true).apply { text = "MHz" }
        row.addView(freq, lp(WRAP, WRAP))
        row.addView(unit, lp(WRAP, WRAP).apply {
            leftMargin = c.dp(6)
            bottomMargin = c.dp(9)
        })
        center.addView(row, lp(MATCH, WRAP))
        name = c.text(17, Palette.white, bold = true).apply { gravity = Gravity.CENTER }
        detail = c.text(12, Color.argb(190, 255, 255, 255), lines = 2).apply { gravity = Gravity.CENTER }
        center.addView(name, lp(MATCH, WRAP).apply { topMargin = c.dp(2) })
        center.addView(detail, lp(MATCH, WRAP).apply { topMargin = c.dp(3) })
        root.addView(center, lp(MATCH, 0, 1f))

        dial = FmDial(c)
        root.addView(dial, lp(MATCH, c.dp(40)))
        val bottom = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(14), 0, c.dp(14), c.dp(6))
        }
        bars = SignalBars(c)
        bottom.addView(bars, lp(c.dp(18), c.dp(12)).apply { rightMargin = c.dp(8) })
        info = c.text(11, Color.argb(200, 255, 255, 255))
        bottom.addView(info, lp(0, WRAP, 1f))
        hint = c.text(11, Color.argb(170, 255, 255, 255)).apply { gravity = Gravity.END }
        bottom.addView(hint, lp(WRAP, WRAP))
        root.addView(bottom, lp(MATCH, c.dp(26)))
        render()
        return root
    }

    fun render() {
        if (!::freq.isInitialized) return
        val s = FmRadio.state
        freq.text = fmName(s.frequency)
        dial.frequency = s.frequency
        dial.tuning = tuning
        dial.saved = Config.radio.presets.map { it.frequency }
        val saved = Config.radio.presets.firstOrNull { it.frequency == s.frequency }
        val (title, sub) = when {
            s.error == "headphones" -> "Plug in wired headphones" to "Their cable is the radio's antenna."
            s.error == "chip" -> "The radio didn't start" to "Press Play/Pause to try again."
            s.starting -> "Turning on…" to ""
            !s.on -> "Radio off" to "Press Play/Pause to turn it on."
            s.scanning -> "Scanning…" to "Finding every station. This takes a little while."
            s.seeking -> "Searching…" to ""
            else -> (s.name.ifEmpty { saved?.name?.ifEmpty { null } ?: Config.radio.names[s.frequency].orEmpty() }) to s.text
        }
        name.text = title
        name.visible(title.isNotEmpty())
        detail.text = sub
        detail.visible(sub.isNotEmpty())
        freq.alpha = if (s.on) 1f else 0.45f
        bars.level = if (s.on) s.signal else 0
        bars.visible(s.on)
        info.text = when {
            !s.on -> ""
            s.muted -> "Muted"
            else -> listOfNotNull(if (s.stereo) "Stereo" else "Mono", if (s.speaker) "Speaker" else "Headphones").joinToString(" · ")
        }
        hint.text = if (tuning) "turn to tune · center: done" else ""
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean {
        val s = FmRadio.state
        when (action) {
            WheelAction.Up, WheelAction.Down -> {
                val dir = if (action == WheelAction.Down) 1 else -1
                if (tuning && s.on) FmRadio.step(dir * steps) else shell.changeVolume(dir)
            }
            WheelAction.Select -> {
                tuning = !tuning && s.on
                render()
            }
            WheelAction.LongSelect -> {
                tuning = false
                shell.push(FmOptionsScreen(shell))
            }
            WheelAction.Next, WheelAction.Prev -> FmRadio.seek(action == WheelAction.Next)
            WheelAction.LongNext, WheelAction.LongPrev -> FmRadio.step(if (action == WheelAction.LongNext) 1 else -1)
            WheelAction.SeekEnd -> Unit
            WheelAction.PlayPause -> when {
                !s.on && !s.starting -> start()
                s.on -> FmRadio.setMuted(!s.muted)
            }
            WheelAction.Back -> {
                if (!tuning) return false
                tuning = false
                render()
            }
        }
        return true
    }

    private fun start() {
        if (player?.isPlaying() == true) player?.pause()
        FmRadio.start()
    }

    override fun onShow() {
        FmRadio.listen(changed)
        if (!FmRadio.state.on && !FmRadio.state.starting) start()
        render()
    }

    override fun onHide() {
        FmRadio.unlisten(changed)
        tuning = false
    }
}

// Hold center on the radio: stations, scan, save, output, off
class FmOptionsScreen(shell: ShellActivity) : ShellListScreen(shell, "FM Radio") {
    private val changed: () -> Unit = { render() }

    override fun render() {
        val s = FmRadio.state
        val saved = Config.radio.presets.any { it.frequency == s.frequency }
        list.setRows(
            listOf(
                Row.Entry("Stations", trailing = "${Config.radio.presets.size} saved", chevron = true, onSelect = { shell.push(FmStationsScreen(shell)) }),
                Row.Entry(
                    if (s.scanning) "Scanning…" else "Scan for stations",
                    subtitle = if (s.scanning) "" else "Finds every station you can receive",
                    enabled = s.on && !s.scanning,
                    onSelect = { FmRadio.scan { shell.push(FmStationsScreen(shell)) } },
                ),
                if (saved) {
                    Row.Entry("Remove ${fmName(s.frequency)} from saved", onSelect = {
                        Config.update(Config.Change.RADIO) { d -> d.copy(radio = d.radio.copy(presets = d.radio.presets.filter { it.frequency != s.frequency })) }
                        render()
                    })
                } else {
                    Row.Entry("Save ${fmName(s.frequency)}", subtitle = s.name, onSelect = {
                        Config.update(Config.Change.RADIO) { d ->
                            val presets = (d.radio.presets + Config.Preset(s.frequency, s.name)).sortedBy { it.frequency }
                            d.copy(radio = d.radio.copy(presets = presets))
                        }
                        render()
                    })
                },
                Row.Entry("Output", trailing = if (s.speaker) "Speaker" else "Headphones", onSelect = {
                    FmRadio.setSpeaker(!s.speaker)
                    render()
                }),
                Row.Entry("Region", trailing = if (Config.radio.band == "japan") "Japan" else "Most of the world", onSelect = {
                    FmRadio.setBand(if (Config.radio.band == "japan") "world" else "japan")
                    render()
                }),
                Row.Entry(if (s.on || s.starting) "Turn radio off" else "Turn radio on", onSelect = {
                    if (s.on || s.starting) {
                        FmRadio.stop()
                        shell.pop() // back to the (now quiet) radio screen
                    } else {
                        if (player?.isPlaying() == true) player?.pause()
                        FmRadio.start()
                    }
                    render()
                }),
            ),
            keepSelection = true,
        )
    }

    override fun onShow() {
        FmRadio.listen(changed)
        render()
    }

    override fun onHide() = FmRadio.unlisten(changed)
}

// Saved stations first, then what the last scan found; center tunes and goes back to the radio
class FmStationsScreen(shell: ShellActivity) : ShellListScreen(shell, "Stations") {
    override fun render() {
        val r = Config.radio
        val rows = mutableListOf<Row>()
        val pick = { f: Int ->
            FmRadio.tune(f)
            shell.popTo(FmScreen::class.java)
        }
        rows += Row.Header("Saved")
        if (r.presets.isEmpty()) rows += Row.Entry("No saved stations", subtitle = "Hold center on the radio, then Save", enabled = false)
        r.presets.forEach { p ->
            rows += Row.Entry("${fmName(p.frequency)} MHz", subtitle = p.name.ifEmpty { r.names[p.frequency].orEmpty() }, trailing = if (p.frequency == FmRadio.state.frequency) "Playing" else "", onSelect = { pick(p.frequency) })
        }
        val found = r.found.filter { f -> r.presets.none { it.frequency == f } }
        rows += Row.Header("Found")
        if (r.found.isEmpty()) rows += Row.Entry("Not scanned yet", subtitle = "Scan for stations in the radio's options", enabled = false)
        found.forEach { f ->
            rows += Row.Entry("${fmName(f)} MHz", subtitle = r.names[f].orEmpty(), trailing = if (f == FmRadio.state.frequency) "Playing" else "", onSelect = { pick(f) })
        }
        list.setRows(rows, keepSelection = true)
    }
}
