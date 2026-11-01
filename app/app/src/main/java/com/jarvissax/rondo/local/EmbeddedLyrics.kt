package com.jarvissax.rondo.local

import java.io.RandomAccessFile
import java.nio.charset.Charset

// Lyrics stored inside a song file (many ripped FLACs carry them; 47 of the
// first 60 on the test card did). Android 4.2's MediaMetadataRetriever does not expose them, so the
// tags are read here: FLAC Vorbis comments (LYRICS / UNSYNCEDLYRICS / SYNCEDLYRICS), MP4 ©lyr,
// ID3v2 USLT. The text may itself be LRC (timed); the caller decides.
object EmbeddedLyrics {
    fun read(path: String): String? = runCatching {
        when (path.substringAfterLast('.').lowercase()) {
            "flac" -> flac(path)
            "m4a", "mp4", "aac" -> mp4(path)
            "mp3" -> id3(path)
            else -> null
        }
    }.getOrNull()?.trim()?.takeIf { it.isNotEmpty() }

    private val lrcStamp = Regex("\\[\\d{1,3}:\\d{1,2}([.:]\\d{1,3})?]")

    private fun flac(path: String): String? = RandomAccessFile(path, "r").use { f ->
        val magic = ByteArray(4).also { f.readFully(it) }
        if (String(magic, Charsets.US_ASCII) != "fLaC") return null
        var last = false
        while (!last) {
            val header = f.readInt()
            last = header ushr 31 == 1
            val type = (header ushr 24) and 0x7f
            val length = header and 0xffffff
            if (type == 4) return vorbis(ByteArray(length).also { f.readFully(it) })
            f.seek(f.filePointer + length) // STREAMINFO, PICTURE (covers can be large), …
        }
        null
    }

    private fun le32(b: ByteArray, at: Int) = (b[at].toInt() and 0xff) or ((b[at + 1].toInt() and 0xff) shl 8) or
        ((b[at + 2].toInt() and 0xff) shl 16) or ((b[at + 3].toInt() and 0xff) shl 24)

    private fun vorbis(b: ByteArray): String? {
        var at = 4 + le32(b, 0) // vendor string
        val count = le32(b, at)
        at += 4
        val found = mutableListOf<String>()
        repeat(count) {
            val len = le32(b, at)
            at += 4
            val comment = String(b, at, len, Charsets.UTF_8)
            at += len
            val key = comment.substringBefore('=').uppercase().replace(" ", "")
            if (key == "LYRICS" || key == "UNSYNCEDLYRICS" || key == "SYNCEDLYRICS" || key.startsWith("LYRICS-")) found += comment.substringAfter('=')
        }
        // a timed version wins over a plain one
        return found.firstOrNull { lrcStamp.containsMatchIn(it) } ?: found.firstOrNull()
    }

    // MP4: moov/udta/meta/ilst/©lyr/data
    private fun mp4(path: String): String? = RandomAccessFile(path, "r").use { f ->
        val moov = atom(f, 0, f.length(), "moov") ?: return null
        val udta = atom(f, moov.first, moov.second, "udta") ?: return null
        val meta = atom(f, udta.first, udta.second, "meta") ?: return null
        val ilst = atom(f, meta.first + 4, meta.second, "ilst") ?: return null // meta is a full box
        val lyr = atom(f, ilst.first, ilst.second, "©lyr") ?: return null
        val data = atom(f, lyr.first, lyr.second, "data") ?: return null
        val bytes = ByteArray((data.second - data.first - 8).toInt().coerceAtLeast(0))
        f.seek(data.first + 8) // type + locale
        f.readFully(bytes)
        String(bytes, Charsets.UTF_8)
    }

    // the body (start, end) of the first child atom named `name` between start and end
    private fun atom(f: RandomAccessFile, start: Long, end: Long, name: String): Pair<Long, Long>? {
        var at = start
        val type = ByteArray(4)
        while (at + 8 <= end) {
            f.seek(at)
            var size = f.readInt().toLong() and 0xffffffffL
            f.readFully(type)
            var header = 8L
            if (size == 1L) {
                size = f.readLong()
                header = 16
            } else if (size == 0L) {
                size = end - at
            }
            if (size < header) return null
            if (String(type, Charsets.ISO_8859_1) == name) return (at + header) to (at + size)
            at += size
        }
        return null
    }

    // ID3v2.3/2.4 USLT frame
    private fun id3(path: String): String? = RandomAccessFile(path, "r").use { f ->
        val h = ByteArray(10).also { f.readFully(it) }
        if (String(h, 0, 3, Charsets.ISO_8859_1) != "ID3") return null
        val major = h[3].toInt()
        val size = syncsafe(h, 6)
        val tag = ByteArray(size).also { f.readFully(it) }
        var at = 0
        while (at + 10 <= tag.size) {
            val id = String(tag, at, 4, Charsets.ISO_8859_1)
            if (id[0] == '\u0000') break
            val len = if (major >= 4) syncsafe(tag, at + 4) else
                ((tag[at + 4].toInt() and 0xff) shl 24) or ((tag[at + 5].toInt() and 0xff) shl 16) or ((tag[at + 6].toInt() and 0xff) shl 8) or (tag[at + 7].toInt() and 0xff)
            if (len <= 0 || at + 10 + len > tag.size) break
            if (id == "USLT") return uslt(tag.copyOfRange(at + 10, at + 10 + len))
            at += 10 + len
        }
        null
    }

    private fun syncsafe(b: ByteArray, at: Int) =
        ((b[at].toInt() and 0x7f) shl 21) or ((b[at + 1].toInt() and 0x7f) shl 14) or ((b[at + 2].toInt() and 0x7f) shl 7) or (b[at + 3].toInt() and 0x7f)

    // encoding, 3-byte language, descriptor (terminated), text
    private fun uslt(b: ByteArray): String? {
        val (charset, wide) = when (b[0].toInt()) {
            1 -> Charset.forName("UTF-16") to true
            2 -> Charset.forName("UTF-16BE") to true
            3 -> Charsets.UTF_8 to false
            else -> Charsets.ISO_8859_1 to false
        }
        var at = 4
        if (wide) {
            while (at + 1 < b.size && !(b[at].toInt() == 0 && b[at + 1].toInt() == 0)) at += 2
            at += 2
        } else {
            while (at < b.size && b[at].toInt() != 0) at++
            at++
        }
        if (at >= b.size) return null
        return String(b, at, b.size - at, charset)
    }
}
