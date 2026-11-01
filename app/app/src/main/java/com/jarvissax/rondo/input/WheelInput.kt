package com.jarvissax.rondo.input

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.KeyEvent

// Every screen sees only these; the Y1's raw keys are translated in one place.
// LongNext/LongPrev repeat while Next/Prev is held; SeekEnd follows when it is let go
enum class WheelAction { Up, Down, Select, LongSelect, Back, PlayPause, Next, Prev, LongNext, LongPrev, SeekEnd }

// Raw mapping measured on the device (docs/device/README.md): wheel clockwise = DPAD_RIGHT,
// counter-clockwise = DPAD_LEFT, two instant pulses per felt detent; center = ENTER;
// Menu = BACK; Play = MEDIA_PLAY_PAUSE; Next/Prev = MEDIA_NEXT/PREVIOUS (real press/release).
class WheelInputDispatcher(private val emit: (action: WheelAction, steps: Int) -> Unit) {
    private val handler = Handler(Looper.getMainLooper())

    // wheel: every pulse is a step, two per felt click, as Rockbox's Y1 port does (one step per
    // click felt slow). Pulses less than SPIN_GAP_MS apart in one direction are a
    // spin; `spin` counts its pulses (1 for a lone pulse).
    private var lastPulseAt = 0L
    private var lastDir = 0
    private var spin = 0

    var acceleration = "normal" // off | normal | fast (Config.behavior)

    // after each pulse, with the spin count: the vibration tick (WheelHaptics)
    var onPulse: (spin: Int) -> Unit = {}

    // held buttons
    private var longFired = false
    private var heldKey = 0

    private val longSelect = Runnable {
        longFired = true
        emit(WheelAction.LongSelect, 1)
    }

    private val seekRepeat = object : Runnable {
        override fun run() {
            longFired = true
            emit(if (heldKey == KeyEvent.KEYCODE_MEDIA_NEXT) WheelAction.LongNext else WheelAction.LongPrev, 1)
            handler.postDelayed(this, SEEK_REPEAT_MS)
        }
    }

    // true when the event was one of the Y1's controls (consumed)
    fun onKeyEvent(e: KeyEvent): Boolean {
        val down = e.action == KeyEvent.ACTION_DOWN
        val up = e.action == KeyEvent.ACTION_UP
        when (e.keyCode) {
            KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_DOWN -> if (down) pulse(+1)
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_UP -> if (down) pulse(-1)
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_DPAD_CENTER -> {
                if (down && e.repeatCount == 0) {
                    longFired = false
                    handler.postDelayed(longSelect, LONG_PRESS_MS)
                } else if (up) {
                    handler.removeCallbacks(longSelect)
                    if (!longFired) emit(WheelAction.Select, 1)
                }
            }
            KeyEvent.KEYCODE_BACK -> if (up) emit(WheelAction.Back, 1)
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, KeyEvent.KEYCODE_MEDIA_PLAY, KeyEvent.KEYCODE_MEDIA_PAUSE ->
                if (down && e.repeatCount == 0) emit(WheelAction.PlayPause, 1)
            KeyEvent.KEYCODE_MEDIA_NEXT, KeyEvent.KEYCODE_MEDIA_PREVIOUS -> {
                if (down && e.repeatCount == 0) {
                    longFired = false
                    heldKey = e.keyCode
                    handler.postDelayed(seekRepeat, LONG_PRESS_MS)
                } else if (up) {
                    handler.removeCallbacks(seekRepeat)
                    if (longFired) emit(WheelAction.SeekEnd, 1)
                    else emit(if (e.keyCode == KeyEvent.KEYCODE_MEDIA_NEXT) WheelAction.Next else WheelAction.Prev, 1)
                }
            }
            else -> return false
        }
        return true
    }

    private fun pulse(dir: Int) {
        val now = SystemClock.uptimeMillis()
        spin = if (dir == lastDir && now - lastPulseAt < SPIN_GAP_MS) spin + 1 else 1
        lastPulseAt = now
        lastDir = dir
        onPulse(spin)
        // acceleration (a setting): a long fast spin moves lists and the scrubber several steps a pulse
        // (volume and the keyboard always take one)
        val steps = when (acceleration) {
            "off" -> 1
            "fast" -> if (spin >= 6) 3 else 1
            else -> if (spin >= 12) 2 else 1
        }
        emit(if (dir > 0) WheelAction.Down else WheelAction.Up, steps)
    }

    fun cancel() {
        handler.removeCallbacks(longSelect)
        handler.removeCallbacks(seekRepeat)
    }

    companion object {
        const val SPIN_GAP_MS = 154L // Rockbox's HZ/6.5
        private const val LONG_PRESS_MS = 600L
        private const val SEEK_REPEAT_MS = 300L
    }
}
