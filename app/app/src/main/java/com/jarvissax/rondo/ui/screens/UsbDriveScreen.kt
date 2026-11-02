package com.jarvissax.rondo.ui.screens

import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.os.Handler
import android.os.Looper
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

    // a computer is on the other end of the cable and can take the card
    fun connected(context: Context): Boolean = runCatching { call(context, "isUsbMassStorageConnected") as Boolean }.getOrDefault(false)

    fun enabled(context: Context): Boolean = runCatching { call(context, "isUsbMassStorageEnabled") as Boolean }.getOrDefault(false)

    // blocks while Android unmounts or mounts the card: call off the main thread
    fun setEnabled(context: Context, on: Boolean) {
        runCatching { call(context, if (on) "enableUsbMassStorage" else "disableUsbMassStorage") }
            .onFailure { Log.w(TAG, "usb drive: switching ${if (on) "on" else "off"} failed: $it") }
        if (!on) LocalLibrary.invalidate()
        Log.i(TAG, "usb drive: ${if (enabled(context)) "on" else "off"}")
    }

    // SystemUI opens its own "USB connected" screen when the cable goes in, which the wheel can't
    // use; Rondo shows this one instead (CHANGE_COMPONENT_ENABLED_STATE, also built-in apps only)
    fun silenceSystemScreen(context: Context) {
        val systemScreen = ComponentName("com.android.systemui", "com.android.systemui.usb.UsbStorageActivity")
        runCatching {
            val pm = context.packageManager
            if (pm.getComponentEnabledSetting(systemScreen) != PackageManager.COMPONENT_ENABLED_STATE_DISABLED) {
                pm.setComponentEnabledSetting(systemScreen, PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP)
            }
        }
    }
}

class UsbDriveScreen(shell: ShellActivity) : ShellListScreen(shell, "USB drive") {
    private val main = Handler(Looper.getMainLooper())
    private var switching = false

    override fun render() {
        val on = UsbDrive.enabled(context)
        list.setRows(
            when {
                switching -> listOf(Row.Entry(if (on) "Turning off…" else "Turning on…", tile = Tiles.device, enabled = false))
                on -> listOf(
                    Row.Entry("USB drive is on", subtitle = "Copy music on your computer", tile = Tiles.device, enabled = false),
                    Row.Entry("Turn off USB drive", subtitle = "Eject it on the computer first", onSelect = { switch(false) }),
                    Row.Header("While it's on"),
                    Row.Entry("Music on the card is paused", subtitle = "It comes back when the drive is off", enabled = false),
                )
                else -> listOf(
                    Row.Entry("Use as USB drive", subtitle = "Copy music from your computer", tile = Tiles.device, onSelect = { switch(true) }),
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
