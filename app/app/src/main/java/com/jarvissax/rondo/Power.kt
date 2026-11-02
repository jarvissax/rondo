package com.jarvissax.rondo

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.util.Log

// Shutting down and restarting the Y1. Android's ShutdownActivity does both; it lets in only apps
// with SHUTDOWN, which Android 4.2 grants to apps built into the firmware (/system/app). An
// installed copy of Rondo on the stock firmware can't, and says so.
object Power {
    fun available(context: Context): Boolean =
        context.checkCallingOrSelfPermission("android.permission.SHUTDOWN") == PackageManager.PERMISSION_GRANTED

    fun shutDown(context: Context) = request(context, "android.intent.action.ACTION_REQUEST_SHUTDOWN")

    fun restart(context: Context) = request(context, Intent.ACTION_REBOOT)

    private fun request(context: Context, action: String) {
        Log.i("Y1", "power: $action")
        runCatching {
            context.startActivity(
                Intent(action)
                    .putExtra("android.intent.extra.KEY_CONFIRM", false) // Rondo asked already
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            )
        }.onFailure { Log.w("Y1", "power: refused: $it") }
    }
}
