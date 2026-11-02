package com.jarvissax.rondo.local

import android.content.Context
import android.media.MediaMetadataRetriever
import android.util.Log
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import com.jarvissax.rondo.net.Item
import com.jarvissax.rondo.ui.ImageLoader

// Music and videos on the microSD card (/storage/sdcard0) and the internal FAT partition
// (/storage/sdcard1). The ROM has no MediaProvider (docs/device/README.md), so we walk the
// folders ourselves. A scan lists files at once with titles from their names ("Artist -
// Title.flac"); tags (title, artist, album, track, duration) are read afterwards in the
// background and kept in files/local-index.json, so only new or changed files are read again.
object LocalLibrary {
    @Serializable
    data class Track(
        val path: String,
        val size: Long,
        val modified: Long,
        val video: Boolean,
        val title: String,
        val artist: String = "",
        val album: String = "",
        val track: Int = 0,
        val durationSec: Int? = null,
        val tagged: Boolean = false,
    )

    private val roots = listOf("/storage/sdcard0", "/storage/sdcard1")
    private val audioExt = setOf("flac", "mp3", "m4a", "aac", "ogg", "wav", "opus")
    private val videoExt = setOf("mp4", "m4v", "mkv", "3gp", "webm")
    private val skip = setOf("android", "lost.dir", "dcim", ".thumbnails")

    private val json = Json { ignoreUnknownKeys = true }
    private val lock = Mutex()
    private var tracks: List<Track> = emptyList()
    private var scannedAt = 0L

    // the dashboard added or removed files: scan again on the next load
    fun invalidate() {
        scannedAt = 0L
    }

    // listeners get the library again while tags are being read
    var onUpdate: (() -> Unit)? = null

    val songs: List<Track> get() = tracks.filter { !it.video }.sortedBy { it.title.lowercase() }
    val videos: List<Track> get() = tracks.filter { it.video }.sortedBy { it.title.lowercase() }

    fun byPath(): Map<String, Track> = tracks.associateBy { it.path }

    data class Found(val artists: List<Pair<String, List<Track>>>, val albums: List<Pair<String, List<Track>>>, val songs: List<Track>, val videos: List<Track>) {
        val isEmpty get() = artists.isEmpty() && albums.isEmpty() && songs.isEmpty() && videos.isEmpty()
    }

    // The dashboard's search: every word has to be in the title, artist, album or file name,
    // ignoring case and accents (fold() in assets/dashboard/app.js). Artists and albums match on
    // their own name.
    fun search(query: String): Found {
        val words = fold(query).split(' ').filter { it.isNotEmpty() }
        if (words.isEmpty()) return Found(emptyList(), emptyList(), emptyList(), emptyList())
        val hit = { text: String -> fold(text).let { key -> words.all { key.contains(it) } } }
        val all = tracks
        return Found(
            artists = artists().filter { hit(it.first) },
            albums = albums().filter { hit(it.first) },
            songs = all.filter { !it.video && hit("${it.title} ${it.artist} ${it.album} ${File(it.path).name}") }.sortedBy { it.title.lowercase() },
            videos = all.filter { it.video && hit("${it.title} ${File(it.path).name}") }.sortedBy { it.title.lowercase() },
        )
    }

    fun fold(s: String): String = java.text.Normalizer.normalize(s.lowercase(), java.text.Normalizer.Form.NFD)
        .replace(Regex("\\p{Mn}+"), "").replace('ı', 'i').replace('ə', 'e')

    // a song or video, and a .lrc next to it, off the card; playlists forget it too
    suspend fun delete(context: Context, path: String): Boolean = withContext(Dispatchers.IO) {
        val file = File(path)
        if (!file.isFile || !file.delete()) return@withContext false
        File(file.parentFile, file.nameWithoutExtension + ".lrc").takeIf { it.isFile }?.delete()
        lock.withLock { tracks = tracks.filter { it.path != path } }
        writeIndex(context, tracks)
        runCatching { Playlists.forget(path) }
        Log.i("Y1", "local: deleted ${file.name}")
        true
    }

    // newest first, by the files' dates (the dashboard's "Recently added")
    val recent: List<Track> get() = tracks.filter { !it.video }.sortedByDescending { it.modified }

    fun artists(): List<Pair<String, List<Track>>> = tracks.filter { !it.video }
        .groupBy { it.artist.ifEmpty { "Unknown artist" } }
        .toSortedMap(String.CASE_INSENSITIVE_ORDER)
        .map { (name, list) -> name to list.sortedWith(compareBy({ it.album.lowercase() }, { it.track }, { it.title.lowercase() })) }

    fun albums(): List<Pair<String, List<Track>>> = tracks.filter { !it.video && it.album.isNotEmpty() }
        .groupBy { it.album }
        .toSortedMap(String.CASE_INSENSITIVE_ORDER)
        .map { (name, list) -> name to list.sortedWith(compareBy({ it.track }, { it.title.lowercase() })) }

