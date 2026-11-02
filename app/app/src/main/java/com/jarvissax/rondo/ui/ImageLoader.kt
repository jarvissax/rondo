package com.jarvissax.rondo.ui

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaMetadataRetriever
import android.util.LruCache
import android.widget.ImageView
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.Request
import com.jarvissax.rondo.R
import com.jarvissax.rondo.net.Api

// Artwork for every list and player. Online images come from the server's /thumb, already
// resized to the size they are shown at; local files use their embedded cover art (or a
// cover.jpg/folder.jpg next to them, or a frame for videos). 6 MB LRU, at most 3 loads at a
// time; a row scrolled away cancels its load, so fast wheel turns do not queue hundreds.
object ImageLoader {
    const val LOCAL = "local:" // thumb for a file on the card: "local:/storage/sdcard0/Music/x.flac"

    private val cache = object : LruCache<String, Bitmap>(6 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap) = value.rowBytes * value.height
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    private val io = Dispatchers.IO.limitedParallelism(3)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    private fun key(thumb: String, widthPx: Int, heightPx: Int?) =
        if (thumb.startsWith(LOCAL)) "$thumb#$widthPx" else "${Api.base}$thumb&w=$widthPx" + (heightPx?.let { "&h=$it" } ?: "")

    // the bitmap a view at this size already shows, if any (for colors taken from the artwork)
    fun cached(thumb: String?, widthPx: Int, heightPx: Int? = null): Bitmap? = thumb?.let { cache.get(key(it, widthPx, heightPx)) }

    fun load(view: ImageView, thumb: String?, widthPx: Int, heightPx: Int? = null, onLoaded: ((Bitmap) -> Unit)? = null) {
        val job = view.getTag(R.id.image_job) as? Job
        val key = thumb?.let { key(it, widthPx, heightPx) }
        // same image: already shown, or on its way (a rebind for the selection must not restart it)
        if (key != null && view.getTag(R.id.image_url) == key && (view.drawable != null || job?.isActive == true)) return
        job?.cancel()
        if (thumb == null || key == null) {
            view.setTag(R.id.image_url, null)
            view.setImageDrawable(null)
            return
        }
        view.setTag(R.id.image_url, key)
        cache.get(key)?.let {
            view.setImageBitmap(it)
            onLoaded?.invoke(it)
            return
        }
        view.setImageDrawable(null)
        view.setTag(R.id.image_job, scope.launch {
            val bitmap = runCatching { withContext(io) { if (thumb.startsWith(LOCAL)) local(thumb.removePrefix(LOCAL), widthPx) else fetch(key) } }.getOrNull()
                ?: return@launch
            cache.put(key, bitmap)
            if (view.getTag(R.id.image_url) == key) {
                view.setImageBitmap(bitmap)
                onLoaded?.invoke(bitmap)
            }
        })
    }

    // fetched into the cache now, so a later load() shows it at once (the next short's first frame)
    fun preload(thumb: String?, widthPx: Int, heightPx: Int? = null) {
        if (thumb == null || thumb.startsWith(LOCAL)) return
        val key = key(thumb, widthPx, heightPx)
        if (cache.get(key) != null) return
        scope.launch { runCatching { withContext(io) { fetch(key) } }.getOrNull()?.let { cache.put(key, it) } }
    }

    private fun fetch(url: String): Bitmap? =
        Api.client.newCall(Request.Builder().url(url).build()).execute().use { res ->
            if (!res.isSuccessful) return@use null
            val bytes = res.body()?.bytes() ?: return@use null
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        }

    private val videoExt = setOf("mp4", "m4v", "mkv", "3gp", "webm")

    private fun local(path: String, widthPx: Int): Bitmap? {
        val video = path.substringAfterLast('.').lowercase() in videoExt
        var frame: Bitmap? = null
        val bytes = runCatching {
            MediaMetadataRetriever().run {
                try {
                    setDataSource(path)
                    embeddedPicture ?: if (video) {
                        frame = getFrameAtTime(10_000_000)
                        null
                    } else null
                } finally {
                    release()
                }
            }
        }.getOrNull()
        if (bytes != null) return decodeSampled(widthPx) { opts -> BitmapFactory.decodeByteArray(bytes, 0, bytes.size, opts) }
        frame?.let { f ->
            if (f.width <= widthPx * 2) return f
            val scaled = Bitmap.createScaledBitmap(f, widthPx * 2, f.height * widthPx * 2 / f.width, true)
            f.recycle()
            return scaled
        }
        val dir = File(path).parentFile ?: return null
        val cover = listOf("cover.jpg", "folder.jpg", "Cover.jpg", "Folder.jpg", "front.jpg").map { File(dir, it) }.firstOrNull { it.exists() } ?: return null
        return decodeSampled(widthPx) { opts -> BitmapFactory.decodeFile(cover.path, opts) }
    }

    // decode at roughly the size shown: big embedded covers are 1000+ px
    private fun decodeSampled(widthPx: Int, decode: (BitmapFactory.Options) -> Bitmap?): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        decode(bounds)
        var sample = 1
        while (bounds.outWidth / (sample * 2) >= widthPx) sample *= 2
        return decode(BitmapFactory.Options().apply { inSampleSize = sample })
    }
}
