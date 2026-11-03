package com.jarvissax.rondo.ui.screens

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import com.jarvissax.rondo.BuildConfig
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.SoftwareUpdate
import com.jarvissax.rondo.isNewerVersion
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.net.UpdateInfo
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.Tiles

// Settings › Software update: a newer Rondo over Wi-Fi, without flashing the firmware again. The
// server says which release is newest and passes its APK through from GitHub (the Y1 talks only to
// its server); SoftwareUpdate installs it, and Rondo restarts by itself with everything kept. The
// dashboard and the update page (jarvissax.github.io/rondo) do the same without a server. A release
// that needs a newer firmware (SoftwareUpdate.firmware()) installs only with Innioasis Updater CE.
class SoftwareUpdateScreen(shell: ShellActivity) : ShellListScreen(shell, "Software update") {
    private sealed class State {
        object Checking : State()
        object NoServer : State()
        class Failed(val why: String) : State()
        object UpToDate : State()
        class Available(val info: UpdateInfo) : State()
        class NeedsFirmware(val info: UpdateInfo) : State()
        class Downloading(val percent: Int) : State()
        object Installing : State()
    }

    private var state: State = State.Checking

    override fun render() {
        val now = "Rondo ${BuildConfig.VERSION_NAME}"
        val rows: List<Row> = when (val s = state) {
            State.Checking -> listOf(Row.Entry("Checking for updates…", subtitle = now, tile = Tiles.update, enabled = false))
            State.NoServer -> listOf(
                Row.Entry(now, subtitle = "This Rondo has no server to check with", tile = Tiles.update),
                Row.Header("Other ways to update"),
                Row.Entry("From the dashboard", subtitle = "Settings › Dashboard, then the Overview page"),
                Row.Entry("Over USB", subtitle = "jarvissax.github.io/rondo in Chrome or Edge"),
            )
            is State.Failed -> listOf(
                Row.Entry("Couldn't check", subtitle = s.why, tile = Tiles.update),
                Row.Entry("Try again", onSelect = { check() }),
            )
            State.UpToDate -> listOf(
                Row.Entry(now, subtitle = "This is the newest Rondo", trailing = "Up to date", tile = Tiles.update),
                Row.Entry("Check again", onSelect = { check() }),
            )
            is State.NeedsFirmware -> listOf(
                Row.Entry("Rondo ${s.info.version} is out", subtitle = "It needs a new firmware · you have ${BuildConfig.VERSION_NAME}", tile = Tiles.update),
                Row.Header("Install it with Innioasis Updater CE"),
                Row.Entry("On a computer", subtitle = "Choose Y1, Type A, then Rondo"),
                Row.Entry("Your settings", subtitle = "Choose Keep Data, if the Updater offers it, to keep them"),
            )
            is State.Available -> listOf(
                Row.Entry("Rondo ${s.info.version} is out", subtitle = "You have ${BuildConfig.VERSION_NAME} · ${bytes(s.info.size)}", tile = Tiles.update),
                Row.Entry("Install now", subtitle = "Rondo restarts by itself; settings, Wi-Fi and music stay", onSelect = { install(s.info) }),
            )
            is State.Downloading -> listOf(Row.Entry("Downloading… ${s.percent}%", subtitle = "Keep the Y1 on Wi-Fi", tile = Tiles.update, enabled = false))
            State.Installing -> listOf(Row.Entry("Installing…", subtitle = "Rondo restarts by itself in a few seconds", tile = Tiles.update, enabled = false))
        }
        // a Rondo installed past these checks (over adb) on a firmware older than it needs
        val stale = BuildConfig.FIRMWARE > SoftwareUpdate.firmware()
        list.setRows(if (stale) listOf(Row.Entry("This Rondo needs a newer firmware", subtitle = "Install Rondo with Innioasis Updater CE", tile = Tiles.update)) + rows else rows)
    }

    override fun onShow() {
        if (state == State.Checking) check()
    }

    private fun check() {
        if (!Api.configured) {
            state = State.NoServer
            return render()
        }
        state = State.Checking
        render()
        launch {
            state = try {
                val info = Api.updateInfo()
                when {
                    info.version == null || !isNewerVersion(info.version, BuildConfig.VERSION_NAME) -> State.UpToDate
                    info.firmware > SoftwareUpdate.firmware() -> State.NeedsFirmware(info)
                    else -> State.Available(info)
                }
            } catch (e: Exception) {
                State.Failed(if (com.jarvissax.rondo.ui.isOnline(context)) "The server didn't answer" else "No Wi-Fi connection")
            }
            render()
        }
    }

    private fun install(info: UpdateInfo) {
        if (!SoftwareUpdate.available(context)) {
            state = State.Failed("Updates need the Rondo firmware")
            return render()
        }
        state = State.Downloading(0)
        render()
        launch {
            val apk = SoftwareUpdate.file(context)
            try {
                Api.downloadUpdate(apk, info.size) { f ->
                    val percent = (f * 100).toInt()
                    shell.runOnUiThread {
                        if ((state as? State.Downloading)?.percent != percent) {
                            state = State.Downloading(percent)
                            render()
                        }
                    }
                }
            } catch (e: Exception) {
                state = State.Failed("The download didn't finish: ${e.message}")
                return@launch render()
            }
            state = State.Installing
            render()
            player?.let { if (it.isPlaying()) it.togglePlay() }
            // only comes back when Android refused it: a good update stops this process first
            val problem = withContext(Dispatchers.IO) { SoftwareUpdate.install(context, apk) } ?: return@launch
            state = State.Failed(problem)
            render()
        }
    }
}
