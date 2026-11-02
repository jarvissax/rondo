package com.jarvissax.rondo.dashboard

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.AudioManager
import android.media.MediaMetadataRetriever
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Handler
import android.os.Looper
import android.os.StatFs
import android.os.SystemClock
import android.provider.Settings
import android.util.Log
import android.util.LruCache
import fi.iki.elonen.NanoHTTPD
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.net.URLEncoder
import java.security.SecureRandom
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.floatOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put
import okhttp3.Request
import com.jarvissax.rondo.BuildConfig
import com.jarvissax.rondo.Config
import com.jarvissax.rondo.Power
import com.jarvissax.rondo.Prefs
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.SoftwareUpdate
import com.jarvissax.rondo.input.WheelHaptics
import com.jarvissax.rondo.local.LocalLibrary
import com.jarvissax.rondo.local.Playlists
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.net.HttpException
import com.jarvissax.rondo.player.FmRadio
import com.jarvissax.rondo.player.QualityPreference
import com.jarvissax.rondo.ui.screens.Bt
import com.jarvissax.rondo.ui.screens.BtSession
import com.jarvissax.rondo.ui.screens.Wifi

// The dashboard's server (docs/dev/dashboard-api.md): the web page from the APK's assets and its JSON API,
// on port 8080, only while Settings › Dashboard is open. One thread per request (NanoHTTPD); what
// touches the player or the views runs on the main thread.
class DashboardServer(private val shell: ShellActivity) : NanoHTTPD(PORT) {
    private val context: Context = shell.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private val random = SecureRandom()
    private val sessions = HashSet<String>()
    private val io = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val art = object : LruCache<String, ByteArray>(2 * 1024 * 1024) {
        override fun sizeOf(key: String, value: ByteArray) = value.size
    }
    private var wrongCodes = 0
    private var pairBlockedUntil = 0L
    @Volatile private var scanning = false

    @Volatile var code: String = newCode()
        private set
    @Volatile var lastRequestAt = SystemClock.uptimeMillis()
        private set
    @Volatile var browsers = 0 // sessions that made a request in the last 30 s
        private set
    private val seen = HashMap<String, Long>()

