package com.jarvissax.rondo.ui.screens

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color
import android.net.wifi.WifiManager
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.qrcode.QRCodeWriter
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.dashboard.DashboardServer
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.ShellStatusBar
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.rounded
import com.jarvissax.rondo.ui.show
import com.jarvissax.rondo.ui.text
import com.jarvissax.rondo.ui.visible

// Settings › Dashboard (docs/dev/dashboard-api.md): runs the dashboard server while this screen is open
// and shows how to reach it: the address, its QR code and the pairing code. The screen stays on
// (the CPU and Wi-Fi must be awake to answer); 15 minutes without a request stop the server.
class DashboardScreen(shell: ShellActivity) : Screen(shell) {
    private lateinit var status: ShellStatusBar
    private lateinit var qr: ImageView
    private lateinit var address: TextView
    private lateinit var code: TextView
    private lateinit var state: TextView
    private lateinit var details: LinearLayout
    private lateinit var message: TextView
    private var wifiLock: WifiManager.WifiLock? = null
    private var stoppedIdle = false

    override fun build(): View {
        val c = context
        val root = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Palette.shellBg)
            keepScreenOn = true
        }
        status = ShellStatusBar(c, "Dashboard")
        root.addView(status, lp(MATCH, c.dp(22)))

        val body = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(12), c.dp(8), c.dp(12), c.dp(8))
        }
        details = body
        qr = ImageView(c).apply {
            scaleType = ImageView.ScaleType.FIT_CENTER
            setBackgroundColor(Color.WHITE)
            setPadding(c.dp(4), c.dp(4), c.dp(4), c.dp(4))
        }
        body.addView(qr, lp(c.dp(112), c.dp(112)).apply { rightMargin = c.dp(14) })
        val column = LinearLayout(c).apply { orientation = LinearLayout.VERTICAL }
        column.addView(c.text(10, Palette.shellMuted, bold = true).apply { text = "OPEN ON YOUR PHONE OR COMPUTER" }, lp(MATCH, WRAP))
        address = c.text(15, Palette.shellText, bold = true)
        column.addView(address, lp(MATCH, WRAP).apply { topMargin = c.dp(3) })
        column.addView(c.text(10, Palette.shellMuted, bold = true).apply { text = "PAIRING CODE" }, lp(MATCH, WRAP).apply { topMargin = c.dp(12) })
        code = c.text(24, Palette.accent, bold = true)
        column.addView(code, lp(MATCH, WRAP).apply { topMargin = c.dp(2) })
        state = c.text(11, Palette.shellMuted, lines = 2)
        column.addView(state, lp(MATCH, WRAP).apply { topMargin = c.dp(10) })
        body.addView(column, lp(0, WRAP, 1f))
        root.addView(body, lp(MATCH, 0, 1f))

        message = c.text(14, Palette.shellText, lines = 4).apply {
            gravity = Gravity.CENTER
            setPadding(c.dp(20), 0, c.dp(20), 0)
        }
        root.addView(message, lp(MATCH, 0, 1f))
        root.addView(c.text(10, Palette.shellMuted).apply {
            text = "Keep this screen open while you use the dashboard · Menu stops it"
            gravity = Gravity.CENTER
            background = rounded(Palette.shellHeader, 0f)
            setPadding(0, c.dp(5), 0, c.dp(5))
        }, lp(MATCH, WRAP))
        render()
        return root
    }

    private fun ip(): String? {
        val wm = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val info = wm.connectionInfo ?: return null
        if (info.networkId == -1 || info.ipAddress == 0) return null
        val ip = info.ipAddress
        return "${ip and 0xff}.${ip shr 8 and 0xff}.${ip shr 16 and 0xff}.${ip shr 24 and 0xff}"
    }

    private fun render() {
        val server = running
        val ip = ip()
        val online = server != null && ip != null
        details.visible(online)
        message.visible(!online)
        if (!online) {
            message.show(
                when {
                    ip == null -> "Connect to Wi-Fi first.\nPress center for Settings › Wi-Fi."
                    stoppedIdle -> "The dashboard stopped after 15 minutes without use.\nPress center to start it again."
                    else -> "The dashboard could not start.\nPress center to try again."
                },
            )
            return
        }
        val url = "http://$ip:${DashboardServer.PORT}"
        if (address.text.toString() != url) {
            address.text = url
            qr.setImageBitmap(qrCode(url, context.dp(104)))
        }
        // 4.2 has no letterSpacing: thin spaces spread the digits
        code.show(server!!.code.chunked(3).joinToString("  ") { it.toCharArray().joinToString(" ") })
        state.show(
            when (server.browsers) {
                0 -> "Waiting for a browser…"
                1 -> "1 browser connected"
                else -> "${server.browsers} browsers connected"
            },
        )
    }

    private fun qrCode(text: String, size: Int): Bitmap {
        val matrix = QRCodeWriter().encode(text, BarcodeFormat.QR_CODE, size, size, mapOf(EncodeHintType.MARGIN to 0))
        val pixels = IntArray(matrix.width * matrix.height)
        for (y in 0 until matrix.height) for (x in 0 until matrix.width) {
            pixels[y * matrix.width + x] = if (matrix.get(x, y)) Color.BLACK else Color.WHITE
        }
        return Bitmap.createBitmap(pixels, matrix.width, matrix.height, Bitmap.Config.RGB_565)
    }

    private fun start() {
        if (running != null) return
        if (ip() == null) return render()
        stoppedIdle = false
        running = try {
            DashboardServer(shell).apply { start(5_000, false) }
        } catch (e: Exception) {
            Log.w("Y1", "dashboard: cannot start: $e")
            null
        }
        if (running != null) {
            Log.i("Y1", "dashboard: listening on ${DashboardServer.PORT}")
            val wm = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "Y1:dashboard").apply {
                setReferenceCounted(false)
                acquire()
            }
        }
        view.keepScreenOn = running != null
        render()
    }

    private fun stop() {
        running?.stop()
        running = null
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
        if (::details.isInitialized) view.keepScreenOn = false
        Log.i("Y1", "dashboard: stopped")
    }

    override fun onShow() = start()

    override fun onHide() = stop()

    override fun onDestroy() {
        stop()
        super.onDestroy()
    }

    override fun onTick() {
        status.refresh(player?.isPlaying() == true)
        val server = running
        if (server != null && SystemClock.uptimeMillis() - server.lastRequestAt > IDLE_MS) {
            stop()
            stoppedIdle = true
        }
        if (server == null && running == null && ip() != null && !stoppedIdle) start()
        render()
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean {
        if (action != WheelAction.Select) return false
        if (ip() == null) shell.push(WifiScreen(shell)) else start()
        return true
    }

    companion object {
        private const val IDLE_MS = 15 * 60_000L

        // one server for the app; it survives a redraw of this screen (theme change from the dashboard)
        private var running: DashboardServer? = null
    }
}
