package com.jarvissax.rondo.player

import android.util.LruCache
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import com.jarvissax.rondo.local.EmbeddedLyrics
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.net.Item
import com.jarvissax.rondo.net.LyricLine
import com.jarvissax.rondo.net.Lyrics

// Where the lyrics of a song come from: for songs on the card an .lrc file next to it or the
// lyrics in its tags (offline), for YouTube Music songs the server's /lyrics.
object LyricsSource {
    private val cache = LruCache<String, Lyrics>(24)

    // Songs on the card use only their own lyrics, never the internet: an .lrc
    // next to the file or the lyrics in its tags, timed preferred. YouTube Music songs ask the
    // server, which uses YouTube Music's own lyrics first.
    suspend fun find(item: Item): Lyrics {
        cache.get(item.id)?.let { return it }
        val lyrics = if (item.id.startsWith("/")) {
            fromFile(item.id) ?: Lyrics()
        } else {
            Api.lyrics(
                title = item.title,
                artist = item.subtitle.split(" · ").firstOrNull().orEmpty(),
                album = null,
                durationSec = item.durationSec,
                id = item.id,
            )
        }
        cache.put(item.id, lyrics)
        return lyrics
    }

    private suspend fun fromFile(path: String): Lyrics? = withContext(Dispatchers.IO) {
        val base = path.substringBeforeLast('.')
        val sidecar = listOf("$base.lrc", "$base.LRC").map(::File).firstOrNull { it.isFile }
        val lrc = sidecar?.let { runCatching { it.readText() }.getOrNull() }
        val tag = EmbeddedLyrics.read(path)
        // timed beats plain, wherever it is
        listOfNotNull(lrc, tag).map { text -> parseLrc(text) to text }.let { found ->
            found.firstOrNull { it.first.isNotEmpty() }?.let { return@withContext Lyrics(synced = it.first, source = "file") }
            found.firstOrNull()?.let { Lyrics(plain = it.second, source = "file") }
        }
    }

    private val stamp = Regex("\\[(\\d{1,3}):(\\d{1,2})(?:[.:](\\d{1,3}))?]")
    private val offsetTag = Regex("\\[offset:\\s*([+-]?\\d+)\\s*]", RegexOption.IGNORE_CASE)

    // [mm:ss], [mm:ss.xx], [mm:ss.xxx], several stamps per line; [offset:+/-ms] applied
    fun parseLrc(text: String): List<LyricLine> {
        val offset = offsetTag.find(text)?.groupValues?.get(1)?.toLongOrNull() ?: 0L
        val out = mutableListOf<LyricLine>()
        text.lineSequence().forEach { raw ->
            val stamps = stamp.findAll(raw).toList()
            if (stamps.isEmpty() || stamps.first().range.first != raw.indexOfFirst { !it.isWhitespace() }) return@forEach
            val words = raw.substring(stamps.last().range.last + 1).trim()
            stamps.forEach { m ->
                val (min, sec, frac) = m.destructured
                val ms = when (frac.length) {
                    0 -> 0L
                    1 -> frac.toLong() * 100
                    2 -> frac.toLong() * 10
                    else -> frac.toLong()
                }
                out += LyricLine(((min.toLong() * 60 + sec.toLong()) * 1000 + ms - offset).coerceAtLeast(0), words)
            }
        }
        return out.sortedBy { it.t }
    }
}
