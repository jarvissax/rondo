package com.jarvissax.rondo.ui.screens

import android.bluetooth.BluetoothA2dp
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothClass
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.SystemClock
import android.util.Log
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.Tiles

// Bluetooth headphones and speakers from the wheel; Android's own screen
// cannot be driven by the wheel. On API 17 pairing (createBond), A2DP connect/disconnect and
// unpairing (removeBond) are hidden APIs, called by reflection like Android's Settings does.
object Bt {
    val adapter: BluetoothAdapter? get() = BluetoothAdapter.getDefaultAdapter()
    var a2dp: BluetoothA2dp? = null
        private set
    private var asked = false

    // the A2DP profile proxy arrives asynchronously; screens re-render when it does
    fun bindA2dp(c: Context, ready: () -> Unit) {
        if (a2dp != null || asked) return ready()
        asked = true
        adapter?.getProfileProxy(c.applicationContext, object : BluetoothProfile.ServiceListener {
            override fun onServiceConnected(profile: Int, proxy: BluetoothProfile) {
                a2dp = proxy as BluetoothA2dp
                ready()
            }

            override fun onServiceDisconnected(profile: Int) {
                a2dp = null
                asked = false
            }
        }, BluetoothProfile.A2DP)
    }

    fun state(d: BluetoothDevice): Int = runCatching { a2dp?.getConnectionState(d) }.getOrNull() ?: BluetoothProfile.STATE_DISCONNECTED

    private fun call(target: Any?, name: String, d: BluetoothDevice): Boolean = runCatching {
        target!!.javaClass.getMethod(name, BluetoothDevice::class.java).invoke(target, d) as? Boolean ?: true
    }.onFailure { Log.w("Y1", "bt: $name failed: $it") }.getOrDefault(false)

    fun connect(d: BluetoothDevice): Boolean {
        // only one pair of headphones at a time
        a2dp?.connectedDevices?.filter { it != d }?.forEach { call(a2dp, "disconnect", it) }
        return call(a2dp, "connect", d)
    }

    fun disconnect(d: BluetoothDevice) = call(a2dp, "disconnect", d)

    fun pair(d: BluetoothDevice): Boolean = runCatching { d.javaClass.getMethod("createBond").invoke(d) as Boolean }.getOrDefault(false)

    fun forget(d: BluetoothDevice): Boolean = runCatching { d.javaClass.getMethod("removeBond").invoke(d) as Boolean }.getOrDefault(false)

    fun isAudio(d: BluetoothDevice) = d.bluetoothClass?.majorDeviceClass == BluetoothClass.Device.Major.AUDIO_VIDEO

    fun kind(d: BluetoothDevice): String = when (d.bluetoothClass?.deviceClass) {
        BluetoothClass.Device.AUDIO_VIDEO_HEADPHONES, BluetoothClass.Device.AUDIO_VIDEO_WEARABLE_HEADSET -> "Headphones"
        BluetoothClass.Device.AUDIO_VIDEO_HANDSFREE -> "Headset"
        BluetoothClass.Device.AUDIO_VIDEO_LOUDSPEAKER, BluetoothClass.Device.AUDIO_VIDEO_PORTABLE_AUDIO -> "Speaker"
        BluetoothClass.Device.AUDIO_VIDEO_CAR_AUDIO -> "Car audio"
        else -> if (isAudio(d)) "Audio" else "Other device"
    }

    fun name(d: BluetoothDevice): String = d.name?.takeIf { it.isNotBlank() } ?: d.address
}

// One Bluetooth visit, the Rondo's screen or the dashboard's page: the devices found while searching,
// the one being paired (connected once it bonds), the ones being connected. Searching costs battery
// and disturbs Bluetooth audio, so it runs only between start() and stop(). `changed` runs on the
// main thread after every Bluetooth event.
class BtSession(private val context: Context, private val changed: () -> Unit) {
    private val found = LinkedHashMap<String, BluetoothDevice>() // by address, guarded by itself
    @Volatile var searching = false
        private set
    @Volatile var pairing: String? = null // address being paired: connected when bonded
        private set
    private val connecting = java.util.Collections.synchronizedSet(HashSet<String>())
    @Volatile private var lastSearch = 0L
    @Volatile private var started = false

