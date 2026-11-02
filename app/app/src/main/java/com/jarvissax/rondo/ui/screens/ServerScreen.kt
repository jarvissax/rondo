package com.jarvissax.rondo.ui.screens

import android.os.SystemClock
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import com.jarvissax.rondo.Config
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.net.HttpException
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.Tiles

// Settings › Server: the dashboard's Server page on the device. Which Rondo server this Y1 uses,
// a connection test, and connecting to one with its address and the code from its admin page
// (docs/dev/server-api.md), or with a token for servers started with API_TOKEN.
class ServerScreen(shell: ShellActivity) : ShellListScreen(shell, "Server") {
    private var test: List<Row>? = null // the last connection test's answer

    override fun render() {
        val rows = mutableListOf<Row>(
            Row.Entry("Address", trailing = Api.base.removePrefix("https://").removePrefix("http://").ifEmpty { "Not set up" }, tile = Tiles.server),
            Row.Entry("Connection test", subtitle = "Calls the server and reports back", onSelect = { runTest() }),
        )
        test?.let { rows += Row.Header("Test") ; rows += it }
        rows += Row.Header("Connect")
        rows += Row.Entry("Connect to a server", subtitle = "Its address and the code from its admin page", chevron = true, onSelect = { askAddress() })
        rows += Row.Entry("Use a token instead", subtitle = "For servers started with API_TOKEN", chevron = true, onSelect = { askToken() })
        list.setRows(rows, keepSelection = true)
    }

    override fun onShow() = render()

    private fun runTest() {
        test = listOf(Row.Entry("Calling the server…", enabled = false))
        render()
        launch {
            test = if (!Api.configured) listOf(Row.Entry("No server yet", subtitle = "Connect to one below")) else try {
                val started = SystemClock.uptimeMillis()
                val h = Api.health()
                val ms = SystemClock.uptimeMillis() - started
                Api.time() // needs the token
                listOf(
                    Row.Entry("Answered in", trailing = "$ms ms"),
                    Row.Entry("yt-dlp", trailing = h.ytdlp ?: "missing"),
                    Row.Entry("Playback", trailing = when (h.lastResolveOk) { true -> "working"; false -> "failing"; null -> "not tried yet" }),
                    Row.Entry("YouTube account", trailing = if (h.account.music || h.account.videos) "signed in" else "signed out"),
                )
            } catch (e: HttpException) {
                listOf(Row.Entry(if (e.code == 401 || e.code == 403) "The server refused this Rondo" else "The server answered HTTP ${e.code}",
                    subtitle = "Connect again with a new code"))
            } catch (e: Exception) {
                listOf(Row.Entry("The server didn't answer", subtitle = "Check the Wi-Fi and the address"))
            }
            render()
        }
    }

    // two steps: the address, then the connect code (or keep the address and only send a new code)
    private fun askAddress() {
        shell.push(TextEntryScreen(
            shell, "Server address", initial = Api.base.ifEmpty { "https://" }, action = "Next",
            note = "Like https://music.example.com or http://192.168.1.20:3000", maxLength = 120,
            onDone = { url -> if (url.startsWith("https://") || url.startsWith("http://")) null else "Start with https:// or http://" },
            then = { url -> askCode(url.trimEnd('/')) },
        ))
    }

    private fun askCode(url: String) {
        shell.push(TextEntryScreen(
            shell, "Connect code", action = "Connect", note = "On the server's admin page: Device › Connect a Rondo", maxLength = 6,
            onDone = { typed ->
                val code = typed.filter { it.isDigit() }
                if (code.length != 6) return@TextEntryScreen "The code has 6 digits"
                val token = try {
                    withContext(Dispatchers.IO) { Api.pair(url, code) }
                } catch (e: HttpException) {
                    return@TextEntryScreen "The server answered HTTP ${e.code}"
                } catch (e: Exception) {
                    return@TextEntryScreen "The server didn't answer"
                } ?: return@TextEntryScreen "Wrong or expired code"
                Config.update(Config.Change.SERVER) { d -> d.copy(server = d.server.copy(url = url, token = token)) }
                Api.clearCache()
                Log.i("Y1", "server: connected with a connect code")
                test = listOf(Row.Entry("Connected", subtitle = "Run the test to check it"))
                null
            },
        ))
    }

    private fun askToken() {
        shell.push(TextEntryScreen(
            shell, "API token", action = "Save", note = "The API_TOKEN the server was started with", maxLength = 128,
            onDone = { token ->
                if (token.length < 8) return@TextEntryScreen "That's too short for a token"
                Config.update(Config.Change.SERVER) { d -> d.copy(server = d.server.copy(token = token)) }
                Api.clearCache()
                test = listOf(Row.Entry("Token saved", subtitle = "Run the test to check it"))
                null
            },
        ))
    }
}