    // rescans at most every 30 s (the card does not change while the Y1 is in use)
    suspend fun load(context: Context): List<Track> = lock.withLock {
        if (tracks.isNotEmpty() && System.currentTimeMillis() - scannedAt < 30_000) return tracks
        withContext(Dispatchers.IO) {
            val known = (if (tracks.isEmpty()) readIndex(context) else tracks).associateBy { it.path }
            val found = mutableListOf<Track>()
            roots.map(::File).filter { it.isDirectory }.forEach { walk(it, 0, found, known) }
            tracks = found
            scannedAt = System.currentTimeMillis()
            Log.i("Y1", "local: ${found.count { !it.video }} songs, ${found.count { it.video }} videos")
        }
        tracks
    }

    private fun walk(dir: File, depth: Int, out: MutableList<Track>, known: Map<String, Track>) {
        if (depth > 6) return
        val children = dir.listFiles() ?: return
        for (f in children) {
            val name = f.name
            if (name.startsWith(".")) continue
            if (f.isDirectory) {
                if (name.lowercase() !in skip) walk(f, depth + 1, out, known)
                continue
            }
            val ext = name.substringAfterLast('.', "").lowercase()
            val video = ext in videoExt
            if (!video && ext !in audioExt) continue
            val size = f.length()
            val modified = f.lastModified()
            val old = known[f.path]
            out += if (old != null && old.size == size && old.modified == modified) old else fromName(f, size, modified, video)
        }
    }

    // "Artist - Title.flac" (how the card is laid out); a "01 " track prefix is dropped
    private fun fromName(f: File, size: Long, modified: Long, video: Boolean): Track {
        val base = f.nameWithoutExtension.replace('_', ' ').trim()
        val parts = base.split(" - ", limit = 2)
        val artist = if (parts.size == 2) parts[0].trim() else f.parentFile?.name?.takeIf { it.lowercase() !in setOf("music", "video", "videos", "movies", "sdcard0", "sdcard1") } ?: ""
        val title = (if (parts.size == 2) parts[1] else base).replace(Regex("^\\d{1,3}[ .-]+"), "").trim().ifEmpty { base }
        return Track(f.path, size, modified, video, title, artist)
    }

    // reads tags of files not read yet, a few at a time, saving the index as it goes
    suspend fun readTags(context: Context) = withContext(Dispatchers.IO) {
        val todo = tracks.filter { !it.tagged }
        if (todo.isEmpty()) return@withContext
        Log.i("Y1", "local: reading tags of ${todo.size} files")
        val byPath = tracks.associateBy { it.path }.toMutableMap()
        todo.forEachIndexed { i, t ->
            byPath[t.path] = tagged(t)
            if ((i + 1) % 40 == 0 || i == todo.lastIndex) {
                lock.withLock { tracks = tracks.map { byPath[it.path] ?: it } }
                writeIndex(context, tracks)
                withContext(Dispatchers.Main) { onUpdate?.invoke() }
            }
        }
    }

    private fun tagged(t: Track): Track {
        val r = MediaMetadataRetriever()
        return try {
            r.setDataSource(t.path)
            fun tag(key: Int) = r.extractMetadata(key)?.trim()?.takeIf { it.isNotEmpty() }
            t.copy(
                title = tag(MediaMetadataRetriever.METADATA_KEY_TITLE) ?: t.title,
                artist = tag(MediaMetadataRetriever.METADATA_KEY_ARTIST) ?: tag(MediaMetadataRetriever.METADATA_KEY_ALBUMARTIST) ?: t.artist,
                album = tag(MediaMetadataRetriever.METADATA_KEY_ALBUM) ?: t.album,
                track = tag(MediaMetadataRetriever.METADATA_KEY_CD_TRACK_NUMBER)?.substringBefore('/')?.toIntOrNull() ?: 0,
                durationSec = tag(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull()?.let { (it / 1000).toInt() },
                tagged = true,
            )
        } catch (e: Exception) {
            t.copy(tagged = true) // unreadable: keep the name-based entry
        } finally {
            r.release()
        }
    }

    private fun indexFile(context: Context) = File(context.filesDir, "local-index.json")

    private fun readIndex(context: Context): List<Track> = runCatching {
        json.decodeFromString(ListSerializer(Track.serializer()), indexFile(context).readText())
    }.getOrDefault(emptyList())

    private fun writeIndex(context: Context, list: List<Track>) {
        runCatching {
            val tmp = File(context.filesDir, "local-index.json.tmp")
            tmp.writeText(json.encodeToString(ListSerializer(Track.serializer()), list))
            tmp.renameTo(indexFile(context))
        }
    }

    fun item(t: Track): Item = Item(
        kind = if (t.video) "video" else "song",
        id = t.path,
        title = t.title,
        subtitle = listOf(t.artist, t.album).filter { it.isNotEmpty() }.joinToString(" · "),
        durationSec = t.durationSec,
        thumb = ImageLoader.LOCAL + t.path,
    )
}