    // the switch's last request until the adapter gets there: enable()/disable() return before the
    // adapter even starts turning, so right after a tap it still says ON and the switch flipped back
    @Volatile private var target: Boolean? = null
    @Volatile private var targetAt = 0L

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            @Suppress("DEPRECATION")
            val device = i.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
            when (i.action) {
                BluetoothDevice.ACTION_FOUND -> device?.let { synchronized(found) { found[it.address] = it } }
                BluetoothAdapter.ACTION_DISCOVERY_STARTED -> searching = true
                BluetoothAdapter.ACTION_DISCOVERY_FINISHED -> searching = false
                BluetoothAdapter.ACTION_STATE_CHANGED ->
                    if (i.getIntExtra(BluetoothAdapter.EXTRA_STATE, 0) == BluetoothAdapter.STATE_ON) {
                        Bt.bindA2dp(context) { changed() }
                        search()
                    }
                BluetoothDevice.ACTION_BOND_STATE_CHANGED ->
                    if (device != null && device.address == pairing && device.bondState == BluetoothDevice.BOND_BONDED) {
                        pairing = null
                        connecting += device.address
                        Bt.connect(device)
                    } else if (device != null && device.address == pairing && device.bondState == BluetoothDevice.BOND_NONE) {
                        pairing = null // refused or timed out
                    }
                BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED ->
                    if (i.getIntExtra(BluetoothProfile.EXTRA_STATE, -1) != BluetoothProfile.STATE_CONNECTING) device?.let { connecting -= it.address }
                // headphones that ask for a PIN almost always use 0000; a numeric comparison is
                // accepted (we cannot show both screens' numbers side by side anyway)
                PAIRING_REQUEST -> if (device != null) answerPairing(device, i)
            }
            changed()
        }
    }

    private fun answerPairing(d: BluetoothDevice, i: Intent) {
        val variant = i.getIntExtra("android.bluetooth.device.extra.PAIRING_VARIANT", -1)
        runCatching {
            when (variant) {
                0 -> d.javaClass.getMethod("setPin", ByteArray::class.java).invoke(d, "0000".toByteArray()) // PAIRING_VARIANT_PIN
                2, 3 -> d.javaClass.getMethod("setPairingConfirmation", Boolean::class.javaPrimitiveType).invoke(d, true) // passkey confirmation / consent
            }
        }.onFailure { Log.w("Y1", "bt: pairing answer failed: $it") }
    }

    fun start() {
        if (started) return
        started = true
        context.registerReceiver(receiver, IntentFilter().apply {
            addAction(BluetoothDevice.ACTION_FOUND)
            addAction(BluetoothAdapter.ACTION_DISCOVERY_STARTED)
            addAction(BluetoothAdapter.ACTION_DISCOVERY_FINISHED)
            addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
            addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
            addAction(BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED)
            addAction(PAIRING_REQUEST)
            priority = IntentFilter.SYSTEM_HIGH_PRIORITY - 1
        })
        Bt.bindA2dp(context) { changed() }
        search()
    }

    fun stop() {
        if (!started) return
        started = false
        runCatching { context.unregisterReceiver(receiver) }
        Bt.adapter?.cancelDiscovery()
    }

    fun search() {
        val bt = Bt.adapter ?: return
        if (!bt.isEnabled || bt.isDiscovering || pairing != null) return
        bt.startDiscovery()
        lastSearch = SystemClock.uptimeMillis()
    }

    // a fresh search every 20 s while nothing is being paired
    fun searchAgainIfIdle() {
        if (!searching && SystemClock.uptimeMillis() - lastSearch > 20_000) search()
    }

    fun power(on: Boolean) {
        val bt = Bt.adapter ?: return
        target = on
        targetAt = SystemClock.uptimeMillis()
        if (on) bt.enable() else bt.disable()
    }

    // BluetoothAdapter.STATE_*, with a requested change shown as turning until it happens (15 s at most)
    fun state(): Int {
        val s = Bt.adapter?.state ?: return BluetoothAdapter.STATE_OFF
        val t = target ?: return s
        val reached = if (t) s == BluetoothAdapter.STATE_ON else s == BluetoothAdapter.STATE_OFF
        if (reached || SystemClock.uptimeMillis() - targetAt > 15_000) {
            target = null
            return s
        }
        return if (t) BluetoothAdapter.STATE_TURNING_ON else BluetoothAdapter.STATE_TURNING_OFF
    }

    fun bonded(): List<BluetoothDevice> = Bt.adapter?.bondedDevices.orEmpty().sortedByDescending { Bt.state(it) }

    fun others(): List<BluetoothDevice> = synchronized(found) { found.values.toList() }
        .filter { it.bondState != BluetoothDevice.BOND_BONDED }
        .sortedWith(compareByDescending<BluetoothDevice> { Bt.isAudio(it) }.thenBy { Bt.name(it).lowercase() })

    fun isConnecting(d: BluetoothDevice) = Bt.state(d) == BluetoothProfile.STATE_CONNECTING || d.address in connecting

    fun isPairing(d: BluetoothDevice) = d.address == pairing || d.bondState == BluetoothDevice.BOND_BONDING

    fun connect(d: BluetoothDevice) {
        Bt.adapter?.let { if (it.isDiscovering) it.cancelDiscovery() } // discovery stalls connecting
        connecting += d.address
        if (!Bt.connect(d)) connecting -= d.address
    }

    fun disconnect(d: BluetoothDevice) {
        Bt.disconnect(d)
    }

    fun pair(d: BluetoothDevice) {
        Bt.adapter?.cancelDiscovery()
        pairing = d.address
        if (!Bt.pair(d)) pairing = null
    }

    fun forget(d: BluetoothDevice) {
        Bt.forget(d)
    }

    companion object {
        const val PAIRING_REQUEST = "android.bluetooth.device.action.PAIRING_REQUEST"
    }
}

