package org.vita3k.emulator.ui.viewmodel

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import org.vita3k.emulator.Emulator
import org.vita3k.emulator.NativeLib
import org.vita3k.emulator.data.NativeImeState
import org.vita3k.emulator.overlay.DEFAULT_OVERLAY_MASK
import org.vita3k.emulator.overlay.OverlayConfig
import org.vita3k.emulator.overlay.OverlayState
import org.vita3k.emulator.overlay.OverlayStore

data class EmulationSessionUiState(
    val titleId: String = "",
    val gameTitle: String = "",
    val showMenu: Boolean = false,
    val isPaused: Boolean = false,
    val isEditingControls: Boolean = false,
    val wasPausedBeforeControlsEditor: Boolean = false,
    val showExitConfirmation: Boolean = false,
    val controlsVisible: Boolean = true,
    val overlayMask: Int = DEFAULT_OVERLAY_MASK,
    val l2r2Visible: Boolean = false,
    val l3r3Visible: Boolean = false,
    val touchSwitchVisible: Boolean = true,
    val hideToggleVisible: Boolean = false,
    val overlayScale: Int = 100,
    val overlayOpacity: Int = 100,
    val hideOverlayWhenControllerConnected: Boolean = true,
    val controllerConnected: Boolean = false,
    val statusMessage: String? = null
)

class EmulationSessionViewModel(application: Application) : AndroidViewModel(application) {

    private companion object {
        // Long enough to collapse a slider drag into a single write, short enough that the
        // setting is durable before the user can leave the screen.
        const val OVERLAY_PERSIST_DEBOUNCE_MS = 400L
    }

    private var initialized = false
    private var useGameOverlayOverride = false
    private var overlayPersistHandler: android.os.Handler? = null
    private var pendingOverlayState: OverlayState? = null

