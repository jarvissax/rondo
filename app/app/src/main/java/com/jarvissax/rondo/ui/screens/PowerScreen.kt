package com.jarvissax.rondo.ui.screens

import com.jarvissax.rondo.Power
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.ui.Row
import com.jarvissax.rondo.ui.Tiles

// Shut down or restart, from Settings or by holding the Power button (ShellActivity swaps Android's
// own power dialog, which the wheel can't use, for this screen). Cancel is first, so a stray
// center press does nothing.
class PowerScreen(shell: ShellActivity) : ShellListScreen(shell, "Power") {
    private var going: String? = null

    override fun render() {
        going?.let { return list.showMessage(it) }
        list.setRows(
            listOf(
                Row.Entry("Cancel", onSelect = { shell.pop() }),
                Row.Entry("Shut down", tile = Tiles.power, onSelect = { go("Shutting down…") { Power.shutDown(context) } }),
                Row.Entry("Restart", tile = Tiles.restart, onSelect = { go("Restarting…") { Power.restart(context) } }),
            ),
        )
    }

    private fun go(message: String, action: () -> Unit) {
        player?.let { if (it.isPlaying()) it.togglePlay() }
        going = message
        render()
        action()
    }
}
