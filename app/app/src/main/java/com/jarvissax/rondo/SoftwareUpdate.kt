package com.jarvissax.rondo

import android.content.Context
import android.content.pm.PackageManager
import android.util.Log
import java.io.File

// Updating Rondo without reflashing the firmware. A newer rondo-<version>.apk installs over the copy
// built into the firmware: Android keeps that copy and runs the update from /data/app, with the
// same built-in permissions. Android accepts only an APK signed with the same key as the firmware's
// copy, so nothing but Rondo's own releases can replace it. Installing needs INSTALL_PACKAGES,
// which Android 4.2 grants to built-in apps. Android stops Rondo while it swaps it, then starts it
// again as the home screen. (A change to the firmware itself, outside Rondo, still needs a flash.)
object SoftwareUpdate {
    private const val TAG = "Y1"

    data class Build(val version: String, val code: Int)

    fun available(context: Context): Boolean =
        context.checkCallingOrSelfPermission("android.permission.INSTALL_PACKAGES") == PackageManager.PERMISSION_GRANTED

    // where an update waits to be installed (readable by Android's installer)
    fun file(context: Context): File = File(context.filesDir, "update.apk")

    fun installed(context: Context): Build =
        context.packageManager.getPackageInfo(context.packageName, 0).let { Build(it.versionName ?: "", it.versionCode) }

    // what an APK holds, or null when it isn't a build of Rondo
    fun inspect(context: Context, apk: File): Build? =
        context.packageManager.getPackageArchiveInfo(apk.path, 0)
            ?.takeIf { it.packageName == context.packageName }
            ?.let { Build(it.versionName ?: "", it.versionCode) }

    // Hands the APK to Android's package manager (`pm install -r`, which runs as Rondo and so has its
    // permission). Blocks until Android answers; for a good update that answer usually never arrives
    // here, because Android stops this process first. Null when it installed, else why it didn't.
    fun install(context: Context, apk: File): String? {
        val build = inspect(context, apk) ?: return "That isn't an update for Rondo"
        if (build.code < installed(context).code) return "That's an older Rondo (${build.version})"
        apk.setReadable(true, false)
        apk.parentFile?.setExecutable(true, false)
        Log.i(TAG, "update: installing ${build.version} (${build.code})")
        return try {
            val pm = ProcessBuilder("pm", "install", "-r", apk.path).redirectErrorStream(true).start()
            val out = pm.inputStream.bufferedReader().readText().trim()
            pm.waitFor()
            Log.i(TAG, "update: pm said: $out")
            when {
                out.contains("Success") -> null
                out.contains("INCONSISTENT_CERTIFICATES") || out.contains("UPDATE_INCOMPATIBLE") -> "That APK isn't signed by Rondo's key"
                out.contains("INSUFFICIENT_STORAGE") -> "Not enough space on the Y1"
                else -> out.lineSequence().lastOrNull { it.isNotBlank() } ?: "The install didn't work"
            }
        } catch (e: Exception) {
            Log.w(TAG, "update: $e")
            "The install didn't start: ${e.message}"
        }
    }
}

// "1.0.10" is newer than "1.0.9" (server/src/updates.ts compares the same way)
fun isNewerVersion(latest: String?, current: String?): Boolean {
    if (latest.isNullOrEmpty() || current.isNullOrEmpty()) return false
    val a = latest.split('.', '-')
    val b = current.split('.', '-')
    for (i in 0 until maxOf(a.size, b.size)) {
        val x = a.getOrElse(i) { "0" }.toIntOrNull() ?: return latest != current
        val y = b.getOrElse(i) { "0" }.toIntOrNull() ?: return latest != current
        if (x != y) return x > y
    }
    return false
}
