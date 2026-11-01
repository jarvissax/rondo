package com.jarvissax.rondo.ui.screens

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.NetworkInfo
import android.net.wifi.ScanResult
import android.net.wifi.SupplicantState
import android.net.wifi.WifiConfiguration
import android.net.wifi.WifiManager
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.ui.IconView
import com.jarvissax.rondo.ui.MATCH
import com.jarvissax.rondo.ui.Palette
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.Screen
import com.jarvissax.rondo.ui.Tiles
import com.jarvissax.rondo.ui.TvKeyboard
import com.jarvissax.rondo.ui.WRAP
import com.jarvissax.rondo.ui.dp
import com.jarvissax.rondo.ui.dpf
import com.jarvissax.rondo.ui.lp
import com.jarvissax.rondo.ui.rounded
import com.jarvissax.rondo.ui.text

// Wi-Fi from the wheel. Android's own Wi-Fi screen opens but cannot be used:
// the wheel sends left/right, its list only moves with up/down, and its password box needs the
// touch keyboard. So: our list of networks, the TV keyboard for passwords.
object Wifi {
    enum class Security { OPEN, WEP, PSK, EAP }

    fun manager(c: Context) = c.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager

    fun security(r: ScanResult): Security = when {
        r.capabilities.contains("EAP") -> Security.EAP
        r.capabilities.contains("PSK") -> Security.PSK
        r.capabilities.contains("WEP") -> Security.WEP
        else -> Security.OPEN
    }

    fun unquote(s: String?) = s?.removeSurrounding("\"") ?: ""

    fun saved(c: Context, ssid: String): WifiConfiguration? =
        manager(c).configuredNetworks?.firstOrNull { unquote(it.SSID) == ssid }

    // The hidden WifiManager.connect()/forget() are what Android's own screen uses: they raise the
    // network's priority (so it is chosen again after a reboot) and keep the other saved networks
    // enabled. The public fallback (enableNetwork with disableOthers) disables the others.
    private val listenerClass by lazy { runCatching { Class.forName("android.net.wifi.WifiManager\$ActionListener") }.getOrNull() }

    fun connect(c: Context, netId: Int) {
        val wm = manager(c)
        val ok = runCatching {
            WifiManager::class.java.getMethod("connect", Int::class.javaPrimitiveType, listenerClass).invoke(wm, netId, null)
        }.isSuccess
        if (!ok) {
            wm.enableNetwork(netId, true)
            wm.saveConfiguration()
            wm.reconnect()
        }
        Log.i("Y1", "wifi: connect #$netId (${if (ok) "connect()" else "enableNetwork"})")
    }

    fun join(c: Context, ssid: String, security: Security, password: String) {
        val config = WifiConfiguration().apply {
            SSID = "\"$ssid\""
            when (security) {
                Security.PSK -> {
                    allowedKeyManagement.set(WifiConfiguration.KeyMgmt.WPA_PSK)
                    preSharedKey = "\"$password\""
                }
                Security.WEP -> {
                    allowedKeyManagement.set(WifiConfiguration.KeyMgmt.NONE)
                    allowedAuthAlgorithms.set(WifiConfiguration.AuthAlgorithm.OPEN)
                    allowedAuthAlgorithms.set(WifiConfiguration.AuthAlgorithm.SHARED)
                    // 10/26 hex digits are a raw key, anything else is an ASCII key
                    wepKeys[0] = if (password.matches(Regex("[0-9A-Fa-f]{10}|[0-9A-Fa-f]{26}"))) password else "\"$password\""
                    wepTxKeyIndex = 0
                }
                else -> allowedKeyManagement.set(WifiConfiguration.KeyMgmt.NONE)
            }
        }
        val wm = manager(c)
        saved(c, ssid)?.let { wm.removeNetwork(it.networkId) } // a new password replaces the old entry
        val ok = runCatching {
            WifiManager::class.java.getMethod("connect", WifiConfiguration::class.java, listenerClass).invoke(wm, config, null)
        }.isSuccess
        if (!ok) {
            val id = wm.addNetwork(config)
            if (id >= 0) connect(c, id)
        }
        Log.i("Y1", "wifi: join ${security.name} network (${if (ok) "connect()" else "addNetwork"})")
    }

    fun forget(c: Context, netId: Int) {
        val wm = manager(c)
        val ok = runCatching {
            WifiManager::class.java.getMethod("forget", Int::class.javaPrimitiveType, listenerClass).invoke(wm, netId, null)
        }.isSuccess
        if (!ok) {
            wm.removeNetwork(netId)
            wm.saveConfiguration()
        }
    }

