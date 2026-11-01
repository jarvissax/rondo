package com.jarvissax.rondo.ui

import android.content.Context
import android.view.View
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import com.jarvissax.rondo.ShellActivity
import com.jarvissax.rondo.input.WheelAction
import com.jarvissax.rondo.player.PlayerService

// One page of the shell (a plain class with a View root, cheaper than Fragments on API 17).
// Screens see only WheelActions; anything a screen does not use falls back to the shell's
// global meaning (Back pops, Play/Pause toggles, Next/Prev skip).
abstract class Screen(protected val shell: ShellActivity) {
    protected val context: Context get() = shell
    protected val player: PlayerService? get() = shell.player
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    lateinit var view: View
        private set

    fun create(): View {
        view = build()
        return view
    }

    protected abstract fun build(): View

    open fun onAction(action: WheelAction, steps: Int): Boolean = false
    open fun onShow() {}
    open fun onHide() {}
    open fun onTick() {} // every 500 ms while on top
    open fun onPlayerReady() {}

    open fun onDestroy() {
        scope.cancel()
    }

    protected fun launch(block: suspend CoroutineScope.() -> Unit): Job = scope.launch(block = block)
}
