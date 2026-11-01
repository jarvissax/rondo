package com.jarvissax.rondo

import android.content.Context

// The few settings the user can change on the device.
object Prefs {
    private fun prefs(c: Context) = c.getSharedPreferences("y1", Context.MODE_PRIVATE)

    // null = Auto
    fun videoQuality(c: Context): Int? = prefs(c).getInt("video_quality", 0).takeIf { it > 0 }

    fun setVideoQuality(c: Context, height: Int?) {
        prefs(c).edit().putInt("video_quality", height ?: 0).apply()
    }
}