    fun signal(level: Int): String = when (WifiManager.calculateSignalLevel(level, 4)) {
        3 -> "Strong"
        2 -> "Good"
        1 -> "Fair"
        else -> "Weak"
    }
}

class WifiScreen(shell: ShellActivity) : ShellListScreen(shell, "Wi-Fi") {
    private val wm = Wifi.manager(context)
    private var lastScan = 0L
    private var state: NetworkInfo.DetailedState? = null
    private var failedSsid: String? = null // wrong password on the last attempt

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            when (i.action) {
                WifiManager.NETWORK_STATE_CHANGED_ACTION -> {
                    @Suppress("DEPRECATION")
                    state = i.getParcelableExtra<NetworkInfo>(WifiManager.EXTRA_NETWORK_INFO)?.detailedState
                    if (state == NetworkInfo.DetailedState.CONNECTED) failedSsid = null
                }
                WifiManager.SUPPLICANT_STATE_CHANGED_ACTION ->
                    if (i.getIntExtra(WifiManager.EXTRA_SUPPLICANT_ERROR, 0) == WifiManager.ERROR_AUTHENTICATING) {
                        failedSsid = Wifi.unquote(wm.connectionInfo?.ssid).ifEmpty { failedSsid }
                    }
            }
            render()
        }
    }

    override fun render() {
        val enabled = wm.isWifiEnabled
        val rows = mutableListOf<Row>(
            Row.Entry("Wi-Fi", trailing = if (enabled) "On" else if (wm.wifiState == WifiManager.WIFI_STATE_ENABLING) "Turning on…" else "Off", tile = Tiles.wifi, onSelect = {
                wm.isWifiEnabled = !enabled
                render()
            }),
        )
        if (enabled) {
            val info = wm.connectionInfo
            val current = Wifi.unquote(info?.ssid).takeIf { info?.networkId != -1 }
            val saved = wm.configuredNetworks.orEmpty().associateBy { Wifi.unquote(it.SSID) }
            // one row per name, strongest first; MediaTek's fake "NVRAM WARNING" network hidden
            val networks = wm.scanResults.orEmpty()
                .filter { it.SSID.isNotBlank() && !it.SSID.startsWith("NVRAM WARNING") }
                .groupBy { it.SSID }.map { (_, list) -> list.maxByOrNull { it.level }!! }
                .sortedWith(compareByDescending<ScanResult> { it.SSID == current }.thenByDescending { it.level })
            rows += Row.Header(if (networks.isEmpty()) "Searching…" else "Choose a network")
            networks.forEach { r ->
                val security = Wifi.security(r)
                val config = saved[r.SSID]
                val isCurrent = r.SSID == current
                val status = when {
                    isCurrent -> when (state) {
                        NetworkInfo.DetailedState.CONNECTED, null -> if (info?.supplicantState == SupplicantState.COMPLETED) "Connected" else "Connecting…"
                        NetworkInfo.DetailedState.OBTAINING_IPADDR -> "Getting address…"
                        NetworkInfo.DetailedState.AUTHENTICATING -> "Checking password…"
                        else -> "Connecting…"
                    }
                    r.SSID == failedSsid -> "Wrong password"
                    config != null -> "Saved"
                    security == Wifi.Security.OPEN -> "Open"
                    security == Wifi.Security.EAP -> "Enterprise (not supported)"
                    else -> "Secured"
                }
                rows += Row.Entry(
                    title = r.SSID,
                    subtitle = status,
                    trailing = Wifi.signal(r.level),
                    chevron = config != null,
                    enabled = security != Wifi.Security.EAP,
                    onSelect = { choose(r, security, config, isCurrent) },
                    onLongSelect = config?.let { { shell.push(WifiNetworkScreen(shell, r.SSID)) } },
                )
            }
        }
        list.setRows(rows, keepSelection = true)
    }

    private fun choose(r: ScanResult, security: Wifi.Security, config: WifiConfiguration?, isCurrent: Boolean) {
        when {
            isCurrent -> shell.push(WifiNetworkScreen(shell, r.SSID))
            // a saved network whose password just failed asks for the password again
            config != null && r.SSID != failedSsid -> Wifi.connect(context, config.networkId)
            security == Wifi.Security.OPEN -> Wifi.join(context, r.SSID, security, "")
            else -> shell.push(WifiPasswordScreen(shell, r.SSID, security))
        }
        render()
    }

    override fun onShow() {
        context.registerReceiver(receiver, IntentFilter().apply {
            addAction(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION)
            addAction(WifiManager.NETWORK_STATE_CHANGED_ACTION)
            addAction(WifiManager.SUPPLICANT_STATE_CHANGED_ACTION)
            addAction(WifiManager.WIFI_STATE_CHANGED_ACTION)
        })
        scan()
        render()
    }

    override fun onHide() {
        runCatching { context.unregisterReceiver(receiver) }
    }

    private fun scan() {
        if (wm.isWifiEnabled) wm.startScan()
        lastScan = SystemClock.uptimeMillis()
    }

    // a fresh scan every 15 s while the list is open
    override fun onTick() {
        super.onTick()
        if (SystemClock.uptimeMillis() - lastScan > 15_000) scan()
    }
}

