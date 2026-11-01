package com.jarvissax.rondo

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.IBinder
import android.util.Log
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.exoplayer2.Player
import com.google.android.exoplayer2.ui.StyledPlayerView
import com.google.android.exoplayer2.video.VideoSize
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import com.jarvissax.rondo.net.Api
import com.jarvissax.rondo.player.PlayerService
import com.jarvissax.rondo.player.TimestampFix
import com.jarvissax.rondo.player.TimestampMode

// Playback test screen for development: play a video or its audio by ID and switch Auto/720/480/360 mid-playback.
// The wheel moves focus along the button row (DPAD_LEFT/RIGHT), center (ENTER) presses.
// For scripted tests: adb shell am start -n com.jarvissax.rondo/.MainActivity --es id <id> --es mode video|audio --es quality auto|720|480|360
class MainActivity : AppCompatActivity() {
    private var service: PlayerService? = null
    private var pendingIntent: Intent? = null
    private lateinit var playerView: StyledPlayerView
    private lateinit var rendition: TextView
    private lateinit var status: TextView
    private lateinit var idField: EditText

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val s = (binder as PlayerService.LocalBinder).service
            service = s
            playerView.player = s.player
            s.player.addListener(renditionListener)
            showRendition()
            pendingIntent?.let { handleExtras(it) }
            pendingIntent = null
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            service = null
        }
    }

    private val renditionListener = object : Player.Listener {
        override fun onVideoSizeChanged(videoSize: VideoSize) = showRendition()
        override fun onIsPlayingChanged(isPlaying: Boolean) = showRendition()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        playerView = findViewById(R.id.player_view)
        rendition = findViewById(R.id.rendition)
        status = findViewById(R.id.status)
        idField = findViewById(R.id.video_id)

        findViewById<Button>(R.id.play_video).setOnClickListener { play(PlayerService.Mode.VIDEO) }
        findViewById<Button>(R.id.play_audio).setOnClickListener { play(PlayerService.Mode.AUDIO) }
        findViewById<Button>(R.id.q_auto).setOnClickListener { service?.setQuality(null) }
        findViewById<Button>(R.id.q_720).setOnClickListener { service?.setQuality(720) }
        findViewById<Button>(R.id.q_480).setOnClickListener { service?.setQuality(480) }
        findViewById<Button>(R.id.q_360).setOnClickListener { service?.setQuality(360) }

        val intent = Intent(this, PlayerService::class.java)
        startService(intent)
        bindService(intent, connection, Context.BIND_AUTO_CREATE)
        pendingIntent = getIntent()

        checkServer()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        if (service != null) handleExtras(intent) else pendingIntent = intent
    }

    override fun onDestroy() {
        service?.player?.removeListener(renditionListener)
        playerView.player = null
        unbindService(connection)
        super.onDestroy()
    }

    private fun handleExtras(intent: Intent) {
        // decoder timestamp handling for A/B tests: --es ts raw|reorder (applies from the next play)
        when (intent.getStringExtra("ts")) {
            "raw" -> TimestampFix.mode = TimestampMode.RAW
            "reorder" -> TimestampFix.mode = TimestampMode.REORDER
        }
        intent.getStringExtra("id")?.let { idField.setText(it) }
        when (intent.getStringExtra("mode")) {
            "video" -> play(PlayerService.Mode.VIDEO)
            "audio" -> play(PlayerService.Mode.AUDIO)
            "v18" -> play(PlayerService.Mode.PROGRESSIVE)
        }
        when (val q = intent.getStringExtra("quality")) {
            null -> Unit
            "auto" -> service?.setQuality(null)
            else -> q.toIntOrNull()?.let { service?.setQuality(it) }
        }
    }

    private fun play(mode: PlayerService.Mode) {
        val id = idField.text.toString().trim()
        service?.play(id, mode)
    }

    private fun showRendition() {
        val p = service?.player ?: return
        val f = p.videoFormat
        rendition.text = if (f != null) "${f.height}p · ${f.bitrate / 1000} kbps" else if (p.isPlaying) "audio" else ""
    }

    // the server answers before anything plays: show it, retrying through hotspot drops
    private fun checkServer() = lifecycleScope.launch {
        repeat(5) { attempt ->
            try {
                val h = Api.health()
                val t = Api.time()
                val skewS = (t.epochMs - System.currentTimeMillis()) / 1000
                "server ok · yt-dlp ${h.ytdlp} · clock skew ${skewS}s".also {
                    status.text = it
                    Log.i("Y1", it)
                }
                return@launch
            } catch (e: Exception) {
                status.text = "server check failed (${attempt + 1}/5): $e"
                Log.w("Y1", "server check failed (${attempt + 1}/5): $e")
                delay(3_000)
            }
        }
    }
}
