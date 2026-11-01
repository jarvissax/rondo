package com.jarvissax.rondo.ui

import android.os.SystemClock
import com.jarvissax.rondo.player.PlayerService

// Seeking that scales with the length (a 2-hour video can't be crossed 5 s at a
// time). The target is previewed (bubble + marker on the bar) and playback jumps once:
// on center, when Next/Prev is let go, or 1.2 s after the last wheel turn. Menu cancels.
//   wheel while scrubbing: 1/150 of the length per detent (1 s .. 60 s), fast turning x4
//   Next/Prev while scrubbing: 10 % of the length
//   holding Next/Prev: 1 % every 0.3 s, 2 % after 1.5 s, 4 % after 3 s
// A 4-minute song moves 1.6 s per detent; a 2-hour video 48 s, and holding Next crosses it in ~10 s.
class Scrubber(private val player: () -> PlayerService?) {
    var active = false
        private set
    var target = 0L
        private set
    var from = 0L
        private set
    private var lastInput = 0L
    private var holding = false
    private var repeats = 0
    var moved = false // the wheel or a button moved the target since begin()
        private set

    val duration: Long get() = player()?.durationMs() ?: 0L
    val fraction: Float? get() = if (active && duration > 0) target / duration.toFloat() else null

    fun begin(): Boolean {
        val p = player() ?: return false
        if (p.durationMs() <= 0) return false
        active = true
        holding = false
        moved = false
        from = p.positionMs()
        target = from
        lastInput = SystemClock.uptimeMillis()
        return true
    }

    fun turn(dir: Int, steps: Int) {
        if (!active && !begin()) return
        val step = (duration / 150).coerceIn(1_000, 60_000) * (if (steps > 1) 4 else 1)
        move(dir * step)
    }

    fun jump(dir: Int) {
        if (!active && !begin()) return
        move(dir * maxOf(duration / 10, 5_000))
    }

    // one repeat of a held Next/Prev
    fun hold(dir: Int) {
        if (!holding) {
            if (!active && !begin()) return
            holding = true
            repeats = 0
        }
        repeats++
        val share = when {
            repeats <= 5 -> 0.01
            repeats <= 10 -> 0.02
            else -> 0.04
        }
        move(dir * maxOf((duration * share).toLong(), 2_000))
    }

    private fun move(delta: Long) {
        moved = true
        target = (target + delta).coerceIn(0, maxOf(0, duration - 1_000))
        lastInput = SystemClock.uptimeMillis()
    }

    fun commit() {
        if (!active) return
        if (moved) player()?.seekTo(target)
        active = false
        holding = false
    }

    fun cancel() {
        active = false
        holding = false
    }

    // call from onTick: jumps after the wheel has rested (not while a button is held); an unused
    // scrub mode closes after 4 s
    fun tick() {
        if (active && !holding && SystemClock.uptimeMillis() - lastInput > (if (moved) 1_200 else 4_000)) commit()
    }
}
