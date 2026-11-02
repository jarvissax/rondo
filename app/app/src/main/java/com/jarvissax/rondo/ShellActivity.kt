package com.jarvissax.rondo

import com.jarvissax.rondo.ui.screens.UsbDriveScreen
import com.jarvissax.rondo.ui.screens.UsbDrive
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.ServiceConnection
import android.media.AudioManager
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.KeyEvent
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.input.WheelHaptics
import com.jarvissax.rondo.input.WheelInputDispatcher
import com.jarvissax.rondo.player.PlayerService
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.VolumeOverlay
import com.jarvissax.rondo.ui.flp
import com.jarvissax.rondo.ui.screens.MainMenuScreen

// The whole app is one Activity hosting a back stack of Screens.
class ShellActivity : AppCompatActivity() {
    private val stack = ArrayDeque<Screen>()
    private lateinit var container: FrameLayout
    private lateinit var volumeOverlay: VolumeOverlay
    private val dispatcher = WheelInputDispatcher { action, steps -> onWheel(action, steps) }
    private val haptics by lazy { WheelHaptics(this) }
    private val handler = Handler(Looper.getMainLooper())

    var player: PlayerService? = null
        private set

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            player = (binder as PlayerService.LocalBinder).service
            stack.forEach { it.onPlayerReady() }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            player = null
        }
    }

    private val tick = object : Runnable {
        override fun run() {
            stack.lastOrNull()?.onTick()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        volumeControlStream = AudioManager.STREAM_MUSIC
        val root = FrameLayout(this)
        container = FrameLayout(this)
        volumeOverlay = VolumeOverlay(this)
        root.addView(container, flp(MATCH, MATCH))
        root.addView(volumeOverlay, flp(MATCH, MATCH))
        setContentView(root)
        // no keepScreenOn here: the screen sleeps on the system timeout (battery); only the video
        // player keeps it on

        val intent = Intent(this, PlayerService::class.java)
        startService(intent)
        bindService(intent, connection, Context.BIND_AUTO_CREATE)
        push(MainMenuScreen(this))
        registerReceiver(screenReceiver, IntentFilter(Intent.ACTION_SCREEN_OFF).apply { addAction(Intent.ACTION_SCREEN_ON) })
        registerReceiver(usbReceiver, IntentFilter(Intent.ACTION_UMS_CONNECTED).apply { addAction(Intent.ACTION_UMS_DISCONNECTED) })
        if (UsbDrive.available(this)) UsbDrive.silenceSystemScreen(this)
        dispatcher.acceleration = Config.behavior.wheelAcceleration
        haptics.level = Config.behavior.wheelVibration
        dispatcher.onPulse = { spin -> haptics.tick(spin) }
        Config.listen(configChanged)
    }

    // A computer on the cable offers the USB drive screen (firmware builds only); pulling the cable
    // gives the card back to the Y1 and closes it
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (!UsbDrive.available(context)) return
            if (intent.action == Intent.ACTION_UMS_CONNECTED) {
                if (stack.lastOrNull() !is UsbDriveScreen) push(UsbDriveScreen(this@ShellActivity))
            } else {
                Thread { if (UsbDrive.enabled(context)) UsbDrive.setEnabled(context, false) }.start()
                if (stack.lastOrNull() is UsbDriveScreen) pop()
            }
        }
    }

    // the dashboard changed something: a new theme redraws every screen in the stack in place
    private val configChanged: (Config.Change) -> Unit = { change ->
        when (change) {
            Config.Change.THEME -> rebuildScreens()
            Config.Change.BEHAVIOR -> {
                dispatcher.acceleration = Config.behavior.wheelAcceleration
                haptics.level = Config.behavior.wheelVibration
            }
            else -> Unit
        }
    }

    private fun rebuildScreens() {
        stack.forEach { it.create() }
        container.removeAllViews()
        stack.lastOrNull()?.let {
            container.addView(it.view, flp(MATCH, MATCH))
            it.onTick()
        }
        Log.i("Y1", "theme applied: ${Config.theme.accent} ${Config.theme.appearance} ${Config.theme.textScale}")
    }

    // HOME (a reboot, or leaving another app) lands on the main menu, like any launcher
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        if (intent.hasCategory(Intent.CATEGORY_HOME)) while (stack.size > 1) pop()
    }

    override fun onResume() {
        super.onResume()
        awakeAt = SystemClock.uptimeMillis()
        handler.post(tick)
    }

    override fun onPause() {
        handler.removeCallbacks(tick)
        dispatcher.cancel()
        super.onPause()
    }

    override fun onDestroy() {
        stack.forEach { it.onDestroy() }
        Config.unlisten(configChanged)
        unregisterReceiver(screenReceiver)
        unregisterReceiver(usbReceiver)
        unbindService(connection)
        super.onDestroy()
    }

    // A press that wakes the screen only wakes it (otherwise center with the screen off
    // would open the scrubber). The wheel, center and Menu are swallowed from screen-off until 600 ms
    // after it comes back on; Play/Pause and Next/Prev still work blind, like an iPod.
    private var asleep = false
    private var awakeAt = 0L
    private val swallowing = HashSet<Int>()

    private val screenReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == Intent.ACTION_SCREEN_OFF) {
                asleep = true
                dispatcher.cancel()
            } else {
                asleep = false
                awakeAt = SystemClock.uptimeMillis()
            }
        }
    }

    private fun isNavigation(code: Int) = when (code) {
        KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN,
        KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_BACK -> true
        else -> false
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val code = event.keyCode
        if (isNavigation(code)) {
            if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0 && (asleep || event.downTime - awakeAt < 600)) {
                swallowing += code
                return true
            }
            if (code in swallowing) {
                if (event.action == KeyEvent.ACTION_UP) swallowing -= code
                return true
            }
        }
        return dispatcher.onKeyEvent(event) || super.dispatchKeyEvent(event)
    }

    private fun onWheel(action: WheelAction, steps: Int) {
        if (stack.lastOrNull()?.onAction(action, steps) == true) return
        val p = player
        when (action) {
            WheelAction.Back -> pop()
            WheelAction.PlayPause -> p?.togglePlay()
            WheelAction.Next -> p?.next()
            WheelAction.Prev -> p?.previous()
            // held Next/Prev on any other screen: 2 % of the length per repeat (at least 10 s)
            WheelAction.LongNext -> p?.let { it.seekBy(maxOf(10_000, it.durationMs() / 50)) }
            WheelAction.LongPrev -> p?.let { it.seekBy(-maxOf(10_000, it.durationMs() / 50)) }
            else -> Unit
        }
    }

    fun push(screen: Screen) {
        stack.lastOrNull()?.onHide()
        container.removeAllViews()
        container.addView(screen.create(), flp(MATCH, MATCH))
        stack.addLast(screen)
        screen.onShow()
        if (player != null) screen.onPlayerReady()
        Log.i("Y1", "screen -> ${screen.javaClass.simpleName} (depth ${stack.size})")
    }

    // back to the nearest screen of this kind (the screens above it close)
    fun popTo(kind: Class<out Screen>) {
        while (stack.size > 1 && !kind.isInstance(stack.last())) pop()
    }

    // the main menu is the bottom of the stack and never closes: this is the launcher
    fun pop() {
        if (stack.size <= 1) return
        val gone = stack.removeLast()
        gone.onHide()
        gone.onDestroy()
        val top = stack.last()
        container.removeAllViews()
        container.addView(top.view, flp(MATCH, MATCH))
        top.onShow()
    }

    // one step of STREAM_MUSIC per wheel detent, shown in our own overlay
    fun changeVolume(direction: Int) {
        val am = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        // the radio has its own volume (player/FmRadio.kt)
        val stream = if (com.jarvissax.rondo.player.FmRadio.state.on) com.jarvissax.rondo.player.FmRadio.STREAM_FM else AudioManager.STREAM_MUSIC
        am.adjustStreamVolume(stream, if (direction > 0) AudioManager.ADJUST_RAISE else AudioManager.ADJUST_LOWER, 0)
        volumeOverlay.show(am.getStreamVolume(stream), am.getStreamMaxVolume(stream))
    }
}