    // a join started from the page, and whether the network refused its password (Android reports
    // an authentication error a few seconds in); GET /api/wifi passes both on
    @Volatile private var joining: String? = null
    @Volatile private var joinError: String? = null
    private val wifiEvents = object : android.content.BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val target = joining ?: return
            when (i.action) {
                WifiManager.SUPPLICANT_STATE_CHANGED_ACTION ->
                    if (i.getIntExtra(WifiManager.EXTRA_SUPPLICANT_ERROR, 0) == WifiManager.ERROR_AUTHENTICATING) joinError = "auth"
                WifiManager.NETWORK_STATE_CHANGED_ACTION ->
                    if (Wifi.unquote(wm().connectionInfo?.ssid) == target && wm().connectionInfo?.networkId != -1 &&
                        (i.getParcelableExtra<android.net.NetworkInfo>(WifiManager.EXTRA_NETWORK_INFO))?.isConnected == true
                    ) {
                        joining = null
                        joinError = null
                    }
            }
        }
    }

    init {
        context.registerReceiver(wifiEvents, IntentFilter(WifiManager.SUPPLICANT_STATE_CHANGED_ACTION).apply {
            addAction(WifiManager.NETWORK_STATE_CHANGED_ACTION)
        })
    }

    private fun newCode() = String.format("%06d", random.nextInt(1_000_000))

    // only signed-in requests count as activity: someone guessing codes doesn't keep the dashboard on
    override fun serve(session: IHTTPSession): Response {
        val uri = session.uri
        return try {
            when {
                uri.startsWith("/api/") -> api(session, uri.removePrefix("/api"))
                uri.startsWith("/fonts/") -> asset("fonts/" + uri.removePrefix("/fonts/"))
                uri == "/" || uri.isEmpty() || uri == "/index.html" -> index()
                else -> asset("dashboard" + uri)
            }
        } catch (e: Exception) {
            Log.w("Y1", "dashboard: $uri failed: $e")
            error(Response.Status.INTERNAL_ERROR, e.message ?: "error")
        }
    }

    // --- static files ---

    // the page, with its script and styles stamped with this install's time: a browser that cached
    // an older app.js fetches the new one after an update
    private val stamp: String by lazy {
        runCatching { context.packageManager.getPackageInfo(context.packageName, 0).lastUpdateTime.toString(36) }.getOrDefault(BuildConfig.VERSION_NAME)
    }

    private fun index(): Response {
        val html = runCatching { context.assets.open("dashboard/index.html").use { it.readBytes().toString(Charsets.UTF_8) } }.getOrNull()
            ?: return error(Response.Status.NOT_FOUND, "not found")
        val stamped = html.replace("href=\"app.css\"", "href=\"app.css?v=$stamp\"").replace("src=\"app.js\"", "src=\"app.js?v=$stamp\"")
        return newFixedLengthResponse(Response.Status.OK, "text/html; charset=utf-8", stamped).apply { addHeader("Cache-Control", "no-cache") }
    }

    private fun asset(path: String): Response {
        if (path.contains("..")) return error(Response.Status.NOT_FOUND, "not found")
        val stream: InputStream = runCatching { context.assets.open(path) }.getOrNull()
            ?: return error(Response.Status.NOT_FOUND, "not found")
        val type = when (path.substringAfterLast('.').lowercase()) {
            "html" -> "text/html; charset=utf-8"
            "css" -> "text/css; charset=utf-8"
            "js", "mjs" -> "application/javascript; charset=utf-8"
            "svg" -> "image/svg+xml"
            "ttf" -> "font/ttf"
            "png" -> "image/png"
            "json" -> "application/json"
            else -> "application/octet-stream"
        }
        return newChunkedResponse(Response.Status.OK, type, stream).apply {
            addHeader("Cache-Control", if (path.endsWith(".html")) "no-cache" else "max-age=3600")
        }
    }

    // --- API ---

    private fun api(s: IHTTPSession, path: String): Response {
        val method = s.method
        if (path == "/pair" && method == Method.POST) return pair(s)
        val token = s.headers["x-rondo-session"] ?: cookie(s.headers["cookie"], "rondo_session")
        if (token == null || synchronized(sessions) { token !in sessions }) return error(Response.Status.UNAUTHORIZED, "pair").apply { closeConnection(true) }
        markSeen(token)
        lastRequestAt = SystemClock.uptimeMillis()
        // read every body now (except uploads): an unread body would be taken for the next request
        // on a kept-alive connection
        val b = if ((method == Method.POST || method == Method.PUT) && path != "/library/upload" && path != "/update") body(s) else JsonObject(emptyMap())
        return when {
            path == "/status" && method == Method.GET -> json(status())
            path == "/player" && method == Method.POST -> json(playerAction(b))
            path == "/player/play" && method == Method.POST -> json(playPath(b))
            path == "/thumb" && method == Method.GET -> thumb(s.parms["path"])
            path == "/art" && method == Method.GET -> art(s.parms["path"], s.parms["size"]?.toIntOrNull() ?: 96)
            path == "/library" && method == Method.GET -> json(library())
            path == "/library/upload" && method == Method.PUT -> upload(s).apply { if (status != Response.Status.OK) closeConnection(true) } // body may be unread
            path == "/library" && method == Method.DELETE -> delete(s.parms["path"])
            path == "/library/rescan" && method == Method.POST -> {
                rescan()
                ok()
            }
            path == "/settings" && method == Method.GET -> json(settings())
            path == "/settings" && method == Method.PUT -> json(putSettings(b))
            path == "/theme" && method == Method.GET -> json(theme())
            path == "/theme" && method == Method.PUT -> json(putTheme(b))
            path == "/wifi" && method == Method.GET -> json(wifi())
            path == "/wifi/join" && method == Method.POST -> joinWifi(b)
            path == "/wifi/saved" && method == Method.DELETE -> forgetWifi(s.parms["id"]?.toIntOrNull())
            path == "/radio" && method == Method.GET -> json(radio())
            path == "/radio" && method == Method.PUT -> json(putRadio(b))
            path == "/radio/seek" && method == Method.POST -> {
                FmRadio.seek(b["up"]?.jsonPrimitive?.booleanOrNull != false)
                json(radio())
            }
            path == "/radio/scan" && method == Method.POST -> {
                FmRadio.scan()
                json(radio())
            }
            path == "/radio/presets" && method == Method.PUT -> json(putPresets(b))
            path == "/bluetooth" && method == Method.GET -> json(bluetooth())
            path == "/bluetooth" && method == Method.PUT -> {
                b["enabled"]?.jsonPrimitive?.booleanOrNull?.let { bt().power(it) }
                json(bluetooth())
            }
            path == "/bluetooth/search" && method == Method.POST -> json(bluetooth().also { bt().search() })
            path.startsWith("/bluetooth/") && method == Method.POST -> btAction(path.removePrefix("/bluetooth/"), b)
            path == "/server" && method == Method.GET -> json(server())
            path == "/server" && method == Method.PUT -> putServer(b)
            path == "/server/test" && method == Method.POST -> json(testServer())
            path == "/power" && method == Method.POST -> power(b)
            path == "/update" && method == Method.GET -> json(updateState())
            path == "/update" && method == Method.PUT -> update(s).apply { if (status != Response.Status.OK) closeConnection(true) } // body may be unread
            path == "/playlists" && method == Method.GET -> json(playlists())
            path == "/playlists" && method == Method.POST -> playlistChange { Playlists.create(b["name"]?.jsonPrimitive?.contentOrNull.orEmpty(), songPaths(b).orEmpty()) }
            path == "/playlists" && method == Method.PUT -> playlistChange {
                Playlists.update(s.parms["name"].orEmpty(), newName = b["name"]?.jsonPrimitive?.contentOrNull, paths = songPaths(b))
            }
            path == "/playlists" && method == Method.DELETE -> playlistChange {
                Playlists.delete(s.parms["name"].orEmpty())
                null
            }
            else -> error(Response.Status.NOT_FOUND, "not found")
        }
    }

    // five wrong guesses: a new code on the device and a 30 s pause for everyone (at most 10 guesses a
    // minute against a code of a million)
    private fun pair(s: IHTTPSession): Response {
        val sent = body(s)["code"]?.jsonPrimitive?.contentOrNull?.trim().orEmpty()
        synchronized(this) {
            val wait = pairBlockedUntil - SystemClock.uptimeMillis()
            if (wait > 0) return wait(wait)
            if (!java.security.MessageDigest.isEqual(sent.toByteArray(), code.toByteArray())) {
                if (++wrongCodes >= 5) {
                    code = newCode()
                    wrongCodes = 0
                    pairBlockedUntil = SystemClock.uptimeMillis() + PAIR_PAUSE_MS
                    Log.w("Y1", "dashboard: five wrong codes, new code and a pause")
                    return wait(PAIR_PAUSE_MS)
                }
                return error(Response.Status.FORBIDDEN, "code")
            }
            wrongCodes = 0
        }
        lastRequestAt = SystemClock.uptimeMillis()
        val token = ByteArray(24).also { random.nextBytes(it) }.joinToString("") { "%02x".format(it) }
        synchronized(sessions) { sessions += token }
        markSeen(token)
        return json(buildJsonObject { put("session", token) }).apply {
            addHeader("Set-Cookie", "rondo_session=$token; Path=/; SameSite=Strict")
        }
    }

    private fun wait(ms: Long): Response =
        newFixedLengthResponse(Response.Status.TOO_MANY_REQUESTS, "application/json",
            buildJsonObject { put("error", "wait"); put("retryAfterSec", (ms + 999) / 1000) }.toString())

    private fun markSeen(token: String) {
        val now = SystemClock.uptimeMillis()
        synchronized(seen) {
            seen[token] = now
            browsers = seen.values.count { now - it < 30_000 }
        }
    }

    private fun cookie(header: String?, name: String): String? =
        header?.split(';')?.map { it.trim() }?.firstOrNull { it.startsWith("$name=") }?.substringAfter('=')

    // small JSON bodies only (uploads stream separately)
    private fun body(s: IHTTPSession): JsonObject {
        val length = s.headers["content-length"]?.toIntOrNull() ?: 0
        if (length <= 0 || length > 64 * 1024) return JsonObject(emptyMap())
        val bytes = ByteArray(length)
        var read = 0
        while (read < length) {
            val n = s.inputStream.read(bytes, read, length - read)
            if (n < 0) break
            read += n
        }
        return runCatching { Api.json.parseToJsonElement(String(bytes, 0, read, Charsets.UTF_8)).jsonObject }.getOrDefault(JsonObject(emptyMap()))
    }

    private fun json(e: JsonElement): Response =
        newFixedLengthResponse(Response.Status.OK, "application/json; charset=utf-8", e.toString()).apply { addHeader("Cache-Control", "no-store") }

    private fun ok() = json(buildJsonObject { put("ok", true) })

    private fun error(status: Response.IStatus, reason: String): Response =
        newFixedLengthResponse(status, "application/json; charset=utf-8", buildJsonObject { put("error", reason) }.toString())

    // runs on the main thread and waits (player, views, receivers live there)
    private fun <T> onMain(block: () -> T): T {
        if (Looper.myLooper() == Looper.getMainLooper()) return block()
        var result: Result<T>? = null
        val done = CountDownLatch(1)
        main.post {
            result = runCatching(block)
            done.countDown()
        }
        if (!done.await(5, TimeUnit.SECONDS)) throw IllegalStateException("device busy")
        return result!!.getOrThrow()
    }

    // the answer goes out first: the Y1 starts shutting down a moment later
    private fun power(b: JsonObject): Response {
        if (!Power.available(context)) return error(Response.Status.CONFLICT, "firmware")
        val action = b["action"]?.jsonPrimitive?.contentOrNull
        val run: () -> Unit = when (action) {
            "shutdown" -> { { Power.shutDown(context) } }
            "restart" -> { { Power.restart(context) } }
            else -> return error(Response.Status.BAD_REQUEST, "action")
        }
        Log.i("Y1", "dashboard: power $action")
        main.postDelayed({
            shell.player?.let { if (it.isPlaying()) it.togglePlay() }
            run()
        }, 800)
        return ok()
    }

    // --- software update (SoftwareUpdate.kt): an APK from the page, installed over Rondo ---

    // the APK as the body (Content-Length, like uploads). The answer goes out first, saying which
    // version arrived; Android then stops Rondo, swaps it and starts it again (the page reconnects)
    private fun update(s: IHTTPSession): Response {
        if (!SoftwareUpdate.available(context)) return error(Response.Status.CONFLICT, "firmware")
        val length = s.headers["content-length"]?.toLongOrNull() ?: return error(Response.Status.LENGTH_REQUIRED, "length")
        if (length > 64L * 1024 * 1024) return error(Response.Status.BAD_REQUEST, "That's too big for a Rondo update")
        val apk = SoftwareUpdate.file(context)
        val partial = File(apk.path + ".part")
        try {
            partial.outputStream().buffered(64 * 1024).use { out ->
                val buf = ByteArray(64 * 1024)
                var left = length
                while (left > 0) {
                    val r = s.inputStream.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
                    if (r < 0) break
                    out.write(buf, 0, r)
                    left -= r
                }
                if (left > 0) throw IllegalStateException("upload cut short")
            }
            if (!partial.renameTo(apk)) throw IllegalStateException("cannot save")
        } catch (e: Exception) {
            partial.delete()
            return error(Response.Status.INTERNAL_ERROR, e.message ?: "upload failed")
        }
        val build = SoftwareUpdate.inspect(context, apk)
        val installed = SoftwareUpdate.installed(context)
        when {
            build == null -> return error(Response.Status.BAD_REQUEST, "That isn't an update for Rondo").also { apk.delete() }
            build.code < installed.code -> return error(Response.Status.CONFLICT, "That's an older Rondo (${build.version})").also { apk.delete() }
        }
        Log.i("Y1", "dashboard: update ${build!!.version} arrived")
        main.postDelayed({
            shell.player?.let { if (it.isPlaying()) it.togglePlay() }
            Thread {
                val problem = SoftwareUpdate.install(context, apk) ?: return@Thread
                Log.w("Y1", "dashboard: update not installed: $problem")
                updateProblem = problem
            }.start()
        }, 800)
        return json(buildJsonObject { put("version", build.version) })
    }

    // why the last update from the page didn't install, for GET /api/update
    @Volatile private var updateProblem: String? = null

    private fun updateState(): JsonObject = buildJsonObject {
        put("version", BuildConfig.VERSION_NAME)
        put("canInstall", SoftwareUpdate.available(context))
        put("problem", updateProblem)
    }

    // --- status and player ---

    private fun status(): JsonObject {
        val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = battery?.let { it.getIntExtra(BatteryManager.EXTRA_LEVEL, 0) * 100 / it.getIntExtra(BatteryManager.EXTRA_SCALE, 100).coerceAtLeast(1) }
        val plugged = (battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
        val card = cardRoot()
        val stat = card?.let { runCatching { StatFs(it.path) }.getOrNull() }
        return buildJsonObject {
            put("device", buildJsonObject {
                put("name", "Rondo")
                put("version", BuildConfig.VERSION_NAME)
                put("model", "Innioasis Y1")
                put("power", Power.available(context)) // shut down and restart (firmware builds)
                put("canUpdate", SoftwareUpdate.available(context)) // install an update from the page
            })
            put("battery", buildJsonObject {
                put("level", level)
                put("charging", plugged)
            })
            put("storage", stat?.let {
                @Suppress("DEPRECATION")
                buildJsonObject {
                    put("freeBytes", it.availableBlocks.toLong() * it.blockSize)
                    put("totalBytes", it.blockCount.toLong() * it.blockSize)
                }
            } ?: JsonNull)
            put("wifi", currentWifi() ?: JsonNull)
            put("player", onMain { player() })
        }
    }

    private fun player(): JsonElement {
        val p = shell.player ?: return JsonNull
        val item = p.current ?: return JsonNull
        val audio = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val local = item.id.startsWith("/")
        return buildJsonObject {
            put("title", item.title)
            put("subtitle", item.subtitle)
            put("path", if (local) item.id else null)
            put("art", when {
                local -> "/api/art?path=" + enc(item.id) + "&size=300"
                item.thumb != null -> "/api/thumb?path=" + enc(item.thumb)
                else -> null
            })
            put("source", if (local) "device" else "youtube")
            put("playing", p.isPlaying())
            put("positionMs", p.positionMs())
            put("durationMs", p.durationMs())
            put("volume", audio.getStreamVolume(AudioManager.STREAM_MUSIC))
            put("maxVolume", audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC))
        }
    }

    private fun playerAction(b: JsonObject): JsonElement = onMain {
        val p = shell.player
        when (b["action"]?.jsonPrimitive?.contentOrNull) {
            "toggle" -> p?.togglePlay()
            "next" -> p?.next()
            "previous" -> p?.previous()
            "seek" -> b["positionMs"]?.jsonPrimitive?.longOrNull?.let { p?.seekTo(it) }
            "volume" -> b["volume"]?.jsonPrimitive?.intOrNull?.let {
                val audio = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                audio.setStreamVolume(AudioManager.STREAM_MUSIC, it.coerceIn(0, audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC)), 0)
            }
        }
        player()
    }

    // `queue` (a playlist's songs): play those around the song; otherwise the whole library in its order
    private fun playPath(b: JsonObject): JsonElement {
        val path = b["path"]?.jsonPrimitive?.contentOrNull ?: return JsonNull
        val onCard = LocalLibrary.byPath()
        val queue = runCatching { b["queue"]!!.jsonArray.map { it.jsonPrimitive.content } }.getOrNull()
        val songs = queue?.mapNotNull { onCard[it] }?.filter { !it.video } ?: LocalLibrary.songs
        val index = songs.indexOfFirst { it.path == path }
        // songs only: a video would open the video player over Settings › Dashboard and stop it
        if (index < 0) return JsonNull
        return onMain {
            shell.player?.playQueue(songs.map(LocalLibrary::item), index)
            player()
        }
    }

    // --- playlists (local/Playlists.kt: .m3u8 files in the card's Playlists folder) ---

    private fun playlistJson(p: Playlists.Playlist, onCard: Map<String, LocalLibrary.Track>) = buildJsonObject {
        put("name", p.name)
        put("paths", JsonArray(p.paths.map { JsonPrimitive(it) }))
        put("missing", p.paths.count { it !in onCard }) // songs no longer on the card
        put("durationSec", p.paths.sumOf { onCard[it]?.durationSec ?: 0 })
    }

    private fun playlists(): JsonObject {
        val onCard = LocalLibrary.byPath()
        return buildJsonObject { put("playlists", JsonArray(Playlists.all().map { playlistJson(it, onCard) })) }
    }

    // songs on the card only, in the order sent
    private fun songPaths(b: JsonObject): List<String>? = runCatching {
        b["paths"]!!.jsonArray.map { it.jsonPrimitive.content }
            .filter { inside(it) && it.substringAfterLast('.', "").lowercase() in setOf("flac", "mp3", "m4a", "aac", "ogg", "opus", "wav") }
            .distinct()
    }.getOrNull()

    private fun playlistChange(change: () -> Playlists.Playlist?): Response = try {
        change()?.let { json(playlistJson(it, LocalLibrary.byPath())) } ?: ok()
    } catch (e: Playlists.Problem) {
        error(if (e.message.orEmpty().startsWith("There's already")) Response.Status.CONFLICT else Response.Status.BAD_REQUEST, e.message ?: "playlist")
    }

    // YouTube Music artwork through the music server (the Y1 never talks to Google)
    private fun thumb(path: String?): Response {
        if (path == null || !path.startsWith("/thumb?")) return error(Response.Status.NOT_FOUND, "not found")
        val bytes = art.get(path) ?: runCatching {
            Api.client.newCall(Request.Builder().url("${Api.base}$path&w=300").build()).execute().use { r ->
                if (r.isSuccessful) r.body()?.bytes() else null
            }
        }.getOrNull()?.also { art.put(path, it) } ?: return error(Response.Status.NOT_FOUND, "no art")
        return image(bytes)
    }

    private fun image(bytes: ByteArray) = newFixedLengthResponse(Response.Status.OK, "image/jpeg", bytes.inputStream(), bytes.size.toLong()).apply {
        addHeader("Cache-Control", "max-age=86400")
    }

    // --- library ---

    private val roots = listOf("/storage/sdcard0", "/storage/sdcard1")
    private val mediaExt = setOf("flac", "mp3", "m4a", "aac", "ogg", "opus", "wav", "mp4", "m4v", "mkv", "3gp", "webm", "lrc")

    private fun cardRoot(): File? = roots.map(::File).firstOrNull { it.isDirectory }

    private fun library(): JsonObject {
        if (LocalLibrary.songs.isEmpty() && !scanning) rescan()
        return buildJsonObject {
            put("songs", buildJsonArray {
                LocalLibrary.songs.forEach { t ->
                    add(buildJsonObject {
                        put("path", t.path)
                        put("title", t.title)
                        put("artist", t.artist)
                        put("album", t.album)
                        put("track", t.track)
                        put("durationSec", t.durationSec)
                        put("size", t.size)
                        put("modified", t.modified)
                        put("art", true)
                    })
                }
            })
            put("videos", buildJsonArray {
                LocalLibrary.videos.forEach { t ->
                    add(buildJsonObject {
                        put("path", t.path)
                        put("title", t.title)
                        put("durationSec", t.durationSec)
                        put("size", t.size)
                        put("modified", t.modified)
                    })
                }
            })
            put("scanning", scanning)
            put("folders", JsonArray(roots.filter { File(it).isDirectory }.map { JsonPrimitive("$it/Music") }))
        }
    }

    private fun rescan() {
        if (scanning) return
        scanning = true
        LocalLibrary.invalidate()
        io.launch {
            try {
                LocalLibrary.load(context)
                LocalLibrary.readTags(context)
            } finally {
                scanning = false
            }
        }
    }

    // embedded cover (or a frame of a video), about `size` px, JPEG
    private fun art(path: String?, size: Int): Response {
        if (path == null || !inside(path)) return error(Response.Status.NOT_FOUND, "not found")
        val key = "$path#$size"
        art.get(key)?.let { return image(it) }
        val retriever = MediaMetadataRetriever()
        val bitmap = (try {
            retriever.setDataSource(path)
            val pic = retriever.embeddedPicture
            if (pic != null) {
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeByteArray(pic, 0, pic.size, bounds)
                var sample = 1
                while (bounds.outWidth / (sample * 2) >= size) sample *= 2
                BitmapFactory.decodeByteArray(pic, 0, pic.size, BitmapFactory.Options().apply { inSampleSize = sample })
            } else {
                retriever.getFrameAtTime(10_000_000)
            }
        } catch (e: Exception) {
            null
        } finally {
            retriever.release()
        }) ?: return error(Response.Status.NOT_FOUND, "no art")
        val scaled = if (bitmap.width > size) Bitmap.createScaledBitmap(bitmap, size, bitmap.height * size / bitmap.width, true) else bitmap
        val bytes = ByteArrayOutputStream().also { scaled.compress(Bitmap.CompressFormat.JPEG, 85, it) }.toByteArray()
        art.put(key, bytes)
        return image(bytes)
    }

    private fun inside(path: String): Boolean {
        if (path.contains("..")) return false
        return roots.any { path.startsWith("$it/") }
    }

    // the raw file as the body: streamed straight to the card, no temporary copy
    private fun upload(s: IHTTPSession): Response {
        val name = s.parms["name"]?.substringAfterLast('/')?.trim()?.takeIf { it.isNotEmpty() && !it.startsWith(".") }
            ?: return error(Response.Status.BAD_REQUEST, "name")
        val ext = name.substringAfterLast('.', "").lowercase()
        if (ext !in mediaExt) return error(Response.Status.UNSUPPORTED_MEDIA_TYPE, "type")
        val folder = File(s.parms["folder"] ?: "${cardRoot()?.path ?: roots[0]}/Music")
        if (!inside(folder.path + "/x")) return error(Response.Status.FORBIDDEN, "folder")
        val length = s.headers["content-length"]?.toLongOrNull() ?: return error(Response.Status.LENGTH_REQUIRED, "length")
        val stat = runCatching { StatFs(cardRoot()?.path ?: roots[0]) }.getOrNull()
        @Suppress("DEPRECATION")
        if (stat != null && stat.availableBlocks.toLong() * stat.blockSize < length + 16L * 1024 * 1024) return error(INSUFFICIENT_STORAGE, "full")
        folder.mkdirs()
        var target = File(folder, name)
        var n = 2
        while (target.exists()) target = File(folder, "${name.substringBeforeLast('.')} (${n++}).$ext")
        val partial = File(folder, ".${target.name}.part")
        try {
            partial.outputStream().buffered(64 * 1024).use { out ->
                val buf = ByteArray(64 * 1024)
                var left = length
                while (left > 0) {
                    val r = s.inputStream.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
                    if (r < 0) break
                    out.write(buf, 0, r)
                    left -= r
                }
                if (left > 0) throw IllegalStateException("upload cut short")
            }
            if (!partial.renameTo(target)) throw IllegalStateException("cannot save")
        } catch (e: Exception) {
            partial.delete()
            return error(Response.Status.INTERNAL_ERROR, e.message ?: "upload failed")
        }
        Log.i("Y1", "dashboard: uploaded ${target.name} (${length / 1024} KB)")
        scheduleRescan()
        return json(buildJsonObject { put("path", target.path) })
    }

    private val rescanLater = Runnable { rescan() }

    // several files usually arrive in a row: scan once they stop
    private fun scheduleRescan() {
        main.removeCallbacks(rescanLater)
        main.postDelayed(rescanLater, 3_000)
    }

    private fun delete(path: String?): Response {
        if (path == null || !inside(path) || path.substringAfterLast('.', "").lowercase() !in mediaExt) return error(Response.Status.BAD_REQUEST, "path")
        val file = File(path)
        if (!file.isFile) return error(Response.Status.NOT_FOUND, "not found")
        if (!file.delete()) return error(Response.Status.INTERNAL_ERROR, "cannot delete")
        runCatching { Playlists.forget(path) }
        Log.i("Y1", "dashboard: deleted ${file.name}")
        scheduleRescan()
        return ok()
    }

    // --- settings ---

    private val timeouts = listOf(15, 30, 60, 120, 300, 600)

    private fun settings(): JsonObject {
        val r = context.contentResolver
        val brightness = Settings.System.getInt(r, Settings.System.SCREEN_BRIGHTNESS, 128)
        val timeout = Settings.System.getInt(r, Settings.System.SCREEN_OFF_TIMEOUT, 60_000) / 1000
        return buildJsonObject {
            put("videoQuality", Prefs.videoQuality(context))
            put("brightness", (brightness * 100 + 127) / 255)
            put("screenTimeoutSec", timeouts.minByOrNull { Math.abs(it - timeout) })
            put("lyricsOnOpen", Config.behavior.lyricsOnOpen)
            put("wheelAcceleration", Config.behavior.wheelAcceleration)
            put("wheelVibration", Config.behavior.wheelVibration)
            put("clickSound", com.jarvissax.rondo.ui.screens.clickSound(context))
        }
    }

    private fun putSettings(b: JsonObject): JsonObject {
        val r = context.contentResolver
        if (b.containsKey("videoQuality")) {
            val q = b["videoQuality"]?.jsonPrimitive?.intOrNull?.takeIf { it in listOf(360, 480, 720) }
            Prefs.setVideoQuality(context, q)
            QualityPreference.height = q
        }
        b["brightness"]?.jsonPrimitive?.intOrNull?.let {
            Settings.System.putInt(r, Settings.System.SCREEN_BRIGHTNESS_MODE, Settings.System.SCREEN_BRIGHTNESS_MODE_MANUAL)
            Settings.System.putInt(r, Settings.System.SCREEN_BRIGHTNESS, (it.coerceIn(10, 100) * 255 / 100))
        }
        b["screenTimeoutSec"]?.jsonPrimitive?.intOrNull?.let { sec ->
            Settings.System.putInt(r, Settings.System.SCREEN_OFF_TIMEOUT, (timeouts.minByOrNull { Math.abs(it - sec) } ?: 60) * 1000)
        }
        b["clickSound"]?.jsonPrimitive?.booleanOrNull?.let { com.jarvissax.rondo.ui.screens.setClickSound(context, it) }
        val lyrics = b["lyricsOnOpen"]?.jsonPrimitive?.booleanOrNull
        val wheel = b["wheelAcceleration"]?.jsonPrimitive?.contentOrNull?.takeIf { it in listOf("off", "normal", "fast") }
        val buzz = b["wheelVibration"]?.jsonPrimitive?.contentOrNull?.takeIf { it in WheelHaptics.LEVELS }
        if (lyrics != null || wheel != null || buzz != null) {
            Config.update(Config.Change.BEHAVIOR) { d ->
                d.copy(behavior = d.behavior.copy(
                    lyricsOnOpen = lyrics ?: d.behavior.lyricsOnOpen,
                    wheelAcceleration = wheel ?: d.behavior.wheelAcceleration,
                    wheelVibration = buzz ?: d.behavior.wheelVibration,
                ))
            }
        }
        return settings()
    }

    // --- theme ---

    private fun theme(): JsonObject {
        val t = Config.theme
        return buildJsonObject {
            put("accent", t.accent)
            put("appearance", t.appearance)
            put("textScale", t.textScale)
            put("menu", buildJsonArray {
                t.menu.forEach { m ->
                    add(buildJsonObject {
                        put("id", m.id)
                        put("label", Config.MENU_LABELS[m.id] ?: m.id)
                        put("visible", m.visible)
                        if (m.id == "settings") put("locked", true)
                    })
                }
            })
            put("presets", buildJsonArray {
                Config.PRESETS.forEach { (name, accent) ->
                    add(buildJsonObject {
                        put("name", name)
                        put("accent", accent)
                    })
                }
            })
        }
    }

    private fun putTheme(b: JsonObject): JsonObject {
        Config.update(Config.Change.THEME) { d ->
            var t = d.theme
            b["accent"]?.jsonPrimitive?.contentOrNull?.let { t = t.copy(accent = it) }
            b["appearance"]?.jsonPrimitive?.contentOrNull?.let { t = t.copy(appearance = it) }
            b["textScale"]?.jsonPrimitive?.floatOrNull?.let { t = t.copy(textScale = it) }
            b["menu"]?.let { menu ->
                t = t.copy(menu = runCatching {
                    menu.jsonArray.map {
                        val o = it.jsonObject
                        Config.MenuEntry(o["id"]!!.jsonPrimitive.content, o["visible"]?.jsonPrimitive?.booleanOrNull ?: true)
                    }
                }.getOrDefault(t.menu))
            }
            d.copy(theme = t)
        }
        return theme()
    }

    // --- Wi-Fi ---

    private fun wm() = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager

    private fun currentWifi(): JsonObject? {
        val info = wm().connectionInfo ?: return null
        if (info.networkId == -1) return null
        val ip = info.ipAddress
        return buildJsonObject {
            put("ssid", Wifi.unquote(info.ssid))
            put("signal", WifiManager.calculateSignalLevel(info.rssi, 5))
            put("ip", "${ip and 0xff}.${ip shr 8 and 0xff}.${ip shr 16 and 0xff}.${ip shr 24 and 0xff}")
        }
    }

    private fun wifi(): JsonObject {
        val wm = wm()
        val saved = wm.configuredNetworks.orEmpty()
        val savedNames = saved.map { Wifi.unquote(it.SSID) }.toSet()
        return buildJsonObject {
            put("enabled", wm.isWifiEnabled)
            put("current", currentWifi() ?: JsonNull)
            put("joining", joining)
            put("lastError", joinError)
            put("saved", buildJsonArray {
                saved.forEach { c ->
                    add(buildJsonObject {
                        put("id", c.networkId)
                        put("ssid", Wifi.unquote(c.SSID))
                    })
                }
            })
            put("nearby", buildJsonArray {
                wm.scanResults.orEmpty()
                    .filter { it.SSID.isNotBlank() && !it.SSID.startsWith("NVRAM WARNING") }
                    .groupBy { it.SSID }.map { (_, l) -> l.maxByOrNull { it.level }!! }
                    .sortedByDescending { it.level }
                    .forEach { r ->
                        add(buildJsonObject {
                            put("ssid", r.SSID)
                            put("signal", WifiManager.calculateSignalLevel(r.level, 5))
                            put("security", Wifi.security(r).name.lowercase())
                            put("saved", r.SSID in savedNames)
                        })
                    }
            })
        }.also { wm.startScan() } // fresher next time
    }

    private fun joinWifi(b: JsonObject): Response {
        val ssid = b["ssid"]?.jsonPrimitive?.contentOrNull?.takeIf { it.isNotBlank() } ?: return error(Response.Status.BAD_REQUEST, "ssid")
        val password = b["password"]?.jsonPrimitive?.contentOrNull.orEmpty()
        val result = wm().scanResults.orEmpty().firstOrNull { it.SSID == ssid }
        val security = result?.let { Wifi.security(it) } ?: if (password.isEmpty()) Wifi.Security.OPEN else Wifi.Security.PSK
        if (security == Wifi.Security.EAP) return error(Response.Status.BAD_REQUEST, "enterprise")
        if (security == Wifi.Security.PSK && password.length < 8) return error(Response.Status.BAD_REQUEST, "password")
        joining = ssid
        joinError = null
        // answer first: joining may cut this very connection
        main.postDelayed({
            val saved = Wifi.saved(context, ssid)
            if (saved != null && password.isEmpty()) Wifi.connect(context, saved.networkId) else Wifi.join(context, ssid, security, password)
        }, 300)
        return ok()
    }

    private fun forgetWifi(id: Int?): Response {
        if (id == null) return error(Response.Status.BAD_REQUEST, "id")
        onMain { Wifi.forget(context, id) }
        return ok()
    }

    // --- music server ---

    private fun server(): JsonObject = buildJsonObject {
        put("url", Api.base)
        put("tokenSet", !Config.server.token.isNullOrEmpty())
    }

    // the address, and either a connect code from the server's admin page (docs/dev/server-api.md: the Rondo
    // swaps it for the one device token) or a token typed in. http:// is allowed (a home server by IP).
    private fun putServer(b: JsonObject): Response {
        val url = b["url"]?.jsonPrimitive?.contentOrNull?.trim()?.trimEnd('/')
        val code = b["code"]?.jsonPrimitive?.contentOrNull?.filter { it.isDigit() }
        if (url != null && !(url.startsWith("https://") || url.startsWith("http://"))) return error(Response.Status.BAD_REQUEST, "url")
        if (!code.isNullOrEmpty()) {
            if (code.length != 6) return error(Response.Status.BAD_REQUEST, "code")
            val target = url ?: Api.base
            val paired = try {
                Api.pair(target, code)
            } catch (e: HttpException) {
                return error(Response.Status.BAD_REQUEST, "HTTP ${e.code}")
            } catch (e: Exception) {
                return error(Response.Status.BAD_REQUEST, "unreachable")
            } ?: return error(Response.Status.FORBIDDEN, "code")
            Config.update(Config.Change.SERVER) { d -> d.copy(server = d.server.copy(url = target, token = paired)) }
            Log.i("Y1", "dashboard: connected to a server with a connect code")
            return json(server())
        }
        val token = b["token"]?.jsonPrimitive?.contentOrNull?.trim()
        if (url != null || token != null) {
            Config.update(Config.Change.SERVER) { d ->
                d.copy(server = d.server.copy(
                    url = url?.takeIf { it.startsWith("https://") || it.startsWith("http://") } ?: d.server.url,
                    token = token?.takeIf { it.isNotEmpty() } ?: d.server.token,
                ))
            }
        }
        return json(server())
    }

    // reachability from /healthz (no token), then an authenticated call to check the token
    private fun testServer(): JsonObject = runBlocking {
        val started = SystemClock.uptimeMillis()
        try {
            val health = Api.health()
            val latency = SystemClock.uptimeMillis() - started
            Api.time()
            buildJsonObject {
                put("ok", true)
                put("latencyMs", latency)
                put("ytdlp", health.ytdlp)
                put("playbackOk", health.lastResolveOk) // null: not checked since the server started
                put("signedIn", health.account.music || health.account.videos)
            }
        } catch (e: HttpException) {
            buildJsonObject {
                put("ok", false)
                put("error", if (e.code == 401 || e.code == 403) "unauthorized" else "HTTP ${e.code}")
            }
        } catch (e: Exception) {
            buildJsonObject {
                put("ok", false)
                put("error", "unreachable")
            }
        }
    }

    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8")

    // --- FM radio (the Rondo's radio, from the browser; player/FmRadio.kt) ---

    private fun station(f: Int, name: String) = buildJsonObject {
        put("frequency", f)
        put("name", name.ifEmpty { Config.radio.names[f].orEmpty() })
    }

    private fun radio(): JsonObject {
        val s = FmRadio.state
        val r = Config.radio
        return buildJsonObject {
            put("headphones", FmRadio.headphonesIn())
            put("on", s.on)
            put("starting", s.starting)
            put("frequency", s.frequency)
            put("name", s.name.ifEmpty { r.presets.firstOrNull { it.frequency == s.frequency }?.name?.ifEmpty { null } ?: r.names[s.frequency].orEmpty() })
            put("text", s.text)
            put("signal", s.signal)
            put("stereo", s.stereo)
            put("muted", s.muted)
            // the radio's own volume (MediaTek's FM stream), not the music volume
            val audio = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            put("volume", audio.getStreamVolume(FmRadio.STREAM_FM))
            put("maxVolume", audio.getStreamMaxVolume(FmRadio.STREAM_FM))
            put("speaker", s.speaker)
            put("seeking", s.seeking)
            put("scanning", s.scanning)
            s.error?.let { put("error", it) }
            put("band", r.band)
            put("min", FmRadio.MIN)
            put("max", FmRadio.MAX)
            put("presets", buildJsonArray { r.presets.forEach { add(station(it.frequency, it.name)) } })
            put("found", buildJsonArray { r.found.filter { f -> r.presets.none { it.frequency == f } }.forEach { add(station(it, "")) } })
        }
    }

    // {"on": bool, "frequency": 1005, "muted": bool, "speaker": bool, "band": "world" | "japan"}, any of them
    private fun putRadio(b: JsonObject): JsonObject {
        b["band"]?.jsonPrimitive?.contentOrNull?.takeIf { it == "world" || it == "japan" }?.let { FmRadio.setBand(it) }
        b["speaker"]?.jsonPrimitive?.booleanOrNull?.let { FmRadio.setSpeaker(it) }
        b["frequency"]?.jsonPrimitive?.intOrNull?.let { FmRadio.tune(it) }
        b["muted"]?.jsonPrimitive?.booleanOrNull?.let { FmRadio.setMuted(it) }
        b["volume"]?.jsonPrimitive?.intOrNull?.let {
            val audio = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            audio.setStreamVolume(FmRadio.STREAM_FM, it.coerceIn(0, audio.getStreamMaxVolume(FmRadio.STREAM_FM)), 0)
        }
        when (b["on"]?.jsonPrimitive?.booleanOrNull) {
            true -> main.post {
                // as on the Rondo's radio screen: music pauses for the radio
                if (shell.player?.isPlaying() == true) shell.player?.pause()
                FmRadio.start()
            }
            false -> FmRadio.stop()
            null -> Unit
        }
        return radio()
    }

    // the saved stations in the page's order and names: {"presets": [{"frequency": 1005, "name": "…"}]}
    private fun putPresets(b: JsonObject): JsonObject {
        val list = runCatching { b["presets"]!!.jsonArray }.getOrNull() ?: return radio()
        val presets = list.mapNotNull { e ->
            val o = runCatching { e.jsonObject }.getOrNull() ?: return@mapNotNull null
            val f = o["frequency"]?.jsonPrimitive?.intOrNull?.takeIf { it in 760..1080 } ?: return@mapNotNull null
            Config.Preset(f, o["name"]?.jsonPrimitive?.contentOrNull.orEmpty().trim().take(40))
        }.distinctBy { it.frequency }
        Config.update(Config.Change.RADIO) { d -> d.copy(radio = d.radio.copy(presets = presets)) }
        return radio()
    }

    // --- Bluetooth (the Rondo's Bluetooth screen, from the browser; ui/screens/BluetoothScreens.kt) ---

    // searching runs while the page is open: every GET keeps it going, 30 s without one stops it
    private var btSession: BtSession? = null
    @Volatile private var btPolledAt = 0L
    private val btIdle = object : Runnable {
        override fun run() {
            if (SystemClock.uptimeMillis() - btPolledAt > 30_000) {
                btSession?.stop()
                btSession = null
            } else {
                btSession?.searchAgainIfIdle()
                main.postDelayed(this, 5_000)
            }
        }
    }

    @Synchronized
    private fun bt(): BtSession {
        btPolledAt = SystemClock.uptimeMillis()
        btSession?.let { return it }
        val session = BtSession(context) {}
        btSession = session
        main.post {
            session.start()
            main.removeCallbacks(btIdle)
            main.postDelayed(btIdle, 5_000)
        }
        return session
    }

    private fun bluetooth(): JsonObject {
        Bt.adapter ?: return buildJsonObject { put("available", false) }
        val session = bt()
        return buildJsonObject {
            put("available", true)
            val state = session.state()
            put("state", when (state) {
                BluetoothAdapter.STATE_TURNING_ON -> "turning_on"
                BluetoothAdapter.STATE_TURNING_OFF -> "turning_off"
                BluetoothAdapter.STATE_ON -> "on"
                else -> "off"
            })
            put("searching", session.searching)
            if (state == BluetoothAdapter.STATE_ON) {
                put("mine", buildJsonArray {
                    session.bonded().forEach { d ->
                        add(buildJsonObject {
                            put("address", d.address)
                            put("name", Bt.name(d))
                            put("kind", Bt.kind(d))
                            put("audio", Bt.isAudio(d))
                            put("status", when {
                                Bt.state(d) == BluetoothProfile.STATE_CONNECTED -> "connected"
                                session.isConnecting(d) -> "connecting"
                                else -> "disconnected"
                            })
                        })
                    }
                })
                put("others", buildJsonArray {
                    session.others().forEach { d ->
                        add(buildJsonObject {
                            put("address", d.address)
                            put("name", Bt.name(d))
                            put("kind", Bt.kind(d))
                            put("audio", Bt.isAudio(d))
                            put("pairing", session.isPairing(d))
                        })
                    }
                })
            }
        }
    }

    // connect (pairs first when needed), disconnect or forget the device {"address": "…"}
    private fun btAction(action: String, b: JsonObject): Response {
        val adapter = Bt.adapter ?: return error(Response.Status.NOT_FOUND, "no bluetooth")
        val address = b["address"]?.jsonPrimitive?.contentOrNull
        if (address == null || !BluetoothAdapter.checkBluetoothAddress(address)) return error(Response.Status.BAD_REQUEST, "address")
        val device = adapter.getRemoteDevice(address)
        val session = bt()
        when (action) {
            "connect" -> if (device.bondState == BluetoothDevice.BOND_BONDED) session.connect(device) else session.pair(device)
            "disconnect" -> session.disconnect(device)
            "forget" -> session.forget(device)
            else -> return error(Response.Status.NOT_FOUND, "not found")
        }
        return json(bluetooth())
    }

    override fun stop() {
        super.stop()
        main.removeCallbacks(btIdle)
        btSession?.let { s -> main.post { s.stop() } }
        btSession = null
        main.removeCallbacks(rescanLater)
        runCatching { context.unregisterReceiver(wifiEvents) }
    }

    companion object {
        const val PORT = 8080
        const val PAIR_PAUSE_MS = 30_000L

        // 507, which NanoHTTPD has no constant for: the card is full
        private val INSUFFICIENT_STORAGE = object : Response.IStatus {
            override fun getDescription() = "507 Insufficient Storage"
            override fun getRequestStatus() = 507
        }
    }
}