class BluetoothScreen(shell: ShellActivity) : ShellListScreen(shell, "Bluetooth") {
    private val session = BtSession(shell.applicationContext) { render() }

    override fun render() {
        Bt.adapter ?: return list.showMessage("This Y1 has no Bluetooth")
        val state = session.state()
        val on = state == BluetoothAdapter.STATE_ON
        val rows = mutableListOf<Row>(
            Row.Entry("Bluetooth", tile = Tiles.bluetooth, trailing = when (state) {
                BluetoothAdapter.STATE_TURNING_ON -> "Turning on…"
                BluetoothAdapter.STATE_TURNING_OFF -> "Turning off…"
                else -> if (on) "On" else "Off"
            }, onSelect = {
                // a press while it is still turning is ignored
                if (state == BluetoothAdapter.STATE_ON || state == BluetoothAdapter.STATE_OFF) session.power(!on)
                render()
            }),
        )
        if (on) {
            val bonded = session.bonded()
            if (bonded.isNotEmpty()) rows += Row.Header("My devices")
            bonded.forEach { d ->
                val state = Bt.state(d)
                rows += Row.Entry(
                    title = Bt.name(d),
                    subtitle = when {
                        state == BluetoothProfile.STATE_CONNECTED -> "Connected · music plays here"
                        session.isConnecting(d) -> "Connecting…"
                        Bt.isAudio(d) -> "${Bt.kind(d)} · not connected"
                        else -> Bt.kind(d)
                    },
                    onSelect = {
                        if (state == BluetoothProfile.STATE_CONNECTED) session.disconnect(d) else session.connect(d)
                        render()
                    },
                    onLongSelect = { shell.push(BluetoothDeviceScreen(shell, d)) },
                )
            }
            val others = session.others()
            rows += Row.Header(if (session.searching) "Other devices · searching…" else "Other devices")
            if (others.isEmpty()) {
                rows += Row.Entry(
                    if (session.searching) "Looking for devices…" else "Search again",
                    subtitle = "Put your headphones in pairing mode",
                    enabled = !session.searching,
                    onSelect = { session.search() },
                )
            }
            others.forEach { d ->
                rows += Row.Entry(
                    title = Bt.name(d),
                    subtitle = if (session.isPairing(d)) "Pairing…" else Bt.kind(d),
                    onSelect = {
                        session.pair(d)
                        render()
                    },
                )
            }
        }
        list.setRows(rows, keepSelection = true)
    }

    override fun onShow() {
        session.start()
        render()
    }

    override fun onHide() = session.stop()

    override fun onTick() {
        super.onTick()
        session.searchAgainIfIdle()
    }
}

// A paired device: connect / disconnect, forget.
class BluetoothDeviceScreen(shell: ShellActivity, private val device: BluetoothDevice) : ShellListScreen(shell, Bt.name(device)) {
    override fun render() {
        val connected = Bt.state(device) == BluetoothProfile.STATE_CONNECTED
        list.setRows(
            listOf(
                Row.Entry(Bt.kind(device), trailing = device.address, enabled = false),
                if (connected) Row.Entry("Disconnect", onSelect = { Bt.disconnect(device); shell.pop() })
                else Row.Entry("Connect", onSelect = { Bt.connect(device); shell.pop() }),
                Row.Entry("Forget this device", subtitle = "Pair it again to use it", onSelect = { Bt.forget(device); shell.pop() }),
            ),
        )
    }
}
