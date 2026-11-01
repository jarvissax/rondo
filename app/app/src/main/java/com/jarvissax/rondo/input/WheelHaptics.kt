package com.jarvissax.rondo.input

import android.content.Context
import android.os.Vibrator

// A short buzz per wheel step, as Rockbox's Y1 port gives (its "wheel vibration intensity",
// firmware/target/hosted/android/button-android.c): a base length from the setting plus a boost that
// fades as a spin goes on (20 ms for the first steps, then 8, 2, 0), so a slow turn feels like
// clicks and a fast spin like a soft purr. Rockbox's intensity 5 (25 ms) is "normal" here.
class WheelHaptics(context: Context) {
    private val vibrator = (context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator)?.takeIf { it.hasVibrator() }

    var level = "normal" // off | light | normal | strong (Config.behavior.wheelVibration)

    fun tick(spin: Int) {
        val base = baseMs(level) ?: return
        val boost = when {
            spin < 3 -> 20
            spin < 7 -> 8
            spin < 15 -> 2
            else -> 0
        }
        runCatching { vibrator?.vibrate((base + boost).toLong()) }
    }

    companion object {
        val LEVELS = listOf("off", "light", "normal", "strong")

        fun baseMs(level: String): Int? = when (level) {
            "light" -> 15
            "normal" -> 25
            "strong" -> 40
            else -> null
        }
    }
}
