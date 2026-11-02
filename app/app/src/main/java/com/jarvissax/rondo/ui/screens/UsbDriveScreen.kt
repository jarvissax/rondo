package com.jarvissax.rondo.ui.screens

import android.content.Context
import android.content.pm.PackageManager
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.local.LocalLibrary
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.Tiles

// The microSD card as a USB drive for a computer: how music goes onto a Y1 running the Rondo
// firmware. Android 4.2 hands the whole card to the computer while it's shared, so the library
// and anything playing from the card pause until the drive is turned off or the cable comes out.
object UsbDrive {
    private const val TAG = "Y1"

    // StorageManager's sharing calls are hidden, and they need MOUNT_UNMOUNT_FILESYSTEMS, which
    // Android 4.2 grants only to apps built into the firmware (/system/app)
    private fun storage(context: Context): Any = context.getSystemService(Context.STORAGE_SERVICE)
    private fun call(context: Context, name: String): Any? =
        storage(context).javaClass.getMethod(name).invoke(storage(context))

    fun available(context: Context): Boolean =
        context.checkCallingOrSelfPermission("android.permission.MOUNT_UNMOUNT_FILESYSTEMS") == PackageManager.PERMISSION_GRANTED

    // Android's own isUsbMassStorageEnabled asks vold about the wrong volume on the Y1's MediaTek
    // build and always says no; the card's state, "shared" while a computer has it, is reliable
    fun enabled(): Boolean = Environment.getExternalStorageState() == Environment.MEDIA_SHARED

    // blocks while Android unmounts or mounts the card: call off the main thread. Android returns
    // before the card has changed hands, so this waits for it (a few seconds at most)
    fun setEnabled(context: Context, on: Boolean) {
        runCatching { call(context, if (on) "enableUsbMassStorage" else "disableUsbMassStorage") }
            .onFailure { Log.w(TAG, "usb drive: switching ${if (on) "on" else "off"} failed: $it") }
        val until = SystemClock.elapsedRealtime() + 15_000
        while (enabled() != on && SystemClock.elapsedRealtime() < until) SystemClock.sleep(250)
        if (!on) LocalLibrary.invalidate()
        Log.i(TAG, "usb drive: ${if (enabled()) "on" else "off"}")
    }
}

class UsbDriveScreen(shell: ShellActivity) : ShellListScreen(shell, "USB drive") {
    private val main = Handler(Looper.getMainLooper())
    private var switching = false

    override fun render() {
        val on = UsbDrive.enabled()
        list.setRows(
            when {
                switching -> listOf(Row.Entry(if (on) "Turning off…" else "Turning on…", tile = Tiles.device, enabled = false))
                on -> listOf(
                    Row.Entry("USB drive is on", subtitle = "Copy files on your computer", tile = Tiles.device, enabled = false),
                    Row.Entry("Turn off USB drive", subtitle = "Eject it on the computer first", onSelect = { switch(false) }),
                    Row.Header("While it's on"),
                    Row.Entry("Music on the card is paused", subtitle = "It comes back when the drive is off", enabled = false),
                )
                else -> listOf(
                    Row.Entry("Use as USB drive", subtitle = "Copy files from your computer", tile = Tiles.device, onSelect = { switch(true) }),
                    Row.Entry("Not now", subtitle = "Keep listening while it charges", onSelect = { shell.pop() }),
                )
            },
        )
    }

    private fun switch(on: Boolean) {
        if (switching) return
        if (on && shell.player?.isLocal == true) shell.player?.pause()
        switching = true
        render()
        Thread {
            UsbDrive.setEnabled(context, on)
            main.post {
                switching = false
                render()
            }
        }.start()
    }
}
