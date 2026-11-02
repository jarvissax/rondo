package com.jarvissax.rondo.local

import android.util.Log
import java.io.File

// Playlists of songs on the card, made on the Rondo or its dashboard. Each is an .m3u8 file in the
// card's Playlists folder: they survive reflashing the Y1, and music players on a computer open
// them too (while the card is a USB drive). Songs are written relative to that folder
// ("../Music/Artist - Title.flac"), the way players expect; absolute paths and Windows separators
// are read as well. The file name is the playlist's name.
object Playlists {
    data class Playlist(val name: String, val paths: List<String>)

    class Problem(message: String) : Exception(message)

    private const val CARD = "/storage/sdcard0"
    private val dir = File("$CARD/Playlists")
    private const val EXT = ".m3u8"
    private const val MAX_NAME = 60

    @Synchronized
    fun all(): List<Playlist> = (dir.listFiles() ?: emptyArray())
        .filter { it.isFile && (it.name.endsWith(EXT, true) || it.name.endsWith(".m3u", true)) && !it.name.startsWith(".") }
        .map { Playlist(it.name.substringBeforeLast('.'), read(it)) }
        .sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })

    @Synchronized
    fun get(name: String): Playlist? = fileOf(name)?.let { Playlist(it.name.substringBeforeLast('.'), read(it)) }

    // FAT can't hold / \ : * ? " < > | in a name, and a name is a file name here
    fun clean(name: String): String = name.replace(Regex("[\\\\/:*?\"<>|\\p{Cntrl}]"), " ").replace(Regex("\\s+"), " ").trim().trim('.').take(MAX_NAME)

    @Synchronized
    fun create(name: String, paths: List<String> = emptyList()): Playlist {
        val clean = clean(name)
        if (clean.isEmpty()) throw Problem("Give it a name")
        if (fileOf(clean) != null) throw Problem("There's already a playlist called “$clean”")
        if (!dir.isDirectory && !dir.mkdirs()) throw Problem("The card isn't available")
        write(File(dir, clean + EXT), paths)
        Log.i("Y1", "playlists: created “$clean” with ${paths.size} songs")
        return Playlist(clean, paths)
    }

    // the new name and/or the whole new list of songs (reordered, added to or removed from)
    @Synchronized
    fun update(name: String, newName: String? = null, paths: List<String>? = null): Playlist {
        val file = fileOf(name) ?: throw Problem("That playlist is gone")
        val songs = paths ?: read(file)
        var target = file
        if (newName != null) {
            val clean = clean(newName)
            if (clean.isEmpty()) throw Problem("Give it a name")
            val other = fileOf(clean)
            if (other != null && other != file) throw Problem("There's already a playlist called “$clean”")
            target = File(dir, clean + EXT)
        }
        when {
            target.path == file.path -> write(file, songs)
            // only the case changes: FAT sees one file, so writing first and deleting after would lose it
            target.path.equals(file.path, ignoreCase = true) -> {
                file.delete()
                write(target, songs)
            }
            else -> {
                write(target, songs)
                file.delete()
            }
        }
        return Playlist(target.name.substringBeforeLast('.'), songs)
    }

    @Synchronized
    fun add(name: String, paths: List<String>): Playlist {
        val current = get(name) ?: throw Problem("That playlist is gone")
        return update(name, paths = current.paths + paths.filter { it !in current.paths })
    }

    @Synchronized
    fun delete(name: String) {
        val file = fileOf(name) ?: return
        if (!file.delete()) throw Problem("Couldn't delete it")
        Log.i("Y1", "playlists: deleted “$name”")
    }

    // a song left the card: drop it everywhere
    @Synchronized
    fun forget(path: String) = all().filter { path in it.paths }.forEach { update(it.name, paths = it.paths - path) }

    private fun fileOf(name: String): File? {
        val clean = clean(name)
        return (dir.listFiles() ?: return null).firstOrNull { f ->
            f.isFile && f.name.substringBeforeLast('.').equals(clean, ignoreCase = true) &&
                (f.name.endsWith(EXT, true) || f.name.endsWith(".m3u", true))
        }
    }

    private fun read(file: File): List<String> = runCatching {
        file.readLines(Charsets.UTF_8).asSequence()
            .map { it.trim().removePrefix("﻿") }
            .filter { it.isNotEmpty() && !it.startsWith("#") }
            .mapNotNull(::resolve)
            .distinct()
            .toList()
    }.getOrDefault(emptyList())

    private fun resolve(entry: String): String? {
        val p = entry.replace('\\', '/')
        if (Regex("^[A-Za-z]:/").containsMatchIn(p) || p.contains("://")) return null // another computer's drive, or a URL
        return normalize(if (p.startsWith("/")) p else "${dir.path}/$p")
    }

    // "/storage/sdcard0/Playlists/../Music/x.flac" -> "/storage/sdcard0/Music/x.flac"
    private fun normalize(path: String): String {
        val out = ArrayDeque<String>()
        path.split('/').forEach { part ->
            when (part) {
                "", "." -> Unit
                ".." -> out.removeLastOrNull()
                else -> out.addLast(part)
            }
        }
        return "/" + out.joinToString("/")
    }

    private fun write(file: File, paths: List<String>) {
        val byPath = LocalLibrary.byPath()
        val text = buildString {
            append("#EXTM3U\n")
            paths.forEach { path ->
                byPath[path]?.let { t -> append("#EXTINF:${t.durationSec ?: -1},${listOf(t.artist, t.title).filter { it.isNotEmpty() }.joinToString(" - ")}\n") }
                append(if (path.startsWith("$CARD/")) "../" + path.removePrefix("$CARD/") else path).append('\n')
            }
        }
        val tmp = File(file.parentFile, ".${file.name}.tmp")
        tmp.writeText(text, Charsets.UTF_8)
        if (!tmp.renameTo(file)) {
            file.delete()
            if (!tmp.renameTo(file)) throw Problem("Couldn't save the playlist")
        }
    }
}
