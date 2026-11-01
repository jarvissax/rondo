package com.jarvissax.rondo

import androidx.multidex.MultiDexApplication
import com.jarvissax.rondo.net.Tls

// Legacy multidex is mandatory below API 21 (Kotlin stdlib + ExoPlayer + OkHttp exceed one dex).
class Y1App : MultiDexApplication() {
    override fun onCreate() {
        super.onCreate()
        Config.load(this)
        com.jarvissax.rondo.net.Accounts.load(this)
        com.jarvissax.rondo.player.FmRadio.init(this)
        Tls.install(this)
        com.jarvissax.rondo.ui.Fonts.init(this)
    }
}
