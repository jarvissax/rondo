package com.jarvissax.rondo.net

import android.content.Context
import android.content.SharedPreferences

// Which account feeds the server has (GET /healthz "account", docs/dev/server-api.md). Without an account
// YouTube Music has no Library tab and YouTube no Subscriptions or History: both run signed out
// (search, playback, artists, albums, a signed-out home). Remembered across restarts so the tabs
// don't jump before the first check; servers older than this send nothing and keep every tab.
object Accounts {
    @Volatile var music = true
        private set
    @Volatile var videos = true
        private set
    private var prefs: SharedPreferences? = null

    fun load(context: Context) {
        val p = context.getSharedPreferences("y1", Context.MODE_PRIVATE)
        prefs = p
        music = p.getBoolean("account_music", true)
        videos = p.getBoolean("account_videos", true)
    }

    fun update(music: Boolean, videos: Boolean) {
        if (music == this.music && videos == this.videos) return
        this.music = music
        this.videos = videos
        prefs?.edit()?.putBoolean("account_music", music)?.putBoolean("account_videos", videos)?.apply()
    }
}