    // Single background thread so pause requests stay ordered relative to each other.
    private val pauseExecutor = java.util.concurrent.Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "Vita3K-Pause").apply { isDaemon = true }
    }

    private val overlayPersistRunnable = Runnable { flushOverlayPersist() }

    var uiState by mutableStateOf(EmulationSessionUiState())
        private set

    var imeState by mutableStateOf<NativeImeState?>(null)
        private set

    fun initialize(titleId: String?, gameTitle: String?) {
        if (initialized && uiState.titleId == titleId.orEmpty()) {
            return
        }

        val resolvedTitleId = titleId.orEmpty()
        val resolved = resolveOverlayState(resolvedTitleId)
        uiState = uiState
            .withOverlayConfig(resolved.second.config)
            .copy(
                titleId = resolvedTitleId,
                gameTitle = gameTitle.orEmpty()
            )
        useGameOverlayOverride = resolved.first
        initialized = true
    }

    fun updateRunningApp(titleId: String?, gameTitle: String?) {
        val resolvedTitleId = titleId.orEmpty()
        val resolved = resolveOverlayState(resolvedTitleId)
        uiState = uiState
            .withOverlayConfig(resolved.second.config)
            .copy(
                titleId = resolvedTitleId,
                gameTitle = gameTitle.orEmpty()
            )
        useGameOverlayOverride = resolved.first
        initialized = true
    }

    fun applyOverlayState(emulator: Emulator) {
        emulator.setControllerOverlayScale(uiState.overlayScale / 100f)
        emulator.setControllerOverlayOpacity(uiState.overlayOpacity)
        val visibleMask = when {
            uiState.showMenu -> 0
            uiState.isEditingControls -> activeOverlayMask()
            uiState.hideOverlayWhenControllerConnected && uiState.controllerConnected -> 0
            else -> uiState.overlayMask
        }
        emulator.setControllerOverlayState(visibleMask, uiState.isEditingControls, false)
    }

    fun handleBackPressed(emulator: Emulator): Boolean {
        return when {
            uiState.isEditingControls -> {
                finishControlsEditor(emulator)
                true
            }
            uiState.showMenu -> {
                closeMenu(emulator)
                true
            }
            else -> {
                openMenu(emulator, pauseGame = false)
                true
            }
        }
    }

    @JvmOverloads
    fun openMenu(emulator: Emulator, pauseGame: Boolean = true) {
        if (uiState.showMenu) {
            return
        }

        emulator.prepareImeForPauseMenu()
        emulator.ensurePauseMenuOnTop()
        releaseInputs(emulator)
        // With no native session there is nothing to pause, and the menu must still open -
        // otherwise the pause menu becomes unreachable. Only guard when we actually asked
        // for the game to be paused.
        val nativeSessionRunning = isNativeAppRunning()
        val paused = if (pauseGame && nativeSessionRunning) {
            if (!setPauseReasonAsync(NativeLib.PAUSE_REASON_MENU, true)) {
                return
            }
            true
        } else {
            isNativeAppPaused()
        }
        if (nativeSessionRunning && !setNativeInputInterceptedAsync(true)) {
            if (paused) {
                setPauseReasonAsync(NativeLib.PAUSE_REASON_MENU, false)
            }
            return
        }
        uiState = uiState.copy(
            showMenu = true,
            isPaused = paused,
            isEditingControls = false,
            showExitConfirmation = false,
            statusMessage = null
        )
        emulator.setControllerOverlayState(0, false, false)
    }

    fun closeMenu(emulator: Emulator) {
        releaseInputs(emulator)
        if (uiState.isPaused) {
            setPauseReasonAsync(NativeLib.PAUSE_REASON_MENU, false)
        }
        setNativeInputInterceptedAsync(false)
        uiState = uiState.copy(
            showMenu = false,
            isPaused = false,
            isEditingControls = false,
            showExitConfirmation = false,
            statusMessage = null
        )
        applyOverlayState(emulator)
        emulator.restoreImeAfterPauseMenu()
    }

    fun togglePause(emulator: Emulator) {
        if (uiState.isPaused) {
            closeMenu(emulator)
        } else {
            releaseInputs(emulator)
            if (!isNativeAppRunning()) {
                return
            }
            if (!setPauseReasonAsync(NativeLib.PAUSE_REASON_MENU, true)) {
                return
            }
            uiState = uiState.copy(isPaused = true, statusMessage = null)
            emulator.setControllerOverlayState(0, false, false)
        }
    }

    fun requestExit() {
        uiState = uiState.copy(showExitConfirmation = true)
    }

    fun dismissExitConfirmation() {
        uiState = uiState.copy(showExitConfirmation = false)
    }

    fun confirmExit(emulator: Emulator) {
        releaseInputs(emulator)
        uiState = uiState.copy(showExitConfirmation = false, statusMessage = null)
        emulator.requestNativeQuit()
    }

    fun setControlsVisible(emulator: Emulator, visible: Boolean) {
        updateOverlayConfig(emulator) { it.withControlsVisible(visible) }
    }

    fun setL2R2Visible(emulator: Emulator, visible: Boolean) {
        updateOverlayConfig(emulator) { it.withL2R2Visible(visible) }
    }

    fun setL3R3Visible(emulator: Emulator, visible: Boolean) {
        updateOverlayConfig(emulator) { it.withL3R3Visible(visible) }
    }

    fun setTouchSwitchVisible(emulator: Emulator, visible: Boolean) {
        updateOverlayConfig(emulator) { it.withTouchSwitchVisible(visible) }
    }

    fun setHideToggleVisible(emulator: Emulator, visible: Boolean) {
        updateOverlayConfig(emulator) { it.withHideToggleVisible(visible) }
    }

    fun setOverlayScale(emulator: Emulator, value: Int) {
        updateOverlayConfig(emulator) { it.copy(overlayScale = value) }
    }

    fun setOverlayOpacity(emulator: Emulator, value: Int) {
        updateOverlayConfig(emulator) { it.copy(overlayOpacity = value) }
    }

    fun setControllerConnected(emulator: Emulator, connected: Boolean) {
        if (uiState.controllerConnected == connected) {
            return
        }

        uiState = uiState.copy(controllerConnected = connected)
        applyOverlayState(emulator)
    }

    fun startControlsEditor(emulator: Emulator) {
        val mask = activeOverlayMask()
        releaseInputs(emulator)
        uiState = uiState.copy(
            showMenu = false,
            isPaused = uiState.isPaused,
            isEditingControls = true,
            wasPausedBeforeControlsEditor = uiState.isPaused,
            controlsVisible = true,
            overlayMask = mask,
            showExitConfirmation = false,
            statusMessage = null
        )
        emulator.setControllerOverlayState(mask, true, false)
    }

    fun finishControlsEditor(emulator: Emulator) {
        releaseInputs(emulator)
        val updatedOverlayState = captureOverlayState(emulator)
        persistOverlayState(updatedOverlayState)
        uiState = uiState.withOverlayConfig(updatedOverlayState.config).copy(
            showMenu = true,
            isPaused = uiState.wasPausedBeforeControlsEditor,
            isEditingControls = false,
            wasPausedBeforeControlsEditor = false,
            statusMessage = null
        )
        emulator.ensurePauseMenuOnTop()
        emulator.setControllerOverlayState(0, false, false)
    }

    fun resetControlsLayout(emulator: Emulator) {
        val defaultLayout = OverlayStore.defaultLayout(getApplication())
        emulator.setControllerOverlayLayout(defaultLayout)
        val updatedOverlayState = OverlayState(
            config = currentOverlayConfig(),
            layout = defaultLayout
        )
        persistOverlayState(updatedOverlayState)
        uiState = uiState.copy(statusMessage = null)
        applyOverlayState(emulator)
    }

    fun clearStatusMessage() {
        uiState = uiState.copy(statusMessage = null)
    }

    fun showStatusMessage(message: String) {
        uiState = uiState.copy(statusMessage = message)
    }

    fun updateImeState(state: NativeImeState?) {
        if (imeState == state) {
            return
        }

        imeState = state
    }

    fun currentOverlayConfig(): OverlayConfig {
        return OverlayConfig(
            overlayMask = uiState.overlayMask,
            overlayScale = uiState.overlayScale,
            overlayOpacity = uiState.overlayOpacity,
            hideOverlayWhenControllerConnected = uiState.hideOverlayWhenControllerConnected
        ).normalized()
    }

    fun updateOverlayConfig(emulator: Emulator, transform: (OverlayConfig) -> OverlayConfig) {
        val updatedOverlayState = captureOverlayState(
            emulator = emulator,
            config = transform(currentOverlayConfig()).normalized()
        )
        // Dragging a slider calls this for every step, and persisting writes about forty
        // SharedPreferences keys. The UI is updated immediately while the write is
        // coalesced, so only the last value of a gesture reaches storage.
        pendingOverlayState = updatedOverlayState
        scheduleOverlayPersist()
        uiState = uiState.withOverlayConfig(updatedOverlayState.config)
        applyOverlayState(emulator)
    }

    private fun scheduleOverlayPersist() {
        val handler = overlayPersistHandler ?: OverlayPersistHandler().also { overlayPersistHandler = it }
        if (!handler.hasCallbacks(overlayPersistRunnable)) {
            handler.postDelayed(overlayPersistRunnable, OVERLAY_PERSIST_DEBOUNCE_MS)
        }
    }

    private fun flushOverlayPersist() {
        val state = pendingOverlayState ?: return
        pendingOverlayState = null
        overlayPersistHandler?.removeCallbacks(overlayPersistRunnable)
        persistOverlayState(state)
    }

    private inner class OverlayPersistHandler : android.os.Handler(android.os.Looper.getMainLooper())

    fun promoteOverlayToGameOverride(emulator: Emulator) {
        val titleId = uiState.titleId
        if (titleId.isBlank()) {
            return
        }

        OverlayStore.saveGameOverride(getApplication(), titleId, captureOverlayState(emulator))
        useGameOverlayOverride = true
    }

    fun clearGameOverlayOverride(emulator: Emulator) {
        val titleId = uiState.titleId
        if (titleId.isBlank()) {
            return
        }

        OverlayStore.deleteGameOverride(getApplication(), titleId)
        useGameOverlayOverride = false
        emulator.refreshControllerOverlayScope()
        val resolved = resolveOverlayState(titleId)
        useGameOverlayOverride = resolved.first
        uiState = uiState.withOverlayConfig(resolved.second.config)
        applyOverlayState(emulator)
    }

    private fun activeOverlayMask(): Int {
        return if (uiState.overlayMask == 0) DEFAULT_OVERLAY_MASK else uiState.overlayMask
    }

    private fun captureOverlayState(
        emulator: Emulator,
        config: OverlayConfig = currentOverlayConfig()
    ): OverlayState {
        return OverlayState(
            config = config.normalized(),
            layout = emulator.captureControllerOverlayLayout().normalized()
        )
    }

    private fun persistOverlayState(state: OverlayState) {
        if (useGameOverlayOverride && uiState.titleId.isNotBlank()) {
            OverlayStore.saveGameOverride(getApplication(), uiState.titleId, state)
        } else {
            OverlayStore.saveGlobalState(getApplication(), state)
        }
    }

    private fun resolveOverlayState(titleId: String): Pair<Boolean, OverlayState> {
        val useGameOverride = titleId.isNotBlank() && OverlayStore.hasGameOverride(getApplication(), titleId)
        val overlayState = OverlayStore.resolveState(
            context = getApplication(),
            scopeId = titleId,
            allowGameOverride = useGameOverride
        )
        return useGameOverride to overlayState
    }

    // Suspending every guest thread happens under the kernel mutex on the calling thread,
    // so it must not run on the UI thread: on a heavy game the suspend calls are slow
    // enough to trip an ANR. The cheap atomic liveness check stays synchronous so the
    // caller keeps its "is a session live" guard, while the suspending part is handed to a
    // background thread.
    private fun setPauseReasonAsync(reasonMask: Int, enabled: Boolean): Boolean {
        if (!isNativeAppRunning()) {
            return false
        }

        val executor = pauseExecutor
        if (executor.isShutdown) {
            return false
        }

        executor.execute { runCatching { NativeLib.setPauseReasonEnabled(reasonMask, enabled) } }
        return true
    }

    private fun isNativeAppRunning(): Boolean {
        return runCatching { NativeLib.isAppRunning() }.getOrDefault(false)
    }

    private fun releaseInputs(emulator: Emulator) {
        runCatching { emulator.releaseControllerOverlayInputs() }
    }

    

    private fun setNativeInputInterceptedAsync(intercepted: Boolean): Boolean {
        if (!isNativeAppRunning()) {
            return false
        }

        val executor = pauseExecutor
        if (executor.isShutdown) {
            return false
        }

        executor.execute { runCatching { NativeLib.setInputIntercepted(intercepted) } }
        return true
    }

    private fun isNativeAppPaused(): Boolean {
        return runCatching { NativeLib.isAppPaused() }.getOrDefault(false)
    }

    override fun onCleared() {
        // A debounced write must not be lost when the session viewmodel goes away.
        flushOverlayPersist()
        overlayPersistHandler?.removeCallbacksAndMessages(null)
        overlayPersistHandler = null
        pauseExecutor.shutdown()
        super.onCleared()
    }

    private fun EmulationSessionUiState.withOverlayConfig(config: OverlayConfig): EmulationSessionUiState {
        val normalized = config.normalized()
        return copy(
            controlsVisible = normalized.controlsVisible,
            overlayMask = normalized.overlayMask,
            l2r2Visible = normalized.l2r2Visible,
            l3r3Visible = normalized.l3r3Visible,
            touchSwitchVisible = normalized.touchSwitchVisible,
            hideToggleVisible = normalized.hideToggleVisible,
            overlayScale = normalized.overlayScale,
            overlayOpacity = normalized.overlayOpacity,
            hideOverlayWhenControllerConnected = normalized.hideOverlayWhenControllerConnected
        )
    }
}
