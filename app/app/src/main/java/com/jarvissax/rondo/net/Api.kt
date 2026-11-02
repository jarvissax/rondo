package com.jarvissax.rondo.net

import java.io.IOException
import java.net.URLEncoder
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import okhttp3.ConnectionSpec
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.TlsVersion
import com.jarvissax.rondo.BuildConfig
import com.jarvissax.rondo.Config

class HttpException(val code: Int, path: String) : IOException("HTTP $code for $path")

@Serializable
data class Health(val ok: Boolean, val ytdlp: String? = null, val lastResolveOk: Boolean? = null, val cookiesOk: Boolean = false, val account: HealthAccount = HealthAccount())

// which account feeds the server has (Accounts.kt)
@Serializable
data class HealthAccount(val music: Boolean = true, val videos: Boolean = true)

@Serializable
data class ServerTime(val epochMs: Long)

// GET /update
@Serializable
data class UpdateInfo(val version: String? = null, val size: Long = 0, val notes: String? = null)

// GET /video/short/:id
@Serializable
data class ShortInfo(val id: String, val title: String = "", val channel: String = "", val subtitle: String = "", val durationSec: Int? = null)

@Serializable
data class Section(val title: String = "", val items: List<Item> = emptyList())

@Serializable
data class SectionsPage(val sections: List<Section> = emptyList(), val next: String? = null)

@Serializable
data class ItemsPage(val items: List<Item> = emptyList(), val next: String? = null)

@Serializable
data class Library(val playlists: List<Item> = emptyList())

@Serializable
data class CollectionPage(val title: String = "", val subtitle: String = "", val thumb: String? = null, val items: List<Item> = emptyList(), val next: String? = null)

@Serializable
data class ArtistPage(val title: String = "", val thumb: String? = null, val sections: List<Section> = emptyList())

@Serializable
data class LyricLine(val t: Long, val text: String)

// /lyrics: timed lines (LRCLIB) when known, else plain text (LRCLIB or YouTube Music)
@Serializable
data class Lyrics(val synced: List<LyricLine>? = null, val plain: String? = null, val instrumental: Boolean = false, val source: String? = null)

// One row of any list the server returns (search results, playlists, up next)
@Serializable
data class Item(
    val kind: String,
    val id: String,
    val title: String,
    val subtitle: String = "",
    val durationSec: Int? = null,
    val thumb: String? = null,
)

// JSON API client for the Rondo server. Every request carries this Rondo's token.
object Api {
    // set from the dashboard's Server page (Config.server); a new Rondo has none
    val base: String get() = Config.server.url?.trimEnd('/').orEmpty()
    val authHeader: String get() = "Bearer ${Config.server.token.orEmpty()}"
    val json = Json { ignoreUnknownKeys = true }

    // TLS 1.2 for https servers (a VPS behind Caddy), and plain http for a server at home reached
    // by IP (docs/dev/server-api.md: both are supported). No auth: used to pair with a new server.
    val bare: OkHttpClient by lazy {
        OkHttpClient.Builder()
            .sslSocketFactory(Tls.socketFactory, Tls.trustManager)
            .connectionSpecs(listOf(ConnectionSpec.Builder(ConnectionSpec.MODERN_TLS).tlsVersions(TlsVersion.TLS_1_2).build(), ConnectionSpec.CLEARTEXT))
            .addInterceptor(ConnectRetryInterceptor())
            // how the server's admin page names this device
            .addInterceptor { chain -> chain.proceed(chain.request().newBuilder().header("User-Agent", USER_AGENT).build()) }
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(15, TimeUnit.SECONDS)
            .writeTimeout(15, TimeUnit.SECONDS)
            .build()
    }

    const val NOT_SET_UP = "No music server yet.\nConnect one in Settings › Server."

    val USER_AGENT = "Rondo/${BuildConfig.VERSION_NAME} (Innioasis Y1; Android 4.2)"

    // every request to the music server carries the device token
    val client: OkHttpClient by lazy {
        bare.newBuilder()
            .addInterceptor { chain ->
                chain.proceed(chain.request().newBuilder().header("Authorization", authHeader).build())
            }
            .build()
    }

    // redeem a connect code from the server's admin page for the one device token (POST /device/pair);
    // null when the code is wrong or expired
    fun pair(url: String, code: String): String? {
        val body = okhttp3.RequestBody.create(okhttp3.MediaType.parse("application/json"), "{\"code\":\"$code\",\"name\":\"Rondo\"}")
        bare.newCall(Request.Builder().url(url.trimEnd('/') + "/device/pair").post(body).build()).execute().use { res ->
            if (res.code() == 403) return null
            if (!res.isSuccessful) throw HttpException(res.code(), "/device/pair")
            val text = res.body()?.string() ?: return null
            return json.parseToJsonElement(text).let { (it as? kotlinx.serialization.json.JsonObject)?.get("token") }
                ?.let { (it as? kotlinx.serialization.json.JsonPrimitive)?.content }
        }
    }

    val configured: Boolean get() = base.isNotEmpty()

    suspend fun get(path: String): String = withContext(Dispatchers.IO) {
        if (!configured) throw java.io.IOException(NOT_SET_UP)
        client.newCall(Request.Builder().url(base + path).build()).execute().use { res ->
            if (!res.isSuccessful) throw HttpException(res.code(), path)
            res.body()?.string() ?: ""
        }
    }