// A saved network: what it is, and Forget.
class WifiNetworkScreen(shell: ShellActivity, private val ssid: String) : ShellListScreen(shell, ssid) {
    override fun render() {
        val wm = Wifi.manager(context)
        val info = wm.connectionInfo
        val connected = Wifi.unquote(info?.ssid) == ssid && info?.networkId != -1
        val config = Wifi.saved(context, ssid)
        val rows = mutableListOf<Row>()
        if (connected && info != null) {
            val ip = info.ipAddress
            rows += Row.Entry("Status", trailing = "Connected", enabled = false)
            rows += Row.Entry("Signal", trailing = Wifi.signal(info.rssi), enabled = false)
            rows += Row.Entry("Speed", trailing = "${info.linkSpeed} Mb/s", enabled = false)
            rows += Row.Entry("IP address", trailing = "${ip and 0xff}.${ip shr 8 and 0xff}.${ip shr 16 and 0xff}.${ip shr 24 and 0xff}", enabled = false)
        } else if (config != null) {
            rows += Row.Entry("Connect", onSelect = {
                Wifi.connect(context, config.networkId)
                shell.pop()
            })
        }
        if (config != null) {
            rows += Row.Entry("Forget this network", subtitle = "The password will be needed again", onSelect = {
                Wifi.forget(context, config.networkId)
                shell.pop()
            })
        }
        list.setRows(rows)
    }
}

// Password entry on the TV keyboard. The password is shown as typed: there is nobody looking
// over the shoulder of a 2.4" screen, and typos are hard to spot otherwise.
class WifiPasswordScreen(shell: ShellActivity, private val ssid: String, private val security: Wifi.Security) : Screen(shell) {
    private val password = StringBuilder()
    private lateinit var field: TextView
    private lateinit var keyboard: TvKeyboard

    override fun build(): View {
        val c = context
        val root = LinearLayout(c).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Palette.dark)
        }
        root.addView(c.darkHeader("Password for $ssid", IconView(c, IconView.Kind.WIFI, Palette.white)), lp(MATCH, c.dp(28)))
        val bar = LinearLayout(c).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(c.dp(10), 0, c.dp(10), 0)
            background = rounded(Palette.darkRaised, c.dpf(14f))
        }
        field = c.text(14, Palette.white)
        bar.addView(field, lp(0, WRAP, 1f))
        root.addView(bar, lp(MATCH, c.dp(28)).apply { setMargins(c.dp(8), c.dp(2), c.dp(8), c.dp(5)) })
        keyboard = TvKeyboard(c, actionLabel = "Join")
        keyboard.onKey = { onKey(it) }
        root.addView(keyboard, lp(MATCH, 0, 1f).apply { setMargins(c.dp(5), 0, c.dp(5), 0) })
        root.addView(c.text(9, Palette.darkMuted).apply {
            text = "ABC: capitals · #+=: symbols · Menu up · Play down · Prev/Next left/right"
            gravity = Gravity.CENTER
        }, lp(MATCH, WRAP).apply { setMargins(0, c.dp(3), 0, c.dp(4)) })
        render()
        return root
    }

    private fun onKey(value: String) {
        when (value) {
            TvKeyboard.DELETE -> if (password.isNotEmpty()) password.setLength(password.length - 1)
            TvKeyboard.SEARCH -> {
                val minimum = if (security == Wifi.Security.PSK) 8 else 1
                if (password.length < minimum) {
                    field.text = "At least $minimum characters"
                    return
                }
                Wifi.join(context, ssid, security, password.toString())
                shell.pop()
                return
            }
            else -> if (password.length < 63) password.append(value)
        }
        render()
    }

    private fun render() {
        field.text = "$password|"
    }

    override fun onAction(action: WheelAction, steps: Int): Boolean = keyboard.handle(action)
}
