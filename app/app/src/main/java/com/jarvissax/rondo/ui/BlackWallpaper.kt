package com.jarvissax.rondo.ui

import android.app.WallpaperManager
import android.content.ComponentName
import android.content.Context
import android.graphics.Color
import android.service.wallpaper.WallpaperService
import android.util.Log
import android.view.SurfaceHolder

// Android 4.2 keeps the boot animation on screen until a wallpaper has drawn. Its default
// wallpaper lives in SystemUI, which the Rondo firmware doesn't have, so Rondo brings this one:
// plain black, and never seen behind Rondo's screens.
class BlackWallpaper : WallpaperService() {
    override fun onCreateEngine(): Engine = object : Engine() {
        override fun onSurfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = draw(holder)
        override fun onSurfaceRedrawNeeded(holder: SurfaceHolder) = draw(holder)

        private fun draw(holder: SurfaceHolder) {
            val canvas = holder.lockCanvas() ?: return
            canvas.drawColor(Color.BLACK)
            holder.unlockCanvasAndPost(canvas)
        }
    }

    companion object {
        // Makes this the system wallpaper once; Android remembers it for later boots. Setting a
        // wallpaper service needs SET_WALLPAPER_COMPONENT, which only built-in apps get.
        fun ensure(context: Context) {
            val wm = WallpaperManager.getInstance(context)
            val mine = ComponentName(context, BlackWallpaper::class.java)
            if (wm.wallpaperInfo?.component == mine) return
            runCatching {
                val service = WallpaperManager::class.java.getMethod("getIWallpaperManager").invoke(wm)
                service.javaClass.getMethod("setWallpaperComponent", ComponentName::class.java).invoke(service, mine)
                Log.i("Y1", "wallpaper: set to Rondo's")
            }.onFailure { Log.w("Y1", "wallpaper: couldn't set it: $it") }
        }
    }
}