    suspend fun health(): Health = json.decodeFromString(Health.serializer(), get("/healthz")).also {
        Accounts.update(it.account.music, it.account.videos)
    }

    suspend fun time(): ServerTime = json.decodeFromString(ServerTime.serializer(), get("/time"))

    // Lists come a page at a time: a page with `next` set continues with `?next=<token>` on the same
    // path (server/src/paging.ts). A token expires after 30 min (HTTP 410): the screen reloads.
    suspend fun musicSearch(query: String, type: String = "song") =
        json.decodeFromString(ItemsPage.serializer(), get("/music/search?type=$type&q=" + enc(query)))

    suspend fun videoSearch(query: String) = json.decodeFromString(ItemsPage.serializer(), get("/video/search?q=" + enc(query)))

    suspend fun more(path: String, next: String) = json.decodeFromString(ItemsPage.serializer(), get(withNext(path, next)))
    suspend fun moreSections(path: String, next: String) = json.decodeFromString(SectionsPage.serializer(), get(withNext(path, next)))

    private fun withNext(path: String, next: String) = path.substringBefore("?") + "?next=" + enc(next)

    suspend fun musicHome() = json.decodeFromString(SectionsPage.serializer(), cached("/music/home"))
    suspend fun musicLibrary() = json.decodeFromString(Library.serializer(), cached("/music/library"))
    suspend fun musicPlaylist(id: String) = json.decodeFromString(CollectionPage.serializer(), cached("/music/playlist/" + enc(id)))
    suspend fun musicAlbum(id: String) = json.decodeFromString(CollectionPage.serializer(), cached("/music/album/" + enc(id)))
    suspend fun musicArtist(id: String) = json.decodeFromString(ArtistPage.serializer(), cached("/music/artist/" + enc(id)))
    suspend fun musicNext(id: String) = json.decodeFromString(ItemsPage.serializer(), get("/music/next/" + enc(id))).items

    suspend fun lyrics(title: String, artist: String, album: String?, durationSec: Int?, id: String?): Lyrics {
        val q = StringBuilder("/lyrics?title=").append(enc(title)).append("&artist=").append(enc(artist))
        album?.takeIf { it.isNotEmpty() }?.let { q.append("&album=").append(enc(it)) }
        durationSec?.let { q.append("&duration=").append(it) }
        id?.let { q.append("&id=").append(enc(it)) }
        return json.decodeFromString(Lyrics.serializer(), cached(q.toString(), ttlMs = 30 * 60_000))
    }

    // /video/home, /video/subscriptions, /video/history, /video/related/<id>
    suspend fun videoFeed(path: String) = json.decodeFromString(ItemsPage.serializer(), cached(path))

    // a short's title, channel and views (the Shorts chain names only the video); cached a while
    suspend fun shortInfo(id: String) = json.decodeFromString(ShortInfo.serializer(), cached("/video/short/" + enc(id), 600_000))

    // asks the server for a video's manifest now, so it has the stream ready when it's played
    suspend fun warm(id: String) {
        get("/mpd/" + enc(id))
    }

    // the newest Rondo with an update APK (GET /update; version null when there's none)
    suspend fun updateInfo() = json.decodeFromString(UpdateInfo.serializer(), get("/update"))

    // the update APK, through the server, into `to`; progress from 0 to 1 as it arrives
    suspend fun downloadUpdate(to: java.io.File, size: Long, progress: (Float) -> Unit) = withContext(Dispatchers.IO) {
        if (!configured) throw IOException(NOT_SET_UP)
        client.newBuilder().readTimeout(60, TimeUnit.SECONDS).build()
            .newCall(Request.Builder().url("$base/update/rondo.apk").build()).execute().use { res ->
                if (!res.isSuccessful) throw HttpException(res.code(), "/update/rondo.apk")
                val body = res.body() ?: throw IOException("empty answer")
                val total = body.contentLength().takeIf { it > 0 } ?: size
                val partial = java.io.File(to.path + ".part")
                partial.outputStream().use { out ->
                    val input = body.byteStream()
                    val buf = ByteArray(64 * 1024)
                    var done = 0L
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        done += n
                        if (total > 0) progress((done.toFloat() / total).coerceAtMost(1f))
                    }
                    if (total > 0 && done < total) throw IOException("the download stopped early")
                }
                if (!partial.renameTo(to)) throw IOException("couldn't save the update")
            }
    }

    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8")

    // reopening a screen within two minutes answers from memory (screens should open in under 400 ms)
    private val responses = object : LinkedHashMap<String, Pair<Long, String>>(32, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Pair<Long, String>>?) = size > 40
    }

    // another server: what the old one answered is not valid any more
    fun clearCache() = synchronized(responses) { responses.clear() }

    private suspend fun cached(path: String, ttlMs: Long = 120_000): String {
        synchronized(responses) { responses[path] }?.let { (at, body) -> if (System.currentTimeMillis() - at < ttlMs) return body }
        val body = get(path)
        synchronized(responses) { responses[path] = System.currentTimeMillis() to body }
        return body
    }
}
