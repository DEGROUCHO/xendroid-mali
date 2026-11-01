package xendroid.compose

import android.os.Build
import android.os.Bundle
import android.os.CombinedVibration
import android.os.SystemClock
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import android.hardware.input.InputManager
import android.view.InputDevice
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import xendroid.compose.core.LiveCvars
import xendroid.compose.core.EmuProcessLink
import xendroid.compose.core.EmulatorRuntime
import xendroid.compose.core.FrontendLaunch
import xendroid.compose.core.EmulatorSession
import xendroid.compose.core.ScreenBrightnessSampler
import xendroid.compose.core.SessionLogs
import kotlin.math.abs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.lifecycle.lifecycleScope
import android.content.res.Configuration
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.widget.Toast
import android.os.VibrationEffect
import android.os.Vibrator
import android.preference.PreferenceManager
import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import kotlin.math.roundToInt
import kotlinx.coroutines.flow.StateFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import xendroid.compose.ui.disc.DiscSwapPanel
import xendroid.compose.ui.pause.SavestateEntry
import xendroid.compose.ui.pause.SavestatePickerPanel
import xendroid.compose.ui.keyboard.GuestKeyboardPanel
import xendroid.compose.ui.keyboard.clampToUtf16Units
import xendroid.compose.ui.messagebox.GuestMessageBoxPanel
import xendroid.compose.settings.ConfigHandle
import xendroid.compose.settings.ConfigStore
import xendroid.compose.ui.pause.QmAction
import xendroid.compose.ui.pause.QmChoice
import xendroid.compose.ui.pause.QmSegmented
import xendroid.compose.ui.pause.QmItem
import xendroid.compose.ui.pause.QmNote
import xendroid.compose.ui.pause.QmSection
import xendroid.compose.ui.pause.qmFirstSelectable
import xendroid.compose.ui.pause.qmMoveSelection
import xendroid.compose.ui.pause.QmIcons
import xendroid.compose.ui.pause.QmSlider
import xendroid.compose.ui.pause.QmTab
import xendroid.compose.ui.pause.QmToggle
import xendroid.compose.ui.pause.QuickMenuPanel
import xendroid.compose.ui.pause.qmActivate
import xendroid.compose.ui.pause.qmAdjust
import xendroid.compose.ui.pause.qmWrap
import xendroid.compose.ui.theme.xendroidTheme
import xendroid.compose.gamepad.GamepadGlobalsDto
import xendroid.compose.gamepad.toDto
import xendroid.compose.gamepad.stickTuning
import xendroid.compose.gamepad.withLayout
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.ExitToApp
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.runtime.mutableFloatStateOf
import xendroid.compose.gamepad.GamepadConfigDto
import xendroid.compose.gamepad.GamepadController
import xendroid.compose.gamepad.GamepadEditorScreen
import xendroid.compose.gamepad.GamepadOverlay
import xendroid.compose.gamepad.Kc
import xendroid.compose.gamepad.rememberAutoHide
import xendroid.compose.data.GameButtons
import xendroid.compose.data.KeymapStore
import xendroid.compose.ui.saves.SavesStore
import java.io.File

/**
 * The :emu emulator host (separate process; see manifest). Reads game_uri from the Intent,
 * performs the PRE-surface native setup, hosts a Vulkan SurfaceView, and drives the exact
 * surface->boot ordering. onDestroy hard-kills the process.
 */
class EmulatorHostActivity : ComponentActivity(), SurfaceHolder.Callback {

    companion object {
        private const val TAG = "EmuHost"
        private const val KEYBOARD_POLL_MS = 150L
        const val EXTRA_GAME_URI = "game_uri"   // keys must match GameLibraryViewModel
        const val EXTRA_DISC_LABELS = "disc_labels"
        const val EXTRA_DISC_PATHS = "disc_paths"
        const val EXTRA_TITLE_ID = "title_id"           // keys must match GameLibraryViewModel
        const val EXTRA_GAME_NAME = "game_name"

        // Default Android-KeyEvent -> VirtualControl KEY_CODE map, mirrored locally because
        // VirtualControl lives in :app, not on the :app-compose classpath.
        private const val KC_DPAD_LEFT = 0;  private const val KC_DPAD_UP = 1
        private const val KC_DPAD_RIGHT = 2; private const val KC_DPAD_DOWN = 3
        private const val KC_A = 4; private const val KC_B = 5
        private const val KC_X = 6; private const val KC_Y = 7
        private const val KC_BACK = 8; private const val KC_START = 9
        private const val KC_SHOULDER_L = 10; private const val KC_SHOULDER_R = 11
        private const val KC_TRIGGER_L = 14; private const val KC_TRIGGER_R = 15
        private const val KC_LTHUMB_LEFT = 16; private const val KC_LTHUMB_UP = 17
        private const val KC_LTHUMB_RIGHT = 18; private const val KC_LTHUMB_DOWN = 19
        private const val KC_RTHUMB_LEFT = 20; private const val KC_RTHUMB_UP = 21
        private const val KC_RTHUMB_RIGHT = 22; private const val KC_RTHUMB_DOWN = 23
        private const val KEY_VALUE_UNUSED = -1
        // Analog hardware never reports exactly 0; without this the drift crosses into
        // native on every sample.
        private const val AXIS_DEADZONE = 0.08f
        // Triggers: resting noise below this reads as released; XInput's own digital threshold.
        private const val TRIGGER_REST = 6
        private const val TRIGGER_PRESSED = 30
        // Rumble one-shots outlast the re-arm period, so a held motor never gaps.
        private const val RUMBLE_SHOT_MS = 600L
        private const val RUMBLE_REARM_MS = 400L
        private const val MENU_TAB_GAME = 0
        private const val MENU_TAB_CONTROLS = 1
    }

    private val session = EmulatorSession()
    private var surfaceView: SurfaceView? = null
    private var started = false          // surface-callback boot guard
    private var bootDisplayTargetFps = 0f // 0: no display-rate preference
    private var lastDisplayVote: Float? = null

    private val mainHandler = Handler(Looper.getMainLooper())

    private val gamepad by lazy { GamepadController(applicationContext) }
    private val brightnessSampler = ScreenBrightnessSampler()
    @Volatile private var overlayWantsBrightness = false
    private var hapticsEnabled = false
    private val bootedState = mutableStateOf(false)
    private val showFpsOverlay = mutableStateOf(false) // Display|show_debug_overlay (native TOML config)
    /** Once the user toggles metrics from the pause menu, the post-boot poll must stop
     *  clobbering the choice (the per-game cvar wins over the global one we persist). */
    private var fpsUserToggled = false
    /** The readout also shows CPU / RAM / GPU / temperature. Kept in this process' own prefs
     *  (the on/off switch itself is the native show_debug_overlay cvar). */
    private val statsDetailedState = mutableStateOf(false)

    /** Emulator-wide performance / heat switches shown in the in-game menu. Each drives one
     *  or more native cvars: booleans, or integers ([onValue] when the switch is on,
     *  [offValue] when off; with offValue 0 any positive value counts as on). [live] ones are
     *  applied to the running game through [LiveCvars]; the others at the next game launch.
     *  [optional] ones are off by default and not part of the recommended set. Captions fit
     *  one line of the panel. */
    private data class HeatCvar(
        val section: String, val name: String, val onValue: Int = 0, val offValue: Int = 0,
        val onBool: Boolean = true,
    ) {
        val isInt get() = onValue != 0
        fun isOn(v: Int) = if (offValue == 0) v > 0 else v == onValue
    }
    /** [HeatCvar.onValue] meaning "the panel's fastest refresh rate" (120 on a 120 Hz
     *  phone): the adaptive limiter then paces at 120, or a steady half when the game
     *  can't hold it, instead of a fixed 60 cap. */
    private val PANEL_MAX_HZ = -1
    private fun onValueOf(c: HeatCvar): Int {
        if (c.onValue != PANEL_MAX_HZ) return c.onValue
        val hz = runCatching {
            val d = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display
                else @Suppress("DEPRECATION") windowManager.defaultDisplay
            d?.supportedModes?.maxOfOrNull { Math.round(it.refreshRate) }
        }.getOrNull() ?: 60
        return hz.coerceIn(60, 144)
    }
    private data class HeatOpt(
        val id: String, val title: String, val subtitle: String, val cvars: List<HeatCvar>,
        val live: Boolean = true, val optional: Boolean = false,
    )
    private val heatOpts = listOf(
        HeatOpt("limiter", l10n("Automatic smoothness (120 / 60 FPS)", "Fluidità automatica (120 / 60 FPS)"),
            l10n("Fixed pace at the highest rate the game can hold. Off: free framerate", "Ritmo fisso al massimo che il gioco regge. Spento: framerate libero"),
            listOf(HeatCvar("GPU", "framerate_limit_adaptive"),
                HeatCvar("GPU", "framerate_limit", onValue = PANEL_MAX_HZ),
                HeatCvar("GPU", "guest_display_refresh_cap"))),
        HeatOpt("pacing", l10n("Idle frame wait", "Attesa del frame a riposo"),
            l10n("Sleeps until its turn instead of spinning", "Dorme fino al suo turno invece di girare a vuoto"),
            listOf(HeatCvar("Video", "guest_swap_pacing"))),
        HeatOpt("parks", l10n("Waiting threads rest", "Riposo dei thread in attesa"),
            l10n("Pauses timed to the edge of each frame", "Pause regolate sul margine di ogni frame"),
            listOf(HeatCvar("Kernel", "guest_timebase_spin_park_us", onValue = 100),
                HeatCvar("Kernel", "guest_poll_spin_park_us", onValue = 100),
                HeatCvar("Kernel", "guest_poll_spin_tight_gap_us", onValue = 12),
                HeatCvar("Kernel", "guest_spin_sample_park_us", onValue = 2000),
                HeatCvar("Video", "guest_loading_ignores_streaming"))),
        HeatOpt("latency", l10n("Fast hand-offs between game threads", "Passaggi rapidi tra i thread del gioco"),
            l10n("More FPS when the game is below target", "Più FPS quando il gioco è sotto l'obiettivo"),
            listOf(HeatCvar("CPU", "pulse_event_skip_without_waiters"),
                HeatCvar("CPU", "global_mutex_spin_us", onValue = 40),
                HeatCvar("Kernel", "guest_scheduler_idle_spin_us", onValue = 80),
                HeatCvar("GPU", "gpu_stall_spin_us", onValue = 80),
                HeatCvar("Kernel", "wait_poll_fast_path"))),
        HeatOpt("jit", l10n("Faster translation of loops and floating point", "Traduzione più veloce di cicli e virgola mobile"),
            l10n("Same game work with less phone work", "Stesso lavoro del gioco con meno lavoro del telefono"),
            listOf(HeatCvar("CPU", "a64_loop_register_pinning"),
                HeatCvar("CPU", "a64_single_precision_narrowing"),
                HeatCvar("CPU", "a64_single_precision_across_blocks"),
                HeatCvar("CPU", "preempt_check_skip_leaf_entry"),
                HeatCvar("CPU", "simplify_rotate_mask_word_bits"),
                HeatCvar("CPU", "a64_sink_compare_to_branch"),
                HeatCvar("CPU", "a64_float_memory_domain"),
                HeatCvar("CPU", "a64_float_memory_domain_doubles", onBool = false),
                HeatCvar("a64", "a64_shared_cold_paths"),
                HeatCvar("a64", "a64_indirection_bases_in_context"),
                HeatCvar("a64", "a64_near_bit_branches"),
                HeatCvar("a64", "a64_call_return_address_in_register"),
                HeatCvar("CPU", "a64_longjmp_sync_at_resolve")), live = false),
        HeatOpt("placement", l10n("Light threads on the efficiency cores", "Thread leggeri sui core a basso consumo"),
            l10n("Fast cores stay with whatever sets the FPS", "I core veloci restano a chi decide gli FPS"),
            listOf(HeatCvar("Kernel", "guest_threads_placement", onValue = 1))),
        HeatOpt("governor", l10n("Less detail when hot", "Dettaglio ridotto quando scalda"),
            l10n("Lighter shading above about 42 °C", "Ombreggiatura più leggera oltre circa 42 °C"),
            listOf(HeatCvar("Vulkan", "thermal_governor"))),
        HeatOpt("adaptive", l10n("Adaptive detail when the GPU struggles", "Dettaglio adattivo quando la GPU non regge"),
            l10n("2x1 shading only while the GPU limits FPS", "Ombreggiatura 2x1 solo mentre la GPU limita gli FPS"),
            listOf(HeatCvar("Vulkan", "adaptive_shading_rate"))),
        HeatOpt("msaa", l10n("Light antialiasing", "Antialiasing leggero"),
            l10n("Less GPU work in games with antialiasing", "Meno lavoro per la GPU nei giochi con antialiasing"),
            listOf(HeatCvar("Vulkan", "vulkan_msaa_single_sample_storage")), live = false),
        HeatOpt("fps30", l10n("30 FPS cap", "Limite a 30 FPS"),
            l10n("Halves the work in 60 FPS games: much less heat", "Dimezza il lavoro nei giochi a 60: molto meno calore"),
            listOf(HeatCvar("GPU", "framerate_limit", onValue = 30, offValue = 60)),
            optional = true),
    )
    /** Switch states by [HeatOpt.id]; a missing entry means the default (on, or off for an
     *  optional one). */
    private val heatOptsState = mutableStateOf<Map<String, Boolean>>(emptyMap())
    /** What the config held when this session first read it: a switch that differs from it
     *  is pending until the next launch. */
    private var heatOptsAtLaunch: Map<String, Boolean>? = null
    private val statsGraphState = mutableStateOf(false)
    /** Bumped to make the readout re-read its saved position (menu: reset position). */
    private val hudPositionEpoch = mutableIntStateOf(0)
    private val menuTabState = mutableIntStateOf(0)
    /** One-line session summary in the menu header, taken when the menu opens. */
    private val menuStatusState = mutableStateOf("")
    /** 0 = follow the system; otherwise this window's own brightness (not persisted). */
    private val screenBrightnessState = mutableFloatStateOf(0f)
    private val statsPrefs by lazy { getSharedPreferences("fps_overlay", MODE_PRIVATE) }
    // Null until the first post-boot poll reads HID|show_touch_overlay. Mounting the overlay
    // on an assumed value and unmounting a tick later fires its release-all teardown while
    // the core is still booting.
    private val showTouchOverlay = mutableStateOf<Boolean?>(null)
    private val menuOpenState = mutableStateOf(false)
    /** False from launch until the game presents its first frame (or the wait gives up):
     *  the loading card covers what would otherwise be a silent black screen. */
    private val firstFrameSeenState = mutableStateOf(false)
    /** The touch-layout editor, opened from the pause menu over the paused game. */
    private val editingControlsState = mutableStateOf(false)
    /** Quit needs a second press; cleared whenever the menu closes or the selection moves. */
    private val quitArmedState = mutableStateOf(false)
    /** Latest persisted touch-controls config (null until first read): the menu both shows
     *  and rewrites its globals. */
    private val gamepadCfgState = mutableStateOf<GamepadConfigDto?>(null)
    /** The game must stay paused while one of our own full-screen panels is up. */
    private fun uiHoldsPause(): Boolean = menuOpenState.value || editingControlsState.value || snapshotBusy.value ||
        savestatePickerState.value != null
    private val snapshotBusy = mutableStateOf(false)
    private val snapshotEnabled = mutableStateOf(false)
    private val snapshotStatus = mutableStateOf("")
    /** The saved states listed by the load picker while it is open, newest first. */
    private val savestatePickerState = mutableStateOf<List<SavestateEntry>?>(null)
    private val savestateDeleteArmed = mutableIntStateOf(-1)
    // Shell-only test entry, registered only while this game Activity exists.
    // DUMP permission excludes ordinary apps; path/slot are owned by this Activity.
    private val snapshotReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            val operation = when (intent?.getStringExtra("operation")) {
                "capability" -> 0
                "save" -> 1
                "restore" -> 2
                else -> return
            }
            // Optional named slot for test scenes (slot-<name>.xds); default slot-1.
            val slot = intent.getStringExtra("slot")?.takeIf { it.matches(Regex("[a-z0-9-]{1,24}")) }
            val target = slot?.let { name -> savestateFolder()?.let { File(it, "slot-$name.xds") } }
            runtimeSnapshotOperation(operation, target)
        }
    }
    private var snapshotReceiverRegistered = false
    /** Launch extras: per-game menu actions (saves backup, graphics profiles). */
    private var currentTitleId: String? = null
    private var currentGameName: String = ""
    /** Per-title profiles shipped in assets/profiles/<titleId>/, cycled from the menu. */
    private val graphicsPresetNames = mutableStateOf<List<String>>(emptyList())
    /** -1 = no preset applied yet this session (the menu label shows "Default"). */
    private val graphicsPresetIndex = mutableIntStateOf(-1)
    private val keyboardRequestState =
        mutableStateOf<Emulator.KeyboardRequest?>(null)
    private val discRequestState =
        mutableStateOf<Emulator.DiscSwapRequest?>(null)
    private val messageBoxRequestState =
        mutableStateOf<Emulator.MessageBoxRequest?>(null)
    private val panelSelectedState = mutableIntStateOf(0)
    private val keyboardTextState = mutableStateOf("")

    // Falls back to GameButtons.DEFAULT_LOOKUP until KeymapStore loads in onCreate.
    @Volatile private var keyMap: Map<Int, Int> = GameButtons.DEFAULT_LOOKUP
    private var vibrator: Vibrator? = null
    // Controller rumble (API 31+): the game's XInputSetState, polled every 16 ms and played on
    // the vibrators of the controller that sent the last input.
    private val rumbleOn = mutableStateOf(true)
    private var rumbleDeviceId = -1
    private var rumbleApplied = 0
    private var rumbleArmedAt = 0L
    private val rumbleTick = object : Runnable {
        override fun run() {
            applyRumble()
            mainHandler.postDelayed(this, 16L)
        }
    }
    private val controllerPrefs by lazy { getSharedPreferences("controller", MODE_PRIVATE) }
    // L3 (THUMBL) / R3 (THUMBR) held-state for the metrics-toggle chord.
    private var l3Down = false
    private var r3Down = false

    // PC controls follow the device in use: a WASD key or the mouse switches them on, a
    // controller button or stick switches them off (F8 toggles by hand). Off at launch.
    private val pcInput = PcInputMapper(
        { code, pressed -> if (session.booted) session.keyEvent(code, pressed, KEY_VALUE_UNUSED) },
        { right, x, y ->
            if (session.booted) {
                emitAxisPair(x, if (right) KC_RTHUMB_LEFT else KC_LTHUMB_LEFT,
                    if (right) KC_RTHUMB_RIGHT else KC_LTHUMB_RIGHT, false)
                emitAxisPair(y, if (right) KC_RTHUMB_UP else KC_LTHUMB_UP,
                    if (right) KC_RTHUMB_DOWN else KC_LTHUMB_DOWN, true)
            }
        },
    )
    private val pcMouseTick = object : Runnable {
        override fun run() {
            if (!pcInput.enabled || !window.decorView.hasWindowFocus() || panelNav() != null) {
                releasePcInput(); return
            }
            pcInput.tick()
            mainHandler.postDelayed(this, 8L)
        }
    }
    private val inputDevices = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) { updatePcCapture() }
        override fun onInputDeviceChanged(deviceId: Int) = Unit
        override fun onInputDeviceRemoved(deviceId: Int) { releasePcInput() }
    }

    private fun releasePcInput() {
        mainHandler.removeCallbacks(pcMouseTick)
        pcInput.reset()
        if (session.booted) for (code in 0..23) {
            session.keyEvent(code, false, if (code >= 16) 0 else KEY_VALUE_UNUSED)
        }
        axisPressed.fill(false); axisValue.fill(Int.MIN_VALUE)
        l3Down = false; r3Down = false
        if (surfaceView?.hasPointerCapture() == true) surfaceView?.releasePointerCapture()
    }

    private fun updatePcCapture() {
        if (!pcInput.enabled || !session.booted || panelNav() != null ||
            !window.decorView.hasWindowFocus()) return
        surfaceView?.requestFocus()
        surfaceView?.requestPointerCapture()
        mainHandler.removeCallbacks(pcMouseTick)
        mainHandler.post(pcMouseTick)
    }

    private fun pcKey(code: Int): PcInputMapper.Key? = when (code) {
        KeyEvent.KEYCODE_W -> PcInputMapper.Key.W
        KeyEvent.KEYCODE_A -> PcInputMapper.Key.A
        KeyEvent.KEYCODE_S -> PcInputMapper.Key.S
        KeyEvent.KEYCODE_D -> PcInputMapper.Key.D
        KeyEvent.KEYCODE_SPACE -> PcInputMapper.Key.SPACE
        KeyEvent.KEYCODE_R -> PcInputMapper.Key.R
        KeyEvent.KEYCODE_F -> PcInputMapper.Key.F
        KeyEvent.KEYCODE_E -> PcInputMapper.Key.E
        KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.KEYCODE_SHIFT_RIGHT -> PcInputMapper.Key.SHIFT
        KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_CTRL_RIGHT -> PcInputMapper.Key.CTRL
        KeyEvent.KEYCODE_C -> PcInputMapper.Key.C
        KeyEvent.KEYCODE_Q -> PcInputMapper.Key.Q
        KeyEvent.KEYCODE_G -> PcInputMapper.Key.G
        KeyEvent.KEYCODE_V -> PcInputMapper.Key.V
        KeyEvent.KEYCODE_ENTER -> PcInputMapper.Key.ENTER
        KeyEvent.KEYCODE_TAB -> PcInputMapper.Key.TAB
        KeyEvent.KEYCODE_X -> PcInputMapper.Key.X
        else -> null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        UiLanguage.init(this)
        super.onCreate(savedInstanceState)
        getSystemService(InputManager::class.java).registerInputDeviceListener(inputDevices, mainHandler)
        enterImmersiveMode()

        // Keep the launcher out of the cached band so lmkd stops reaping it mid-game, and
        // notice if the main process dies anyway.
        EmuProcessLink.bindToMainProcess(this)

        currentTitleId = intent.getStringExtra(EXTRA_TITLE_ID)?.takeIf { it.isNotBlank() }
        val snapshotFilter = IntentFilter("xendroid.compose.RUNTIME_STATE")
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(snapshotReceiver, snapshotFilter, "android.permission.DUMP", null,
                Context.RECEIVER_EXPORTED)
        } else {
            registerReceiver(snapshotReceiver, snapshotFilter, "android.permission.DUMP", null)
        }
        snapshotReceiverRegistered = true
        currentGameName = intent.getStringExtra(EXTRA_GAME_NAME).orEmpty()
        statsDetailedState.value = statsPrefs.getBoolean("detailed", false)
        statsGraphState.value = statsPrefs.getBoolean("graph", false)
        rumbleOn.value = controllerPrefs.getBoolean("rumble", true)
        pcInput.setTitleId(currentTitleId)

        val gameUri = FrontendLaunch.resolveGamePath(this, intent)
        if (gameUri.isNullOrEmpty()) {
            Log.e(TAG, "No bootable game in launch intent; finishing")
            Toast.makeText(this, "XenDroid: no game in launch intent", Toast.LENGTH_LONG).show()
            finish(); return
        }
        if (!EmulatorRuntime.supportsVulkan) {
            Toast.makeText(this, l10n("GPU without Vulkan: the game cannot start", "GPU senza Vulkan: il gioco non può partire"), Toast.LENGTH_LONG).show()
            finish(); return
        }

        // Async ONLY so ensureLoaded() runs off-main on delay-load devices; the native
        // setup_* calls are marshaled back to the main thread.
        lifecycleScope.launch {
            val store = KeymapStore(applicationContext)
            keyMap = withContext(Dispatchers.IO) {
                EmulatorRuntime.ensureLoaded()            // idempotent; lazy on Adreno 5xx/6xx
                store.androidToGameKey.firstOrNull() ?: GameButtons.DEFAULT_LOOKUP
            }
            // show_debug_overlay lives in the native TOML config, not SharedPreferences;
            // read it off-main via ConfigStore.
            showFpsOverlay.value = withContext(Dispatchers.IO) { readShowDebugOverlay() }

            bootDisplayTargetFps = withContext(Dispatchers.IO) { readBootDisplayTarget() }

            // Per-title graphics profiles (cycled from the pause menu), in tier order.
            currentTitleId?.let { tid ->
                graphicsPresetNames.value = withContext(Dispatchers.IO) {
                    runCatching {
                        val present = (assets.list("profiles/$tid") ?: emptyArray()).toSet()
                        listOf("performance.toml", "balanced.toml", "quality.toml", "ultra.toml")
                            .filter { it in present }
                    }.getOrDefault(emptyList())
                }
            }

            // Guard on the LIVE grant: revoked-in-Settings while away => fail cleanly
            // instead of booting against a path we can no longer read.
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R ||
                !Environment.isExternalStorageManager()) {
                Toast.makeText(this@EmulatorHostActivity,
                    "All Files Access not granted; open the library first",
                    Toast.LENGTH_LONG).show()
                finish(); return@launch
            }
            // Shortcut launches skip the main process; make sure the session logcat
            // capture exists.
            withContext(Dispatchers.IO) {
                runCatching { SessionLogs.ensureCaptureRunning() }
                    .onFailure { Log.w(TAG, "Session log capture failed", it) }
            }
            prepareNativeRealPath(gameUri)                // back on main
            installSurfaceView()
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            enterImmersiveMode()
            updatePcCapture()
            requestLowLatencyTouch()
            if (session.booted && !uiHoldsPause()) session.resumeIfPaused()
        } else {
            // Focus can return while Mali is still compiling. Pausing the core
            // here blocks the Android input looper on GPU/audio fences; real
            // backgrounding is still paused by onStop/surfaceDestroyed.
            releasePcInput()
        }
    }

    private var composeView: ComposeView? = null

    /** Touch-pad event trace for on-device checks: adb shell setprop log.tag.EmuHost DEBUG
     *  before launching the game. */
    private val logTouchPad = Log.isLoggable(TAG, Log.DEBUG)

    /** The core samples input once per guest frame, so a tap shorter than a frame can be
     *  missed. 70 ms covers 30 fps and up; below that the pulse grows with the frame time. */
    private fun minTouchPressMs(): Long {
        val frameMs = if (session.booted) session.lastFrameTimeMs() else 0.0
        return (frameMs * 1.5).toLong().coerceIn(70L, 130L)
    }

    /** Touch and controller stick events are normally held back and delivered once per display
     *  frame: up to a frame of added input lag. Ask for them as they happen. */
    private fun requestLowLatencyTouch() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        val view = composeView?.takeIf { it.isAttachedToWindow } ?: return
        runCatching {
            // The request only travels up to the window when the value changes, and one made
            // before the window existed never arrived: clear it first so it always does.
            view.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_NONE)
            view.requestUnbufferedDispatch(
                InputDevice.SOURCE_CLASS_POINTER or InputDevice.SOURCE_CLASS_JOYSTICK)
        }
    }

    private fun enterImmersiveMode() {
        // Gamepad play produces no touch events, so the display would still time out.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // Sustained performance mode: the SoC holds clocks steadier under long play
        // (same trick PPSSPP uses; ignored where unsupported).
        runCatching { window.setSustainedPerformanceMode(true) }
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.systemBars())
        }
    }

    /** Uses a read-only string snapshot so closing the handle frees the native table WITHOUT
     *  re-serializing to disk (a plain read must not mutate the config). Failure => off. */
    private fun readShowDebugOverlay(): Boolean = runCatching {
        val handle = ConfigStore(applicationContext).openLiveSnapshot()
        try {
            handle.getBool("Display", "show_debug_overlay", false)
        } finally {
            handle.closeString()
        }
    }.getOrDefault(false)

    /** PRE-surface setup in order: setupContext -> setupGamePathReal -> launch args ->
     *  uri info list. [absPath] is an absolute host path. */
    private fun prepareNativeRealPath(absPath: String) {
        session.setupContext(this)
        session.setupGamePathReal(absPath)
        session.setupLaunchArgs(arrayOf(
            "--storage_root=" + Utils.get_storage_root_path(),
            "--config=" + Application.get_global_config_file().absolutePath,
            "--log_file=" + Utils.get_log_file_path(),
            // One app session = one xe.log: emulator runs append, the shelver rotates at
            // the next app-session start.
            "--log_append=true",
        ))
        session.setupUriInfoListFile(Application.get_uri_info_list_file().absolutePath)
    }

    private fun installSurfaceView() {
        val sv = object : SurfaceView(this) {
            override fun onPointerCaptureChange(hasCapture: Boolean) {
                super.onPointerCaptureChange(hasCapture)
                Log.i(TAG, "PC pointer capture=$hasCapture mode=${pcInput.enabled}")
                if (!hasCapture && pcInput.enabled) {
                    mainHandler.removeCallbacks(pcMouseTick)
                    pcInput.reset()
                }
            }
        }.apply {
            isFocusable = true
            isFocusableInTouchMode = true
            holder.addCallback(this@EmulatorHostActivity)
            setOnGenericMotionListener { _, ev -> onGenericMotion(ev) }
            setOnCapturedPointerListener { _, ev ->
                if (!pcInput.enabled || panelNav() != null) {
                    releasePcInput(); false
                } else {
                    var x = ev.x; var y = ev.y
                    if (ev.actionMasked != MotionEvent.ACTION_MOVE &&
                        ev.actionMasked != MotionEvent.ACTION_HOVER_MOVE) { x = 0f; y = 0f }
                    for (n in 0 until ev.historySize) {
                        x += ev.getHistoricalX(n); y += ev.getHistoricalY(n)
                    }
                    pcInput.mouse(x, y,
                        ev.buttonState and MotionEvent.BUTTON_PRIMARY != 0,
                        ev.buttonState and MotionEvent.BUTTON_SECONDARY != 0)
                    true
                }
            }
        }
        surfaceView = sv
        val compose = ComposeView(this).apply {
            setContent {
                val cfg by gamepad.config.collectAsState(initial = GamepadConfigDto())
                LaunchedEffect(Unit) { gamepad.config.collect { gamepadCfgState.value = it } }
                val editingControls by editingControlsState
                val landscape = resources.configuration.orientation ==
                    Configuration.ORIENTATION_LANDSCAPE
                val booted by bootedState
                val controls = remember(cfg, landscape) { gamepad.controlsFor(cfg, landscape) }
                val (visible, poke) = rememberAutoHide(cfg.globals.autoHideSeconds)
                val alpha by animateFloatAsState(
                    if (visible) cfg.globals.opacity else 0f,
                    // Waking is instant-ish (the pad is needed NOW); hiding fades out slowly.
                    tween(if (visible) 90 else 500), label = "padAlpha")

                val padVisible = showTouchOverlay.value == true
                val overlayActive = booted && cfg.globals.enabled && padVisible && !editingControls
                // onStart/onStop co-own the sampler thread: no PixelCopy polling while backgrounded.
                DisposableEffect(overlayActive) {
                    overlayWantsBrightness = overlayActive
                    if (overlayActive) brightnessSampler.start(sv)
                    onDispose { brightnessSampler.stop() }
                }
                val contrastState = rememberOverlayContrast(brightnessSampler.brightness)

                LaunchedEffect(cfg.globals.hapticsEnabled) {
                    configureHaptics(cfg.globals.hapticsEnabled)
                }
                Box(Modifier.fillMaxSize()) {
                    AndroidView(factory = { sv }, modifier = Modifier.fillMaxSize())
                    // Stay MOUNTED whenever enabled (alpha drives only the DRAW): the pointerInput
                    // must keep receiving touches so the auto-hide wake tap fires, and so a held
                    // control is never unmounted mid-press (stuck).
                    if (overlayActive) {
                        GamepadOverlay(
                            controls = controls,
                            opacity = alpha,
                            floatingLeftStick = cfg.globals.floatingLeftStick,
                            faceButtonSlide = cfg.globals.faceButtonSlide,
                            floatingRightStick = cfg.globals.floatingRightStick,
                            aimWithTrigger = cfg.globals.aimWithTrigger,
                            stickTuning = remember(cfg.globals) { cfg.globals.stickTuning() },
                            ltToggle = cfg.globals.ltToggle,
                            minPressMs = { minTouchPressMs() },
                            contrast = { contrastState.value },
                            onUserInteraction = {
                                // The mouse tick writes neutral stick values while idle.
                                // Hand ownership to touch before emitting its first axis.
                                if (pcInput.enabled) {
                                    releasePcInput()
                                    pcInput.setEnabled(false)
                                }
                                poke()
                            },
                            onKeyEvent = { kc, pressed, v ->
                                if (pressed && v == Kc.VALUE_UNUSED) maybeVibrate()
                                if (logTouchPad) Log.d(TAG, "pad kc=$kc down=$pressed v=$v")
                                session.keyEvent(kc, pressed, v)
                            },
                            modifier = Modifier.fillMaxSize(),
                        )
                    }
                    // Boot can take a while (shader/JIT warm-up) with nothing on screen.
                    val firstFrameSeen by firstFrameSeenState
                    LaunchedEffect(Unit) {
                        val deadline = android.os.SystemClock.uptimeMillis() + 25_000L
                        while (isActive && !firstFrameSeenState.value) {
                            if ((session.booted && session.averageFps() > 0.0) ||
                                android.os.SystemClock.uptimeMillis() > deadline) {
                                firstFrameSeenState.value = true
                            }
                            delay(200)
                        }
                    }
                    if (!firstFrameSeen) xendroidTheme {
                        BootSplash(
                            gameName = currentGameName,
                            onDismiss = { firstFrameSeenState.value = true },
                            modifier = Modifier.fillMaxSize(),
                        )
                    }
                    val showFps by showFpsOverlay
                    // The onCreate read only saw the GLOBAL config; the per-game override lands
                    // on the detached boot thread, so POLL the effective cvar after boot, else
                    // "global off + per-game on" never shows the overlay.
                    LaunchedEffect(booted) {
                        if (!booted) return@LaunchedEffect
                        while (isActive) {
                            if (!fpsUserToggled) {
                                showFpsOverlay.value = session.showDebugOverlayEnabled()
                            }
                            showTouchOverlay.value = session.showTouchOverlayEnabled()
                            surfaceView?.holder?.let { applyDisplayRateVote(it) }
                            liveExtrasSeconds++
                            if (liveExtrasSeconds == 3 || liveExtrasSeconds == 10) {
                                writeLiveExtras()
                            }
                            // Machine-readable FPS timeline for on-device measurements.
                            Log.i(TAG, "fps=" + session.averageFps() +
                                " frameMs=" + session.lastFrameTimeMs())
                            delay(1000)
                        }
                    }
                    FpsOverlay(
                        session = session,
                        visible = booted && showFps && firstFrameSeenState.value,
                        detailed = statsDetailedState.value,
                        graph = statsGraphState.value,
                        positionEpoch = hudPositionEpoch.intValue,
                        modifier = Modifier.fillMaxSize(),
                    )

                    // Always-available touch door into the pause menu: gamepad users have Back,
                    // touch-only users had no way in. Hidden while a guest prompt owns input.
                    if (booted && firstFrameSeenState.value && !menuOpenState.value && !editingControls &&
                        keyboardRequestState.value == null &&
                        messageBoxRequestState.value == null &&
                        discRequestState.value == null) {
                        Box(
                            modifier = Modifier
                                .align(Alignment.TopCenter)
                                .padding(top = 4.dp)
                                .clip(RoundedCornerShape(50))
                                .background(Color.Black.copy(alpha = 0.35f))
                                .clickable { openPauseMenu() }
                                // A comfortable target: the label is small, the pill is not.
                                .padding(horizontal = 18.dp, vertical = 7.dp),
                            contentAlignment = Alignment.Center,
                        ) {
                            androidx.compose.material3.Icon(
                                Icons.Filled.Menu,
                                contentDescription = "Menu",
                                tint = Color.White.copy(alpha = 0.9f),
                                modifier = Modifier.size(18.dp),
                            )
                        }
                    }

                    // The emulator blocks a dispatch thread until answered.
                    val keyboardRequest by keyboardRequestState
                    LaunchedEffect(booted) {
                        if (!booted) return@LaunchedEffect
                        while (isActive) {
                            if (keyboardRequestState.value == null) {
                                session.keyboardRequest()?.let { req ->
                                    keyboardTextState.value = clampToUtf16Units(
                                        req.defaultText.orEmpty(),
                                        if (req.maxLength <= 0) Int.MAX_VALUE else req.maxLength)
                                    panelSelectedState.intValue = 0
                                    keyboardRequestState.value = req
                                }
                            }
                            delay(KEYBOARD_POLL_MS)
                        }
                    }
                    keyboardRequest?.let { req ->
                        xendroidTheme {
                            GuestKeyboardPanel(
                                request = req,
                                text = keyboardTextState.value,
                                onTextChange = { keyboardTextState.value = it },
                                selected = panelSelectedState.intValue,
                                onAccept = { text -> acceptKeyboard(text) },
                                onCancel = { cancelKeyboard() },
                                modifier = Modifier.fillMaxSize(),
                            )
                        }
                    }

                    // Blocks the guest thread that asked, until answered.
                    val messageBoxRequest by messageBoxRequestState
                    LaunchedEffect(booted) {
                        if (!booted) return@LaunchedEffect
                        while (isActive) {
                            if (messageBoxRequestState.value == null) {
                                session.messageBoxRequest()?.let { req ->
                                    panelSelectedState.intValue = req.activeButton
                                    messageBoxRequestState.value = req
                                }
                            }
                            delay(KEYBOARD_POLL_MS)
                        }
                    }
                    messageBoxRequest?.let { req ->
                        xendroidTheme {
                            GuestMessageBoxPanel(
                                request = req,
                                selected = panelSelectedState.intValue,
                                onChoose = { button -> answerMessageBox(button) },
                                modifier = Modifier.fillMaxSize(),
                            )
                        }
                    }

                    // Blocks the guest thread that asked, until answered.
                    val discRequest by discRequestState
                    LaunchedEffect(booted) {
                        if (!booted) return@LaunchedEffect
                        while (isActive) {
                            if (discRequestState.value == null) {
                                session.discRequest()?.let { req ->
                                    panelSelectedState.intValue = 0
                                    discRequestState.value = req
                                }
                            }
                            delay(KEYBOARD_POLL_MS)
                        }
                    }
                    discRequest?.let { req ->
                        xendroidTheme {
                            DiscSwapPanel(
                                request = req,
                                selected = panelSelectedState.intValue,
                                onChoose = { path -> chooseDisc(path) },
                                onCancel = { cancelDisc() },
                                modifier = Modifier.fillMaxSize(),
                            )
                        }
                    }
                }

                // Back / swipe-back PAUSES the game and opens a Quit menu instead of leaving to
                // the library; the dialog catches back / tap-outside -> resume.
                val menuOpen by menuOpenState
                // The open-prompt gates below give a prompt's BackHandler priority over the
                // menu one; without them back pauses behind the prompt.
                val keyboardOpen = keyboardRequestState.value != null
                val discOpen = discRequestState.value != null
                // No cancel: back answers with the button the guest pre-selected.
                val messageBoxOpen = messageBoxRequestState.value != null
                LaunchedEffect(booted, menuOpen, editingControls, keyboardOpen, discOpen, messageBoxOpen) {
                    if (menuOpen || editingControls || keyboardOpen || discOpen || messageBoxOpen) releasePcInput()
                    else updatePcCapture()
                }
                BackHandler(enabled = keyboardOpen) { cancelKeyboard() }
                BackHandler(enabled = discOpen) { cancelDisc() }
                BackHandler(enabled = messageBoxOpen) {
                    messageBoxRequestState.value?.let { answerMessageBox(it.activeButton) }
                }
                BackHandler(
                    enabled = !menuOpen && !editingControls &&
                        !keyboardOpen && !discOpen && !messageBoxOpen
                ) { openPauseMenu() }
                BackHandler(enabled = menuOpen) { closeMenuAndResume() }
                // Leaving the editor (Save, Cancel or Back) returns to the menu, still paused.
                BackHandler(enabled = editingControls) { closeControlsEditor() }
                if (editingControls) xendroidTheme {
                    GamepadEditorScreen(
                        controller = gamepad,
                        onDone = { closeControlsEditor() },
                        inGame = true,
                    )
                }
                if (menuOpen) xendroidTheme {
                    val tabs = quickMenuTabs()
                    QuickMenuPanel(
                        tabs = tabs,
                        tabIndex = menuTabState.intValue.coerceIn(0, tabs.lastIndex),
                        selected = panelSelectedState.intValue,
                        gameName = currentGameName,
                        statusLine = menuStatusState.value,
                        onSelectTab = { selectMenuTab(it) },
                        onSelectRow = { panelSelectedState.intValue = it },
                        onClose = { closeMenuAndResume() },
                        modifier = Modifier.fillMaxSize(),
                    )
                }
                val savestates by savestatePickerState
                BackHandler(enabled = savestates != null) { closeSavestatePicker() }
                savestates?.let { entries ->
                    xendroidTheme {
                        SavestatePickerPanel(
                            entries = entries,
                            selected = panelSelectedState.intValue,
                            deleteArmed = savestateDeleteArmed.intValue,
                            onLoad = { i -> entries.getOrNull(i)?.let { loadSavestate(it) } },
                            onDelete = { i -> deleteSavestate(i) },
                            onSelect = { panelSelectedState.intValue = it },
                            onCancel = { closeSavestatePicker() },
                            modifier = Modifier.fillMaxSize(),
                        )
                    }
                }
            }
        }
        composeView = compose
        setContentView(compose)
        sv.requestFocus()
        // Needs the view attached to its window; focus gain asks again.
        compose.post { requestLowLatencyTouch() }
    }

    /** Honors the legacy enable_vibrator pref too, so existing users keep their setting. */
    private fun configureHaptics(globalsFlag: Boolean) {
        val legacy = PreferenceManager.getDefaultSharedPreferences(this)
            .getBoolean("enable_vibrator", false)
        hapticsEnabled = globalsFlag || legacy
        if (hapticsEnabled && vibrator == null)
            vibrator = getSystemService(VIBRATOR_SERVICE) as Vibrator
    }
    private fun maybeVibrate() {
        if (!hapticsEnabled) return
        // The platform click: short and crisp, tuned per device (falls back to a plain buzz).
        runCatching {
            vibrator?.vibrate(VibrationEffect.createPredefined(VibrationEffect.EFFECT_CLICK))
        }
    }

    /** Read the configured boot target without rewriting config files. Settings edits
     *  are persisted for the NEXT boot; these files are not live native cvar getters. */
    private fun readBootDisplayTarget(): Float = runCatching {
        val store = ConfigStore(applicationContext)
        val globals = xendroid.compose.settings.ConfigHandle.openString(
            store.globalConfigFile().readText())
        try {
            val gameFile = currentTitleId?.let { store.perGameConfigFile(it) }
            val game = gameFile?.takeIf { it.isFile }?.let {
                xendroid.compose.settings.ConfigHandle.openString(it.readText())
            }
            try {
                fun value(section: String, name: String, default: Int): Int =
                    (game?.getString(section, name) ?: globals.getString(section, name))
                        ?.toIntOrNull() ?: default
                DisplayRatePolicy.target(
                    value("GPU", "framerate_limit", 60),
                    value("Display", "frame_generation", 0),
                    value("Display", "frame_generation_rate", 60))
            } finally {
                game?.closeDiscard()
            }
        } finally {
            globals.closeDiscard()
        }
    }.getOrElse {
        Log.w(TAG, "Display target unavailable; leaving rate selection to Android", it)
        0f
    }

    /** A preference, not a frame limiter. Re-vote only on change / surface recreation.
     *  Unlimited votes for the panel's fastest mode: with no vote the system's dynamic
     *  refresh (60/90/120 on Xiaomi) switches the panel mode under a game whose frame
     *  rate varies, and each switch and each 60 Hz stretch shows as a stutter. */
    private fun applyDisplayRateVote(holder: SurfaceHolder) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || !holder.surface.isValid) return
        val requested = DisplayRatePolicy.resolve(
            session.requestedPresentRate(), bootDisplayTargetFps)
        val displayTargetFps = if (requested > 0f) requested
            else (display?.supportedModes?.maxOfOrNull { it.refreshRate } ?: 0f)
        if (lastDisplayVote == displayTargetFps) return
        runCatching {
            holder.surface.setFrameRate(displayTargetFps,
                android.view.Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
            lastDisplayVote = displayTargetFps
            Log.i(TAG, "surface frame-rate hint: $displayTargetFps Hz")
        }.onFailure { Log.w(TAG, "Display rate vote failed", it) }
    }

    // SurfaceHolder.Callback: the load-bearing surface->boot ordering.

    override fun surfaceCreated(holder: SurfaceHolder) {
        lastDisplayVote = null
        applyDisplayRateVote(holder)
        if (!started) {
            started = true
            session.attachSurface(holder.surface)
            try {
                val labels = intent.getStringArrayExtra(EXTRA_DISC_LABELS)
                val paths = intent.getStringArrayExtra(EXTRA_DISC_PATHS)
                if (labels != null && paths != null && labels.isNotEmpty()) {
                    session.discSetKnown(labels.toList(), paths.toList())
                }
                session.bootOnce()
                bootedState.value = true
            } catch (t: RuntimeException) {
                Log.e(TAG, "boot failed", t)
                finish()                                  // fatal; single-shot core
            }
        } else {
            // Post-rotation/background re-create: re-attach and resume, but NEVER boot again.
            // Stay paused if the in-game menu is open.
            session.attachSurface(holder.surface)
            if (!uiHoldsPause()) session.resumeIfPaused()
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (!started) return
        if (width == 0 || height == 0) return
        session.changeSurface(width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        lastDisplayVote = null
        if (!started) return
        // surfaceDestroyed fires BEFORE onStop here: pause first so the drain below tears
        // down a quiescent GPU. onStop's pause() is then a no-op.
        if (session.booted) session.pause()
        session.detachSurface()                           // synchronous GPU drain
    }

    override fun onPause() {
        super.onPause()
        releasePcInput()
        session.flushGpuCaches()
    }

    override fun onStop() {
        super.onStop()
        mainHandler.removeCallbacks(rumbleTick)
        stopRumble()
        // Freeze guest CPU + GPU worker + audio when stopped (screen sleep / home / switch).
        // Ordering: onPause(flushGpuCaches) -> onStop(pause) -> surfaceDestroyed(detach);
        // pausing BEFORE the swapchain teardown keeps guest/GPU frames from racing the drain.
        if (session.booted) session.pause()
        brightnessSampler.stop()
        // An orphaned core (main process died while we were on screen) hard-kills here, so it
        // does so from a paused, quiescent state.
        EmuProcessLink.setEmuForeground(false)
    }

    override fun onStart() {
        super.onStart()
        mainHandler.removeCallbacks(rumbleTick)
        mainHandler.post(rumbleTick)
        EmuProcessLink.setEmuForeground(true)
        // The library may have saved a new touch layout while this activity was stopped.
        lifecycleScope.launch { gamepad.refresh() }
        // While a snapshot owns the runtime, resume() only updates its pending
        // lifecycle intent. Do not lose a foreground return behind snapshotBusy.
        if (session.booted && snapshotBusy.value && !menuOpenState.value && !editingControlsState.value)
            session.resume()
        if (overlayWantsBrightness) surfaceView?.let { brightnessSampler.start(it) }
        // resumeIfPaused (not bare resume) is idempotent: surfaceCreated already resumes on
        // swapchain-recreate, so this is a no-op there; onStart additionally covers the pure
        // screen-sleep case where the surface was NOT destroyed. Stay paused if the menu is open.
        if (session.booted && !uiHoldsPause()) session.resumeIfPaused()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (snapshotReceiverRegistered) unregisterReceiver(snapshotReceiver)
        getSystemService(InputManager::class.java).unregisterInputDeviceListener(inputDevices)
        releasePcInput()
        // A pending prompt must not hold a dispatch thread through teardown.
        keyboardRequestState.value = null
        session.keyboardCancelAll()
        discRequestState.value = null
        session.discCancelAll()
        messageBoxRequestState.value = null
        session.messageBoxCancelAll()
        // Hard-kill via SIGKILL: killProcess skips the C++ atexit static-destructor path
        // that System.exit(0) ran, which can deadlock joining a paused audio worker and
        // wedge :emu instead of closing it.
        Process.killProcess(Process.myPid())
    }

    /** Tracks L3/R3 and fires toggleFpsOverlay() once when the SECOND of the two goes down.
     *  Trade-off: every L3+R3 press during play also toggles metrics, since the events still
     *  reach the game (the helper never consumes them). */
    private fun trackStickClickChord(keyCode: Int, event: KeyEvent, pressed: Boolean) {
        if (keyCode != KeyEvent.KEYCODE_BUTTON_THUMBL &&
            keyCode != KeyEvent.KEYCODE_BUTTON_THUMBR) return
        val isL3 = keyCode == KeyEvent.KEYCODE_BUTTON_THUMBL
        if (pressed) {
            if (event.repeatCount > 0) return
            val otherWasDown = if (isL3) r3Down else l3Down
            if (isL3) l3Down = true else r3Down = true
            if (otherWasDown) toggleFpsOverlay()
        } else {
            if (isL3) l3Down = false else r3Down = false
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_F8 && panelNav() == null) {
            if (event.repeatCount == 0) {
                val next = !pcInput.enabled
                releasePcInput(); pcInput.setEnabled(next)
                updatePcCapture()
                Toast.makeText(this, if (next) l10n("PC controls: WASD + mouse (F8: controller)", "Comandi PC: WASD + mouse (F8: controller)")
                    else l10n("Controller controls (F8: WASD + mouse)", "Comandi del controller (F8: WASD + mouse)"), Toast.LENGTH_SHORT).show()
            }
            return true
        }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_MODE) {
            if (event.repeatCount == 0) toggleMenuFromGuide()
            return true
        }
        if (keyCode == KeyEvent.KEYCODE_MEDIA_RECORD && consumeIfGamepad(event)) {
            // The Share button of Xbox Series controllers.
            if (event.repeatCount == 0 && panelNav() == null) takeScreenshot()
            return true
        }
        followInputDevice(event)
        if (pcInput.enabled && panelNav() == null) {
            pcKey(keyCode)?.let { pcInput.key(it, true); return true }
            if (consumeIfGamepad(event) || event.source and InputDevice.SOURCE_MOUSE == InputDevice.SOURCE_MOUSE) return true
        }
        trackStickClickChord(keyCode, event, true)
        val nav = panelNav()
        if (nav != null) {
            if (panelKeyDown(nav, keyCode)) return true
            // Swallow the rest so neither the game nor an OEM overlay reacts behind it.
            if (consumeIfGamepad(event)) return true
        }
        val gameKey = keyMap[keyCode]
            ?: return consumeIfGamepad(event) || super.onKeyDown(keyCode, event)
        if (analogTriggerKey(gameKey, event)) return true
        if (event.repeatCount == 0) {
            session.keyEvent(gameKey, true, KEY_VALUE_UNUSED)
            return true
        }
        return super.onKeyDown(keyCode, event)            // ignore auto-repeats
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_F8 || keyCode == KeyEvent.KEYCODE_BUTTON_MODE) return true
        if (keyCode == KeyEvent.KEYCODE_MEDIA_RECORD && consumeIfGamepad(event)) return true
        if (pcInput.enabled && panelNav() == null) {
            pcKey(keyCode)?.let { pcInput.key(it, false); return true }
            if (consumeIfGamepad(event) || event.source and InputDevice.SOURCE_MOUSE == InputDevice.SOURCE_MOUSE) return true
        }
        trackStickClickChord(keyCode, event, false)
        if (panelNav() != null && (isPanelKey(keyCode) || consumeIfGamepad(event))) {
            return true
        }
        val gameKey = keyMap[keyCode]
            ?: return consumeIfGamepad(event) || super.onKeyUp(keyCode, event)
        if (analogTriggerKey(gameKey, event)) return true
        session.keyEvent(gameKey, false, KEY_VALUE_UNUSED)
        return true
    }

    /** Consumes unmapped controller buttons (never BACK): unhandled gamepad input is what
     *  OEM overlays latch onto. */
    private fun consumeIfGamepad(event: KeyEvent): Boolean =
        event.keyCode != KeyEvent.KEYCODE_BACK &&
            (event.source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK)

    /** Pads with analog triggers may also send L2/R2 key events: the axis alone drives the
     *  trigger, or the key's full press would cover its travel. */
    private fun analogTriggerKey(gameKey: Int, event: KeyEvent): Boolean {
        if (gameKey != KC_TRIGGER_L && gameKey != KC_TRIGGER_R) return false
        val device = event.device ?: return false
        return listOf(MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
            MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS).any { device.getMotionRange(it) != null }
    }

    /** PC controls switch with the device in use. Keyboards of controllers (their buttons carry
     *  SOURCE_KEYBOARD too) and the on-screen keyboard never count as a PC keyboard. */
    private fun followInputDevice(event: KeyEvent) {
        if (panelNav() != null) return
        if (consumeIfGamepad(event)) {
            usePcControls(false)
            if (event.deviceId != rumbleDeviceId) selectRumbleDevice(event.deviceId)
        } else if (pcKey(event.keyCode) != null &&
            event.source and InputDevice.SOURCE_KEYBOARD == InputDevice.SOURCE_KEYBOARD &&
            event.deviceId != KeyCharacterMap.VIRTUAL_KEYBOARD) {
            usePcControls(true)
        }
    }

    private fun usePcControls(on: Boolean) {
        if (pcInput.enabled == on) return
        releasePcInput(); pcInput.setEnabled(on)
        if (on) updatePcCapture()
    }

    /** A stick or trigger moved well past its resting noise, or the hat pressed. */
    private fun joystickInUse(event: MotionEvent): Boolean =
        listOf(MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
            MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_BRAKE,
            MotionEvent.AXIS_GAS, MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y)
            .any { abs(event.getAxisValue(it)) > 0.3f }

    /** The Xbox button opens and closes the pause menu, like Back. A prompt from the game or
     *  one of our own panels keeps it. */
    private fun toggleMenuFromGuide() {
        when {
            menuOpenState.value -> closeMenuAndResume()
            editingControlsState.value || savestatePickerState.value != null ||
                keyboardRequestState.value != null || discRequestState.value != null ||
                messageBoxRequestState.value != null -> Unit
            else -> openPauseMenu()
        }
    }

    private fun selectRumbleDevice(deviceId: Int) {
        stopRumble()
        rumbleDeviceId = -1
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return
        val ids = runCatching { InputDevice.getDevice(deviceId)?.vibratorManager?.vibratorIds }
            .getOrNull()
        if (ids != null && ids.isNotEmpty()) rumbleDeviceId = deviceId
    }

    private fun setRumbleOn(on: Boolean) {
        rumbleOn.value = on
        controllerPrefs.edit().putBoolean("rumble", on).apply()
    }

    /** Left motor on the first vibrator and right on the second (Android's order for game
     *  controllers); a single vibrator plays the stronger one. Held rumble is re-armed before
     *  each one-shot ends, so it stops by itself within RUMBLE_SHOT_MS if polling stops. */
    private fun applyRumble() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S || rumbleDeviceId < 0) return
        val want = if (rumbleOn.value && session.booted && !uiHoldsPause())
            session.controllerRumble() else 0
        val now = SystemClock.uptimeMillis()
        if (want == rumbleApplied && (want == 0 || now - rumbleArmedAt < RUMBLE_REARM_MS)) return
        val manager = runCatching { InputDevice.getDevice(rumbleDeviceId)?.vibratorManager }
            .getOrNull()
        if (manager == null) {
            rumbleDeviceId = -1; rumbleApplied = 0; return
        }
        val left = want shr 8 and 0xFF
        val right = want and 0xFF
        runCatching {
            // A motor going quiet is left out of the next combination: stop the old one first.
            if (want == 0 || (left == 0 && rumbleApplied shr 8 and 0xFF != 0) ||
                (right == 0 && rumbleApplied and 0xFF != 0)) manager.cancel()
            val ids = manager.vibratorIds
            if (want != 0 && ids.isNotEmpty()) {
                val combination = CombinedVibration.startParallel()
                if (ids.size >= 2) {
                    if (left > 0) combination.addVibrator(ids[0],
                        VibrationEffect.createOneShot(RUMBLE_SHOT_MS, left))
                    if (right > 0) combination.addVibrator(ids[1],
                        VibrationEffect.createOneShot(RUMBLE_SHOT_MS, right))
                } else {
                    combination.addVibrator(ids[0],
                        VibrationEffect.createOneShot(RUMBLE_SHOT_MS, maxOf(left, right)))
                }
                manager.vibrate(combination.combine())
            }
        }.onFailure { Log.w(TAG, "controller rumble failed", it) }
        rumbleApplied = want
        rumbleArmedAt = now
    }

    private fun stopRumble() {
        if (rumbleApplied != 0 && rumbleDeviceId >= 0 &&
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            runCatching { InputDevice.getDevice(rumbleDeviceId)?.vibratorManager?.cancel() }
        }
        rumbleApplied = 0
    }

    /** The SurfaceView listener only fires while that view holds focus. */
    override fun onGenericMotionEvent(event: MotionEvent): Boolean = onGenericMotion(event)

    // Diagnostics for scripted tests: the first joystick samples that reach this window, and
    // what was done with them (one line each, then silent).
    private var joystickSamplesLogged = 0

    /** Joystick axes + hat (D-pad). */
    private fun onGenericMotion(event: MotionEvent): Boolean {
        if (joystickSamplesLogged < 8 &&
            event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK) {
            joystickSamplesLogged++
            Log.i(TAG, "joystick sample: device=${event.deviceId} x=${event.getAxisValue(MotionEvent.AXIS_X)} " +
                "y=${event.getAxisValue(MotionEvent.AXIS_Y)} z=${event.getAxisValue(MotionEvent.AXIS_Z)} " +
                "rz=${event.getAxisValue(MotionEvent.AXIS_RZ)} panel=${panelNav() != null} pc=${pcInput.enabled} " +
                "booted=${session.booted}")
        }
        panelNav()?.let { nav ->
            panelHat(nav, event)
            return true
        }
        val isJoystick = event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
        if (isJoystick && joystickInUse(event)) {
            usePcControls(false)
            if (event.deviceId != rumbleDeviceId) selectRumbleDevice(event.deviceId)
        } else if (!pcInput.enabled &&
            event.source and InputDevice.SOURCE_MOUSE == InputDevice.SOURCE_MOUSE &&
            event.actionMasked == MotionEvent.ACTION_HOVER_MOVE) {
            usePcControls(true)
        }
        if (pcInput.enabled) {
            if (event.source and InputDevice.SOURCE_MOUSE == InputDevice.SOURCE_MOUSE) updatePcCapture()
            return true
        }
        // No early return: sticks/triggers must still process while a hat is held.
        val hatHandled = isNonDpadSource(event) && handleHat(event)
        if (event.source and InputDevice.SOURCE_JOYSTICK != InputDevice.SOURCE_JOYSTICK) {
            return hatHandled || super.onGenericMotionEvent(event)
        }
        emitStick(event.getAxisValue(MotionEvent.AXIS_X), event.getAxisValue(MotionEvent.AXIS_Y),
            KC_LTHUMB_LEFT, KC_LTHUMB_RIGHT, KC_LTHUMB_UP, KC_LTHUMB_DOWN)
        emitStick(event.getAxisValue(MotionEvent.AXIS_Z), event.getAxisValue(MotionEvent.AXIS_RZ),
            KC_RTHUMB_LEFT, KC_RTHUMB_RIGHT, KC_RTHUMB_UP, KC_RTHUMB_DOWN)
        // Most pads report triggers as ANALOG axes (LTRIGGER/RTRIGGER, or BRAKE/GAS).
        emitTrigger(maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
            event.getAxisValue(MotionEvent.AXIS_BRAKE)), KC_TRIGGER_L)
        emitTrigger(maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
            event.getAxisValue(MotionEvent.AXIS_GAS)), KC_TRIGGER_R)
        return true
    }

    /** Analog trigger -> XInput's 0-255, the whole travel (throttle and brake in driving
     *  games). Pressed, for keystrokes, from XInput's own threshold. */
    private fun emitTrigger(value: Float, gameKey: Int) {
        val v = (value.coerceIn(0f, 1f) * 255f).roundToInt().let { if (it < TRIGGER_REST) 0 else it }
        emitAxis(gameKey, v >= TRIGGER_PRESSED, v)
    }

    /** One stick with a round dead zone: inside it both axes read exactly zero (hardware drift
     *  never reaches native); outside it both pass untouched, so a slight diagonal is kept
     *  instead of snapping to the axis. The game applies its own dead zone on top, as on a 360. */
    private fun emitStick(x: Float, y: Float, left: Int, right: Int, up: Int, down: Int) {
        val rest = x * x + y * y < AXIS_DEADZONE * AXIS_DEADZONE
        emitAxisPair(if (rest) 0f else x, left, right, invert = false)
        emitAxisPair(if (rest) 0f else y, up, down, invert = true)
    }

    /** For one axis emit the opposing thumb directions, scaled to signed-short range.
     *  invert flips sign first (screen Y is up-negative; X360 up is positive). */
    private fun emitAxisPair(axis: Float, negKey: Int, posKey: Int, invert: Boolean) {
        val v = (if (invert) -axis else axis).coerceIn(-1f, 1f)
        when {
            v < 0f -> {
                emitAxis(posKey, false, 0)
                emitAxis(negKey, true, (v * 32768f).toInt())
            }
            v > 0f -> {
                emitAxis(negKey, false, 0)
                emitAxis(posKey, true, (v * 32767f).toInt())
            }
            else -> {
                emitAxis(negKey, false, 0)
                emitAxis(posKey, false, 0)
            }
        }
    }

    // Last value pushed per analog code: motion events arrive per sample (~120Hz x 4 axes)
    // and mostly repeat, so without this every sample costs ~10 JNI calls.
    private val axisPressed = BooleanArray(24)
    private val axisValue = IntArray(24) { Int.MIN_VALUE }

    private fun emitAxis(code: Int, pressed: Boolean, value: Int) {
        if (axisPressed[code] == pressed && axisValue[code] == value) return
        axisPressed[code] = pressed
        axisValue[code] = value
        session.keyEvent(code, pressed, value)
    }

    // Hat D-pad state, edge-detected: the hat only releases what IT pressed, so it can't
    // clobber a D-pad held via real KEYCODE_DPAD_* key events.
    private var hatLeft = false
    private var hatUp = false
    private var hatRight = false
    private var hatDown = false

    /** Hat axes -> D-pad; thresholds, not ==+-1f (some pads are inexact). */
    private fun handleHat(event: MotionEvent): Boolean {
        val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        val left = hx < -0.5f
        val right = hx > 0.5f
        val up = hy < -0.5f
        val down = hy > 0.5f
        if (left != hatLeft) {
            session.keyEvent(KC_DPAD_LEFT, left, KEY_VALUE_UNUSED); hatLeft = left
        }
        if (right != hatRight) {
            session.keyEvent(KC_DPAD_RIGHT, right, KEY_VALUE_UNUSED); hatRight = right
        }
        if (up != hatUp) {
            session.keyEvent(KC_DPAD_UP, up, KEY_VALUE_UNUSED); hatUp = up
        }
        if (down != hatDown) {
            session.keyEvent(KC_DPAD_DOWN, down, KEY_VALUE_UNUSED); hatDown = down
        }
        return left || right || up || down
    }

    /** TRUE when the device is NOT a SOURCE_DPAD (treat as joystick/hat). */
    private fun isNonDpadSource(event: MotionEvent): Boolean =
        event.source and InputDevice.SOURCE_DPAD != InputDevice.SOURCE_DPAD

    // Guest-panel navigation is routed here rather than through Compose focus because on
    // this hardware the D-pad arrives as HAT AXES on a MotionEvent, never reaching a composable.

    /** The open panel's selectable options. Checked in z-order, topmost first; null when no
     *  panel is open. */
    private class PanelNav(
        val count: Int,
        val activate: (Int) -> Unit,
        val cancel: () -> Unit,
        /** Left/right (-1/+1) when the panel uses them for something other than prev/next. */
        val onHorizontal: ((Int) -> Unit)? = null,
        /** LB/RB (-1/+1), where the panel has tabs. */
        val onShoulder: ((Int) -> Unit)? = null,
        /** Up/down (-1/+1) when some entries cannot hold the highlight: returns the new index. */
        val moveFrom: ((Int, Int) -> Int)? = null,
        /** Y on the highlighted entry, where the panel has a second action. */
        val onSecondary: ((Int) -> Unit)? = null,
    )

    private fun panelNav(): PanelNav? {
        discRequestState.value?.let { req ->
            val paths = req.discPaths ?: emptyArray()
            val discs = minOf(req.discLabels?.size ?: 0, paths.size)
            // Cancel is the last option.
            return PanelNav(discs + 1,
                { i -> if (i < discs) chooseDisc(paths[i]) else cancelDisc() },
                { cancelDisc() })
        }
        messageBoxRequestState.value?.let { req ->
            return PanelNav(req.buttons?.size ?: 0,
                { i -> answerMessageBox(i) },
                { answerMessageBox(req.activeButton) })
        }
        keyboardRequestState.value?.let {
            return PanelNav(2,
                { i -> if (i == 0) acceptKeyboard(keyboardTextState.value) else cancelKeyboard() },
                { cancelKeyboard() })
        }
        savestatePickerState.value?.let { entries ->
            // Cancel is the last option.
            return PanelNav(entries.size + 1,
                { i -> if (i < entries.size) loadSavestate(entries[i]) else closeSavestatePicker() },
                { closeSavestatePicker() },
                onSecondary = { i -> if (i < entries.size) deleteSavestate(i) })
        }
        // Last: a guest prompt opened over the menu owns the input until it is answered.
        if (menuOpenState.value) {
            val tabs = quickMenuTabs()
            val tab = tabs[menuTabState.intValue.coerceIn(0, tabs.lastIndex)]
            return PanelNav(tab.items.size,
                { i -> tab.items.getOrNull(i)?.let { qmActivate(it) } },
                { closeMenuAndResume() },
                onHorizontal = { dir ->
                    val item = tab.items.getOrNull(panelSelectedState.intValue)
                    if (item == null || !qmAdjust(item, dir)) {
                        selectMenuTab(qmWrap(menuTabState.intValue, dir, tabs.size))
                    }
                },
                onShoulder = { dir -> selectMenuTab(qmWrap(menuTabState.intValue, dir, tabs.size)) },
                moveFrom = { index, delta -> qmMoveSelection(tab.items, index, delta) })
        }
        return null
    }

    /** Flips the live cvar for an immediate effect and persists it, leaving the menu open so
     *  the result is visible behind it. */
    private fun toggleTouchOverlay() {
        val next = showTouchOverlay.value != true
        showTouchOverlay.value = next
        session.setShowTouchOverlay(next)
        lifecycleScope.launch(Dispatchers.IO) {
            runCatching {
                val handle = ConfigStore(applicationContext).openLive()
                handle.putBool("HID", "show_touch_overlay", next)
                handle.closeFile()
            }.onFailure { Log.w(TAG, "persisting show_touch_overlay failed", it) }
        }
    }

    /** Reads the switches from the global config without rewriting it. A missing key means
     *  the core default, which is on. */
    private fun loadHeatOpts() {
        lifecycleScope.launch(Dispatchers.IO) {
            runCatching {
                val handle = ConfigStore(applicationContext).openLiveSnapshot()
                try {
                    val enabled = handle.getBool("Kernel", "savestate_enable", false)
                    withContext(Dispatchers.Main) { snapshotEnabled.value = enabled }
                    heatOpts.associate { o ->
                        o.id to o.cvars.all { c ->
                            if (c.isInt) c.isOn(handle.getInt(c.section, c.name,
                                if (o.optional) c.offValue else onValueOf(c)))
                            else handle.getBool(c.section, c.name, if (o.optional) !c.onBool else c.onBool) == c.onBool
                        }
                    }
                } finally {
                    handle.closeString()
                }
            }.onSuccess {
                if (heatOptsAtLaunch == null) heatOptsAtLaunch = it
                heatOptsState.value = it
            }.onFailure { Log.w(TAG, "reading heat options failed", it) }
        }
    }

    /** Set once a switch has been handed to the running game: from then on the live file
     *  holds the switches too and the start-up write must not replace it. */
    @Volatile private var liveTogglesWritten = false
    private var liveExtrasSeconds = 0

    /** What the running game needs to know about this device, sent through the live file.
     *  The frame limiter's intermediate step (40 FPS under a 60 limit) is only even on a
     *  display with a mode it divides (120 Hz), so it is offered only then. */
    private fun liveExtras(): Map<String, Any> {
        val cap = bootDisplayTargetFps.toInt()
        val mid = if (cap >= 60 && cap % 3 == 0) cap * 2 / 3 else 0
        val supported = mid > 0 && runCatching {
            val d = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display
                else @Suppress("DEPRECATION") windowManager.defaultDisplay
            d?.supportedModes?.any { m ->
                val hz = Math.round(m.refreshRate)
                hz >= mid && hz % mid == 0
            } == true
        }.getOrDefault(false)
        return mapOf("framerate_limit_adaptive_mid" to (if (supported) mid else 0))
    }

    /** Start-up hand-off of [liveExtras]. The core deletes the live file while it boots, so
     *  this runs a few seconds in, and once more in case the first write came too early. */
    private fun writeLiveExtras() {
        if (liveTogglesWritten) return
        lifecycleScope.launch(Dispatchers.IO) {
            runCatching {
                val extras = liveExtras()
                if (!liveTogglesWritten) {
                    ConfigStore(applicationContext).globalConfigFile().parentFile
                        ?.let { LiveCvars.write(it, extras) }
                }
            }.onFailure { Log.w(TAG, "writing live extras failed", it) }
        }
    }

    private fun heatOptOn(states: Map<String, Boolean>, o: HeatOpt) =
        states[o.id] ?: !o.optional

    /** Persists the given switches (for the next launches) and hands every live one to the
     *  running game, which applies them within about a second. */
    private fun setHeatOpts(changes: Map<HeatOpt, Boolean>) {
        val profile = heatOpts.first { it.id == "limiter" }
        val cap30 = heatOpts.first { it.id == "fps30" }
        val resolved = changes.toMutableMap()
        // Turning the game profile off must also release a previous manual cap.
        if (changes[profile] == false) resolved[cap30] = false
        if (changes[cap30] == true) resolved[profile] = true
        val states = heatOptsState.value + resolved.mapKeys { it.key.id }
        val frameLimit = if (heatOptOn(states, cap30)) 30
            else if (heatOptOn(states, profile)) onValueOf(HeatCvar("GPU", "framerate_limit", onValue = PANEL_MAX_HZ))
            else 0
        heatOptsState.value = states
        lifecycleScope.launch(Dispatchers.IO) {
            runCatching {
                val store = ConfigStore(applicationContext)
                val handle = store.openLive()
                for ((o, on) in resolved) {
                    for (c in o.cvars) {
                        if (c.isInt) handle.putInt(c.section, c.name, if (on) onValueOf(c) else c.offValue)
                        // Off writes the opposite of onBool, so an onBool=false cvar
                        // can be switched off at all.
                        else handle.putBool(c.section, c.name, if (on) c.onBool else !c.onBool)
                    }
                }
                // Both rows refer to this cvar: an unchecked 30-FPS row must
                // not overwrite the unlimited profile with its old 60 default.
                handle.putInt("GPU", "framerate_limit", frameLimit)
                handle.closeFile()
                val live = buildMap<String, Any> {
                    putAll(liveExtras())
                    for (o in heatOpts.filter { it.live }) {
                        val on = heatOptOn(states, o)
                        for (c in o.cvars) {
                            put(c.name, if (c.isInt) (if (on) onValueOf(c) else c.offValue) else (if (on) c.onBool else !c.onBool))
                        }
                    }
                    put("framerate_limit", frameLimit)
                }
                liveTogglesWritten = true
                store.globalConfigFile().parentFile?.let { LiveCvars.write(it, live) }
            }.onFailure { Log.w(TAG, "applying heat options failed", it) }
        }
    }

    private fun openPauseMenu() {
        loadHeatOpts()
        panelSelectedState.intValue = 0
        menuTabState.intValue = MENU_TAB_GAME
        quitArmedState.value = false
        menuStatusState.value = sessionStatusLine()
        menuOpenState.value = true
        if (session.booted) session.pause()
    }

    private fun runtimeSnapshotOperation(operation: Int, target: File? = null,
                                         onDone: ((Boolean) -> Unit)? = null) {
        if (snapshotBusy.value) return
        val title = currentTitleId?.takeIf { it.matches(Regex("[0-9a-fA-F]{8}")) }
        if (operation != 0 && title == null) {
            Toast.makeText(this, l10n("Game title not available", "Titolo del gioco non disponibile"), Toast.LENGTH_LONG).show()
            return
        }
        val folder = ConfigStore(applicationContext).globalConfigFile().parentFile
        if (folder == null) return
        val stateFile = target ?: File(folder, "states/${title ?: "probe"}/slot-1.xds")
        snapshotBusy.value = true
        if (operation != 0) {
            // The runtime barrier needs running guest threads. Closing the menu
            // resumes the lifecycle pause; the core takes its own canonical barrier.
            releasePcInput()
            closeMenuAndResume()
            Toast.makeText(this, if (operation == 1) l10n("Saving state…", "Salvataggio dello stato…") else l10n("Loading state…", "Caricamento dello stato…"),
                Toast.LENGTH_LONG).show()
        }
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching { session.runtimeState(operation, stateFile.absolutePath) }
                    .getOrElse { it.message ?: l10n("Operation failed", "Operazione non riuscita") }
            }
            snapshotBusy.value = false
            if (session.booted) {
                if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED) && !uiHoldsPause())
                    session.resumeIfPaused()
                else session.pause()
            }
            val message = if (result.isBlank()) when (operation) {
                0 -> l10n("Instant states available", "Stati istantanei disponibili")
                1 -> l10n("State saved", "Stato salvato")
                else -> l10n("State loaded", "Stato caricato")
            } else snapshotReasonText(result)
            snapshotStatus.value = message
            Log.i(TAG, "RuntimeState result operation=$operation ok=${result.isBlank()} path=${stateFile.absolutePath} reason=$result")
            onDone?.invoke(result.isBlank())
            Toast.makeText(this@EmulatorHostActivity, message, Toast.LENGTH_LONG).show()
        }
    }

    private fun savestateFolder(): File? {
        val title = currentTitleId?.takeIf { it.matches(Regex("[0-9a-fA-F]{8}")) } ?: return null
        val folder = ConfigStore(applicationContext).globalConfigFile().parentFile ?: return null
        return File(folder, "states/$title")
    }

    /** Every saved state of this game (slot-*.xds; the core's .rollback.xds is not one),
     *  newest first, with the picture saved next to it. */
    private fun listSavestates(): List<SavestateEntry> {
        val folder = savestateFolder() ?: return emptyList()
        return (folder.listFiles() ?: emptyArray())
            .filter { it.isFile && it.name.startsWith("slot-") && it.name.endsWith(".xds") }
            .map { f ->
                val picture = File(f.parentFile, f.name.removeSuffix(".xds") + ".jpg")
                SavestateEntry(f, picture.takeIf { it.isFile }, f.lastModified(), f.length())
            }
            .sortedByDescending { it.savedAt }
    }

    /** A new saved state each time (named by date), with a picture of the paused frame. */
    private fun saveSavestateWithPicture() {
        if (snapshotBusy.value) return
        val folder = savestateFolder()
        if (folder == null) {
            Toast.makeText(this, l10n("Game title not available", "Titolo del gioco non disponibile"), Toast.LENGTH_LONG).show()
            return
        }
        val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
            .format(java.util.Date())
        val stateFile = File(folder, "slot-$stamp.xds")
        val pictureFile = File(folder, "slot-$stamp.jpg")
        captureFrame(480) { bitmap ->
            runtimeSnapshotOperation(1, stateFile) { ok ->
                val picture = bitmap ?: return@runtimeSnapshotOperation
                lifecycleScope.launch(Dispatchers.IO) {
                    if (ok) runCatching {
                        val pending = File(folder, pictureFile.name + ".pending")
                        pending.outputStream().use {
                            picture.compress(android.graphics.Bitmap.CompressFormat.JPEG, 85, it)
                        }
                        pending.renameTo(pictureFile)
                    }.onFailure { Log.w(TAG, "savestate picture failed", it) }
                    picture.recycle()
                }
            }
        }
    }

    /** The frame on the game surface, scaled to [width] pixels wide (null if it can't be
     *  read). The menu is a separate view, so it is not in the picture. */
    private fun captureFrame(width: Int, onResult: (android.graphics.Bitmap?) -> Unit) {
        val sv = surfaceView
        if (sv == null || sv.width <= 0 || sv.height <= 0 || !sv.holder.surface.isValid) {
            onResult(null); return
        }
        val full = android.graphics.Bitmap.createBitmap(
            sv.width, sv.height, android.graphics.Bitmap.Config.ARGB_8888)
        runCatching {
            android.view.PixelCopy.request(sv, full, { result ->
                if (result != android.view.PixelCopy.SUCCESS) {
                    full.recycle(); onResult(null); return@request
                }
                val height = (full.height.toLong() * width / full.width).toInt().coerceAtLeast(1)
                val scaled = android.graphics.Bitmap.createScaledBitmap(full, width, height, true)
                if (scaled !== full) full.recycle()
                onResult(scaled)
            }, mainHandler)
        }.onFailure { full.recycle(); onResult(null) }
    }

    private fun openSavestatePicker() {
        if (snapshotBusy.value) return
        val entries = listSavestates()
        if (entries.isEmpty()) {
            Toast.makeText(this, l10n("No saved state for this game", "Nessuno stato salvato per questo gioco"), Toast.LENGTH_LONG).show()
            return
        }
        releasePcInput()
        savestateDeleteArmed.intValue = -1
        panelSelectedState.intValue = 0
        savestatePickerState.value = entries
    }

    private fun closeSavestatePicker() {
        savestatePickerState.value = null
        savestateDeleteArmed.intValue = -1
        // Back to the menu row that opened the picker.
        panelSelectedState.intValue = 0
    }

    private fun loadSavestate(entry: SavestateEntry) {
        savestatePickerState.value = null
        savestateDeleteArmed.intValue = -1
        runtimeSnapshotOperation(2, entry.file)
    }

    /** First press arms, the second deletes the state and its picture. */
    private fun deleteSavestate(index: Int) {
        val entries = savestatePickerState.value ?: return
        val entry = entries.getOrNull(index) ?: return
        if (savestateDeleteArmed.intValue != index) {
            savestateDeleteArmed.intValue = index
            return
        }
        entry.file.delete()
        entry.thumbnail?.delete()
        savestateDeleteArmed.intValue = -1
        val left = listSavestates()
        if (left.isEmpty()) {
            closeSavestatePicker()
            Toast.makeText(this, l10n("State deleted", "Stato eliminato"), Toast.LENGTH_SHORT).show()
        } else {
            savestatePickerState.value = left
            panelSelectedState.intValue = index.coerceAtMost(left.size)
        }
    }

    /** The core reports why a save/load can't run in technical English: say it the way a
     *  player can act on. Unknown reasons are shown as they are. */
    private fun snapshotReasonText(reason: String): String = when {
        reason.contains("must be enabled before boot") ->
            l10n("Turn on “Instant states” and restart the game", "Attiva «Stati istantanei» e riavvia il gioco")
        reason.contains("no saved state") -> l10n("No saved state for this game", "Nessuno stato salvato per questo gioco")
        reason.contains("mismatch") ->
            l10n("Save made with another version or other settings: it cannot be loaded", "Stato salvato con un'altra versione o altre impostazioni: non si può caricare")
        reason.contains("unsupported kernel object") || reason.contains("socket ") ->
            l10n("This game uses a feature not yet supported by states ($reason)", "Questo gioco usa una funzione non ancora supportata dagli stati ($reason)")
        reason.contains("plugins") -> l10n("States are not available while game plugins are active", "Con i plugin del gioco attivi gli stati non sono disponibili")
        reason.contains("content packages") ->
            l10n("States are not available while additional content is in use", "Con contenuti aggiuntivi in uso gli stati non sono disponibili")
        reason.contains("timed out") || reason.contains("did not reach") ->
            l10n("The game did not pause in time: try again in a moment", "Il gioco non si è fermato in tempo: riprova tra un attimo")
        reason.contains("not enough memory") -> l10n("Not enough memory to load the state", "Memoria insufficiente per caricare lo stato")
        reason.contains("already running") -> l10n("Operation already in progress", "Operazione già in corso")
        else -> l10n("Operation failed: $reason", "Operazione non riuscita: $reason")
    }

    private fun setSnapshotEnabled(enabled: Boolean) {
        snapshotEnabled.value = enabled
        lifecycleScope.launch(Dispatchers.IO) {
            runCatching {
                val handle = ConfigStore(applicationContext).openLive()
                handle.putBool("Kernel", "savestate_enable", enabled)
                handle.closeFile()
            }.onFailure { Log.w(TAG, "saving snapshot option failed", it) }
        }
    }

    private fun closeMenuAndResume() {
        menuOpenState.value = false
        quitArmedState.value = false
        if (session.booted) session.resumeIfPaused()
    }

    /** Menu -> editor: the game stays paused underneath. */
    private fun openControlsEditor() {
        quitArmedState.value = false
        menuOpenState.value = false
        editingControlsState.value = true
    }

    private fun closeControlsEditor() {
        if (!editingControlsState.value) return
        editingControlsState.value = false
        menuTabState.intValue = MENU_TAB_CONTROLS
        // Back on the "Edit layout" row it was opened from.
        panelSelectedState.intValue = quickMenuTabs()[MENU_TAB_CONTROLS].items
            .indexOfFirst { it.title == l10n("Edit layout", "Modifica la disposizione") }.coerceAtLeast(0)
        menuOpenState.value = true
    }

    private fun quitOrArm() {
        if (quitArmedState.value) finish() else quitArmedState.value = true
    }

    /** Rewrites the persisted touch-controls globals; the overlay follows the store's flow. */
    private fun updateGamepadGlobals(transform: (GamepadGlobalsDto) -> GamepadGlobalsDto) {
        val cfg = gamepadCfgState.value ?: return        // not loaded yet: nothing to rewrite
        saveGamepadConfig(cfg.copy(globals = transform(cfg.globals)))
    }

    private fun saveGamepadConfig(next: GamepadConfigDto) {
        gamepadCfgState.value = next
        lifecycleScope.launch { gamepad.save(next) }
    }

    /** Menu: every control of the layout on screen grows or shrinks together. */
    private fun scaleAllControls(factor: Float) {
        val cfg = gamepadCfgState.value ?: return
        val landscape = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
        val scaled = gamepad.controlsFor(cfg, landscape)
            .map { it.withLayout(s = (it.scale * factor).coerceIn(0.5f, 3f)) }.toDto()
        saveGamepadConfig(if (landscape) cfg.copy(landscape = scaled) else cfg.copy(portrait = scaled))
    }

    private fun selectMenuTab(index: Int) {
        if (index == menuTabState.intValue) return
        menuTabState.intValue = index
        panelSelectedState.intValue =
            quickMenuTabs().getOrNull(index)?.let { qmFirstSelectable(it.items) } ?: 0
        quitArmedState.value = false
    }

    /** "33 FPS · 38.5 °C · battery 78% · 11:20" for the menu header. */
    private fun sessionStatusLine(): String {
        val parts = mutableListOf<String>()
        val fps = session.averageFps()
        if (fps > 0.0) parts += "${fps.roundToInt()} FPS"
        runCatching {
            registerReceiver(null, android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED))
        }.getOrNull()?.let { b ->
            val t = b.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, -1)
            if (t > 0) parts += String.format(java.util.Locale.US, "%.1f °C", t / 10f)
            val level = b.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1)
            val scale = b.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, 100)
            if (level >= 0 && scale > 0) parts += l10n("battery ${level * 100 / scale}%", "batteria ${level * 100 / scale}%")
        }
        parts += java.text.SimpleDateFormat("HH:mm", java.util.Locale.US).format(java.util.Date())
        return parts.joinToString("  ·  ")
    }

    private fun setScreenBrightness(v: Float) {
        screenBrightnessState.floatValue = v
        window.attributes = window.attributes.apply {
            screenBrightness = if (v < 0.03f) WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
            else v.coerceIn(0.03f, 1f)
        }
    }

    /** Saves the frame on screen (the game is paused under the menu) to Pictures/XenDroid. */
    private fun takeScreenshot() {
        val sv = surfaceView ?: return
        if (sv.width <= 0 || sv.height <= 0 || !sv.holder.surface.isValid) {
            Toast.makeText(this, l10n("Nothing to capture yet", "Ancora niente da catturare"), Toast.LENGTH_SHORT).show(); return
        }
        val bitmap = android.graphics.Bitmap.createBitmap(
            sv.width, sv.height, android.graphics.Bitmap.Config.ARGB_8888)
        runCatching {
            android.view.PixelCopy.request(sv, bitmap, { result ->
                if (result != android.view.PixelCopy.SUCCESS) {
                    Toast.makeText(this, l10n("Image not saved", "Immagine non salvata"), Toast.LENGTH_SHORT).show()
                    return@request
                }
                lifecycleScope.launch(Dispatchers.IO) {
                    val saved = runCatching {
                        val stamp = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US)
                            .format(java.util.Date())
                        val base = currentGameName.ifBlank { currentTitleId ?: "XenDroid" }
                            .replace(Regex("[^A-Za-z0-9 _-]"), "").trim().take(40).ifBlank { "XenDroid" }
                        val values = android.content.ContentValues().apply {
                            put(android.provider.MediaStore.Images.Media.DISPLAY_NAME, "${base}_$stamp.png")
                            put(android.provider.MediaStore.Images.Media.MIME_TYPE, "image/png")
                            put(android.provider.MediaStore.Images.Media.RELATIVE_PATH, "Pictures/XenDroid")
                        }
                        val uri = contentResolver.insert(
                            android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                            ?: error("no media uri")
                        contentResolver.openOutputStream(uri)!!.use {
                            bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it)
                        }
                    }
                    bitmap.recycle()
                    mainHandler.post {
                        Toast.makeText(this@EmulatorHostActivity,
                            if (saved.isSuccess) l10n("Image saved to Pictures/XenDroid", "Immagine salvata in Pictures/XenDroid")
                            else l10n("Image not saved: ${saved.exceptionOrNull()?.message}", "Immagine non salvata: ${saved.exceptionOrNull()?.message}"),
                            Toast.LENGTH_SHORT).show()
                    }
                }
            }, mainHandler)
        }.onFailure { Toast.makeText(this, l10n("Image not saved", "Immagine non salvata"), Toast.LENGTH_SHORT).show() }
    }

    /** The menu's content, built from the live state. Called from composition (so the panel
     *  follows every change) and from the controller path (so both act on the same rows). */
    private fun quickMenuTabs(): List<QmTab> {
        val g = gamepadCfgState.value?.globals ?: GamepadGlobalsDto()
        fun pct(v: Float) = "${(v * 100).roundToInt()}%"
        val graphics = graphicsPresetNames.value
        val game = listOf(
            QmAction(l10n("Resume", "Riprendi"), l10n("Back to the game", "Torna al gioco"), Icons.Filled.PlayArrow, primary = true) {
                closeMenuAndResume()
            },
            QmSection(l10n("Images and saves", "Immagini e salvataggi")),
            QmToggle(l10n("Instant states", "Stati istantanei"), l10n("From the next game launch", "Dal prossimo avvio del gioco"), Icons.Filled.Lock,
                checked = snapshotEnabled.value, nextLaunch = true) { setSnapshotEnabled(it) },
            QmAction(l10n("Save state", "Salva stato"), l10n("New save of this point, with date and picture", "Nuovo salvataggio di questo punto, con data e immagine"),
                Icons.Filled.Lock) {
                saveSavestateWithPicture()
            },
            QmAction(l10n("Load state", "Carica stato"), l10n("Choose which save to resume from", "Scegli da quale salvataggio riprendere"), Icons.Filled.Refresh) {
                openSavestatePicker()
            },
            QmNote(if (snapshotBusy.value) l10n("Operation in progress…", "Operazione in corso…") else snapshotStatus.value.ifBlank {
                l10n("States apply to the same game and the same settings", "Gli stati valgono con lo stesso gioco e le stesse impostazioni")
            }),
            QmAction(l10n("Save image", "Salva immagine"), l10n("Save this image to Pictures/XenDroid", "Salva questa immagine in Pictures/XenDroid"), QmIcons.Camera) {
                takeScreenshot()
            },
            QmAction(l10n("Copy saves", "Copia i salvataggi"), l10n("Copy saves to Download/XenDroid-Saves", "Copia i salvataggi in Download/XenDroid-Saves"),
                Icons.Filled.Lock) { backupSaves() },
            QmSection(l10n("Session", "Partita")),
            QmAction(
                if (quitArmedState.value) l10n("Press again to quit", "Premi di nuovo per uscire") else l10n("Back to library", "Torna alla libreria"),
                if (quitArmedState.value) l10n("Unsaved progress will be lost", "I progressi non salvati andranno persi")
                else l10n("Closes the game after a second confirmation", "Chiude il gioco dopo una seconda conferma"),
                Icons.Filled.ExitToApp, danger = true,
            ) { quitOrArm() },
        )
        val controls = listOf(
            QmSection("Controller"),
            QmToggle(l10n("Controller rumble", "Vibrazione del controller"), l10n("The controller vibrates when the game asks for it", "Il controller vibra quando il gioco lo chiede"),
                QmIcons.Vibration, checked = rumbleOn.value) { setRumbleOn(it) },
            QmNote(l10n("Xbox button: opens and closes this menu", "Tasto Xbox: apre e chiude questo menu"),
                l10n("Analog triggers: throttle and brake follow pressure", "Grilletti analogici: gas e freno seguono la pressione"), Icons.Filled.Info),
            QmSection(l10n("Sensitivity", "Sensibilità")),
            QmSlider(l10n("Look sensitivity", "Sensibilità della visuale"), l10n("Speed of the right stick and of aiming with RT", "Velocità dello stick destro e della mira con RT"),
                QmIcons.Target, g.lookSensitivity, 0.3f, 2f, 0.05f, pct(g.lookSensitivity)) { v ->
                updateGamepadGlobals { it.copy(lookSensitivity = v) }
            },
            QmSlider(l10n("Move sensitivity", "Sensibilità del movimento"), l10n("Left stick travel up to full speed", "Corsa dello stick sinistro fino alla velocità piena"),
                QmIcons.Stick, g.moveSensitivity, 0.3f, 2f, 0.05f, pct(g.moveSensitivity)) { v ->
                updateGamepadGlobals { it.copy(moveSensitivity = v) }
            },
            QmChoice(l10n("Look curve", "Curva della visuale"), l10n("Precise slows the camera near the centre for small corrections", "Precisa rallenta la telecamera vicino al centro per le piccole correzioni"),
                QmIcons.Target, if (g.lookCurve > 1.01f) l10n("Precise", "Precisa") else l10n("Linear", "Lineare")) {
                updateGamepadGlobals { it.copy(lookCurve = if (it.lookCurve > 1.01f) 1f else 1.6f) }
            },
            QmSlider(l10n("Stick response", "Risposta dello stick"), l10n("Reduces movement with no response; 0% turns it off", "Riduce il movimento senza risposta; 0% disattiva"),
                QmIcons.Stick, g.stickDeadzoneComp, 0f, 0.3f, 0.01f, pct(g.stickDeadzoneComp)) { v ->
                updateGamepadGlobals { it.copy(stickDeadzoneComp = v) }
            },
            QmSection(l10n("Touch controls", "Comandi touch")),
            QmToggle(l10n("Show touch controls", "Mostra i comandi touch"), l10n("The on-screen controller", "Il controller sullo schermo"), QmIcons.Gamepad,
                checked = showTouchOverlay.value == true) { toggleTouchOverlay() },
            QmAction(l10n("Edit layout", "Modifica la disposizione"), l10n("Move, resize or hide each control over the game", "Sposta, ridimensiona o nascondi ogni comando sopra il gioco"), Icons.Filled.Edit) {
                openControlsEditor()
            },
            QmAction(l10n("Enlarge all controls", "Ingrandisci tutti i comandi"), l10n("Each control grows by 10%", "Ogni comando cresce del 10%"), Icons.Filled.Add) {
                scaleAllControls(1.1f)
            },
            QmAction(l10n("Shrink all controls", "Rimpicciolisci tutti i comandi"), l10n("Each control shrinks by 10%", "Ogni comando cala del 10%"), Icons.Filled.Clear) {
                scaleAllControls(1f / 1.1f)
            },
            QmSlider(l10n("Opacity", "Opacità"), l10n("How visible the controls are", "Quanto si vedono i comandi"), QmIcons.Opacity,
                g.opacity, 0.2f, 1f, 0.05f, pct(g.opacity)) { v ->
                updateGamepadGlobals { it.copy(opacity = v) }
            },
            QmSlider(l10n("Auto-hide", "Nascondi da soli"), l10n("They fade after this time without touches", "Svaniscono dopo questo tempo senza tocchi"),
                QmIcons.Timer, g.autoHideSeconds, 0f, 20f, 1f,
                if (g.autoHideSeconds < 0.5f) l10n("Never", "Mai") else "${g.autoHideSeconds.roundToInt()} s") { v ->
                updateGamepadGlobals { it.copy(autoHideSeconds = v) }
            },
            QmSection(l10n("Touch behaviour", "Comportamento del touch")),
            QmToggle(l10n("Aim while holding RT", "Mira tenendo RT"), l10n("Drag the firing thumb to move the camera", "Trascina il pollice che spara per muovere la telecamera"), QmIcons.Target,
                checked = g.aimWithTrigger) { v -> updateGamepadGlobals { it.copy(aimWithTrigger = v) } },
            QmToggle(l10n("Right stick anywhere", "Stick destro ovunque"), l10n("Tap an empty area on the right to look around", "Tocca uno spazio libero a destra per guardarti intorno"), QmIcons.Stick,
                checked = g.floatingRightStick) { v -> updateGamepadGlobals { it.copy(floatingRightStick = v) } },
            QmToggle(l10n("Left stick anywhere", "Stick sinistro ovunque"), l10n("Tap an empty area on the left to move", "Tocca uno spazio libero a sinistra per muoverti"), QmIcons.Stick,
                checked = g.floatingLeftStick) { v -> updateGamepadGlobals { it.copy(floatingLeftStick = v) } },
            QmToggle(l10n("Hold LT", "Mantieni LT premuto"), l10n("Tap LT to hold it down, tap again to release", "Tocca LT per tenerlo premuto, ritocca per rilasciarlo"), Icons.Filled.Lock,
                checked = g.ltToggle) { v -> updateGamepadGlobals { it.copy(ltToggle = v) } },
            QmToggle(l10n("Slide across A / B / X / Y", "Scorri tra A / B / X / Y"), l10n("Slide the thumb from one button to another", "Fai scivolare il pollice da un tasto all'altro"),
                QmIcons.Gamepad,
                checked = g.faceButtonSlide) { v -> updateGamepadGlobals { it.copy(faceButtonSlide = v) } },
            QmToggle(l10n("Touch haptics", "Vibrazione del touch"), l10n("A short click for each button press", "Un breve scatto a ogni tasto premuto"), QmIcons.Vibration,
                checked = g.hapticsEnabled) { v -> updateGamepadGlobals { it.copy(hapticsEnabled = v) } },
        )
        val heat = heatOptsState.value
        val heatAtLaunch = heatOptsAtLaunch
        val limiter = heatOpts.first { it.id == "limiter" }
        val cap30 = heatOpts.first { it.id == "fps30" }
        // The frame-rate limit has its own control; these are the switches under it.
        val advanced = heatOpts.filter { !it.optional && it.id != "limiter" }
        val advancedAllOn = advanced.all { heatOptOn(heat, it) }
        // Only the switches the running game can't take on the fly stay pending.
        val heatPending = heatAtLaunch != null && heatOpts.any {
            !it.live && heatOptOn(heat, it) != heatOptOn(heatAtLaunch, it)
        }
        val fpsMode = when {
            heatOptOn(heat, cap30) -> 1
            heatOptOn(heat, limiter) -> 0
            else -> 2
        }
        val stats = buildList {
            add(QmSection(l10n("Frame rate", "Fluidità")))
            add(QmSegmented(l10n("FPS limit", "Limite FPS"),
                when (fpsMode) {
                    0 -> l10n("Steady pace at the best rate the game can hold. Recommended",
                        "Ritmo costante al massimo che il gioco regge. Consigliato")
                    1 -> l10n("Half the work in 60 FPS games: cooler phone, longer battery",
                        "Metà del lavoro nei giochi a 60: telefono più fresco, batteria più lunga")
                    else -> l10n("As many FPS as possible. Some games may run too fast",
                        "Più FPS possibili. Alcuni giochi possono andare troppo veloci")
                },
                QmIcons.Chart,
                options = listOf("Auto", "30", l10n("Free", "Libero")),
                selected = fpsMode,
            ) { m ->
                when (m) {
                    0 -> setHeatOpts(mapOf(limiter to true, cap30 to false))
                    1 -> setHeatOpts(mapOf(cap30 to true))
                    else -> setHeatOpts(mapOf(limiter to false))
                }
            })
            addAll(statsOverlayRows())
            add(QmSection(l10n("Advanced: all on is best", "Avanzate: meglio tutto acceso")))
            for (o in advanced) {
                val on = heatOptOn(heat, o)
                add(QmToggle(o.title, o.subtitle,
                    if (o.id == "governor") QmIcons.Thermo else QmIcons.Chart,
                    checked = on,
                    nextLaunch = !o.live && heatAtLaunch != null &&
                        on != heatOptOn(heatAtLaunch, o),
                ) { v -> setHeatOpts(mapOf(o to v)) })
            }
            if (!advancedAllOn) {
                add(QmAction(l10n("Turn all on", "Riattiva tutto"), l10n("Recommended setting: all entries on", "Impostazione consigliata: tutte le voci accese"),
                    Icons.Filled.Refresh) { setHeatOpts(advanced.associateWith { true }) })
            }
            add(QmNote(
                when {
                    heatPending -> l10n("A change applies from the next launch", "Una modifica vale dal prossimo avvio")
                    advancedAllOn -> l10n("All on (recommended)", "Tutto attivo (consigliato)")
                    else -> l10n("Some entries are off", "Alcune voci sono spente")
                },
                l10n("They apply immediately and to all games", "Si applicano subito e valgono per tutti i giochi"),
                Icons.Filled.Info))
        }
        val b = screenBrightnessState.floatValue
        val display = buildList {
            add(QmSection(l10n("Screen", "Schermo")))
            add(QmSlider(l10n("Brightness", "Luminosità"), l10n("This session only; all the way left = system", "Solo questa sessione; tutto a sinistra = sistema"),
                QmIcons.Sun, b, 0f, 1f, 0.05f, if (b < 0.03f) l10n("System", "Sistema") else pct(b)) { v ->
                setScreenBrightness(v)
            })
            add(QmSection(l10n("Graphics", "Grafica")))
            if (graphics.isNotEmpty()) {
                add(QmChoice(l10n("Graphics profile", "Profilo grafico"), l10n("Performance, balanced or quality for this game", "Prestazioni, bilanciato o qualità per questo gioco"),
                    Icons.Filled.Settings,
                    graphics.getOrNull(graphicsPresetIndex.intValue)?.removeSuffix(".toml")
                        ?.replaceFirstChar { it.uppercase() } ?: l10n("Default", "Predefinito"),
                    nextLaunch = true) { cycleGraphicsPreset() })
            }
            add(QmNote(l10n("All graphics options", "Tutte le opzioni grafiche"), l10n("In the library: the three dots on a game, then Game settings", "Nella libreria: i tre puntini su un gioco, poi Impostazioni del gioco"),
                Icons.Filled.Info))
        }
        return listOf(
            QmTab(l10n("Game", "Gioco"), Icons.Filled.PlayArrow, game),
            QmTab(l10n("Controls", "Comandi"), QmIcons.Gamepad, controls),
            QmTab(l10n("Performance", "Prestazioni"), QmIcons.Chart, stats),
            QmTab(l10n("Screen", "Schermo"), QmIcons.Display, display),
        )
    }

    /** The rows that choose what the on-screen statistics box shows. */
    private fun statsOverlayRows(): List<QmItem> {
        val mode = when {
            !showFpsOverlay.value -> 0
            !statsDetailedState.value -> 1
            else -> 2
        }
        return listOf(
            QmSection(l10n("On-screen statistics", "Statistiche sullo schermo")),
            QmSegmented(l10n("Show", "Mostra"),
                when (mode) {
                    0 -> l10n("Nothing over the game. L3 + R3 shows it in game",
                        "Niente sopra il gioco. L3 + R3 lo mostra in gioco")
                    1 -> l10n("Frames per second and the time of each frame",
                        "Fotogrammi al secondo e durata di ogni fotogramma")
                    else -> l10n("Also CPU, GPU, memory, battery heat and what limits the FPS",
                        "Anche CPU, GPU, memoria, calore della batteria e cosa limita gli FPS")
                },
                QmIcons.Chart,
                options = listOf(l10n("Off", "No"), "FPS", l10n("All", "Tutto")),
                selected = mode,
            ) { m ->
                setStatsDetailed(m == 2)
                setFpsOverlay(m != 0)
            },
            QmToggle(l10n("Frame graph", "Andamento dei fotogrammi"), l10n("Spikes in the line are stutters", "I picchi nella linea sono gli scatti"),
                QmIcons.Chart, checked = statsGraphState.value) { v ->
                statsGraphState.value = v
                statsPrefs.edit().putBoolean("graph", v).apply()
                if (v && !showFpsOverlay.value) setFpsOverlay(true)
            },
            QmAction(l10n("Reset box position", "Ripristina la posizione del riquadro"), l10n("Moves the box back to the top left", "Riporta il riquadro in alto a sinistra"),
                Icons.Filled.Refresh) {
                statsPrefs.edit().putFloat("x", 0f).putFloat("y", 0f).apply()
                hudPositionEpoch.intValue++
            },
        )
    }

    private fun setStatsDetailed(v: Boolean) {
        statsDetailedState.value = v
        statsPrefs.edit().putBoolean("detailed", v).apply()
    }

    /** Same pattern as the controls toggle: flip the live UI state for an immediate effect,
     *  persist the native cvar async so it survives the next boot. */
    private fun toggleFpsOverlay() = setFpsOverlay(!showFpsOverlay.value)

    private fun setFpsOverlay(next: Boolean) {
        fpsUserToggled = true
        showFpsOverlay.value = next
        lifecycleScope.launch(Dispatchers.IO) {
            runCatching {
                val handle = ConfigStore(applicationContext).openLive()
                handle.putBool("Display", "show_debug_overlay", next)
                handle.closeFile()
            }.onFailure { Log.w(TAG, "persisting show_debug_overlay failed", it) }
        }
    }

    /** Cycles the per-title profiles shipped in assets/profiles/<titleId>/ and merges the
     *  selected one into the per-game TOML. The core reads it at boot, so the change lands on
     *  the next launch (the toast says so). */
    private fun cycleGraphicsPreset() {
        val titleId = currentTitleId
        val names = graphicsPresetNames.value
        if (titleId.isNullOrBlank() || names.isEmpty()) {
            Toast.makeText(this, l10n("No graphics profile for this game", "Nessun profilo grafico per questo gioco"), Toast.LENGTH_SHORT).show()
            return
        }
        val next = (graphicsPresetIndex.intValue + 1) % names.size
        graphicsPresetIndex.intValue = next
        val name = names[next]
        lifecycleScope.launch(Dispatchers.IO) {
            runCatching {
                val text = assets.open("profiles/$titleId/$name")
                    .bufferedReader().use { it.readText() }
                val dir = File(Utils.get_storage_root_path(), "config").apply { mkdirs() }
                val file = File(dir, "$titleId.config.toml")
                // Merge into the sparse per-game overlay: the profile's keys win, the
                // player's other per-game overrides stay.
                val h = ConfigHandle.openString(if (file.exists()) file.readText() else "")
                val merged = try {
                    var section = ""
                    for (raw in text.lineSequence()) {
                        val line = raw.substringBefore('#').trim()
                        if (line.isEmpty()) continue
                        if (line.startsWith("[") && line.endsWith("]")) {
                            section = line.substring(1, line.length - 1).trim(); continue
                        }
                        val eq = line.indexOf('=')
                        if (eq <= 0 || section.isEmpty()) continue
                        val key = line.substring(0, eq).trim()
                        val v = line.substring(eq + 1).trim()
                        when {
                            v == "true" || v == "false" -> h.putBool(section, key, v == "true")
                            v.length >= 2 && v.startsWith("\"") && v.endsWith("\"") ->
                                h.putString(section, key, v.substring(1, v.length - 1))
                            v.toIntOrNull() != null -> h.putInt(section, key, v.toInt())
                            v.toDoubleOrNull() != null -> h.putDouble(section, key, v.toDouble())
                            else -> Log.w(TAG, "graphics profile $name: skipped $section.$key")
                        }
                    }
                    h.closeString()
                } finally {
                    h.closeDiscard()
                }
                val pending = File(dir, file.name + ".pending")
                pending.writeText(merged)
                if (!pending.renameTo(file)) { file.writeText(merged); pending.delete() }
            }.onFailure { Log.w(TAG, "writing graphics profile failed", it) }
        }
        Toast.makeText(this, name.removeSuffix(".toml") + l10n(": applies from the next game launch", ": vale dal prossimo avvio del gioco"),
            Toast.LENGTH_LONG).show()
    }

    /** Copies every save of the current title to Download/XenDroid-Saves/<game>/. */
    private fun backupSaves() {
        val titleId = currentTitleId
        if (titleId.isNullOrBlank()) {
            Toast.makeText(this, l10n("Game title not available", "Titolo del gioco non disponibile"), Toast.LENGTH_SHORT).show()
            return
        }
        val gameName = currentGameName.ifBlank { titleId }
        Toast.makeText(this, l10n("Copying saves…", "Copia dei salvataggi…"), Toast.LENGTH_SHORT).show()
        lifecycleScope.launch(Dispatchers.IO) {
            val result = runCatching {
                val entries = SavesStore.list(applicationContext, titleId)
                entries.forEach { SavesStore.export(applicationContext, it, gameName) }
                entries.size
            }
            mainHandler.post {
                val msg = result.fold(
                    { n -> l10n("$n saves copied to Download/XenDroid-Saves/$gameName", "$n salvataggi copiati in Download/XenDroid-Saves/$gameName") },
                    { t -> l10n("Save copy failed: ${t.message}", "Copia dei salvataggi non riuscita: ${t.message}") },
                )
                Toast.makeText(this@EmulatorHostActivity, msg, Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun isPanelKey(keyCode: Int): Boolean = when (keyCode) {
        KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_LEFT,
        KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT,
        KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
        KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B -> true
        else -> false
    }

    private fun panelKeyDown(nav: PanelNav, keyCode: Int): Boolean = when (keyCode) {
        KeyEvent.KEYCODE_DPAD_UP -> { movePanelSelection(nav, -1); true }
        KeyEvent.KEYCODE_DPAD_LEFT -> { panelHorizontal(nav, -1); true }
        KeyEvent.KEYCODE_DPAD_DOWN -> { movePanelSelection(nav, 1); true }
        KeyEvent.KEYCODE_DPAD_RIGHT -> { panelHorizontal(nav, 1); true }
        KeyEvent.KEYCODE_BUTTON_L1 -> { nav.onShoulder?.invoke(-1); nav.onShoulder != null }
        KeyEvent.KEYCODE_BUTTON_R1 -> { nav.onShoulder?.invoke(1); nav.onShoulder != null }
        KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
        KeyEvent.KEYCODE_BUTTON_A -> {
            nav.activate(panelSelectedState.intValue); true
        }
        KeyEvent.KEYCODE_BUTTON_B -> {
            nav.cancel(); true
        }
        KeyEvent.KEYCODE_BUTTON_Y -> {
            nav.onSecondary?.invoke(panelSelectedState.intValue); nav.onSecondary != null
        }
        else -> false
    }

    // Edge-detected, and shared by hat and left stick so holding either only steps once.
    private var panelNavUp = false
    private var panelNavDown = false
    private var panelNavLeft = false
    private var panelNavRight = false

    private fun panelHat(nav: PanelNav, event: MotionEvent) {
        val y = event.getAxisValue(MotionEvent.AXIS_HAT_Y) + event.getAxisValue(MotionEvent.AXIS_Y)
        val x = event.getAxisValue(MotionEvent.AXIS_HAT_X) + event.getAxisValue(MotionEvent.AXIS_X)
        val up = y < -0.5f; val down = y > 0.5f
        val left = x < -0.5f; val right = x > 0.5f
        if (up != panelNavUp) { panelNavUp = up; if (up) movePanelSelection(nav, -1) }
        if (down != panelNavDown) { panelNavDown = down; if (down) movePanelSelection(nav, 1) }
        if (left != panelNavLeft) { panelNavLeft = left; if (left) panelHorizontal(nav, -1) }
        if (right != panelNavRight) { panelNavRight = right; if (right) panelHorizontal(nav, 1) }
    }

    /** Left/right: the panel's own use for them (menu sliders, tabs), else prev/next. */
    private fun panelHorizontal(nav: PanelNav, dir: Int) {
        val custom = nav.onHorizontal
        if (custom != null) custom(dir) else movePanelSelection(nav, dir)
    }

    private fun movePanelSelection(nav: PanelNav, delta: Int) {
        if (nav.count <= 0) return
        quitArmedState.value = false
        savestateDeleteArmed.intValue = -1
        panelSelectedState.intValue = nav.moveFrom?.invoke(panelSelectedState.intValue, delta)
            ?: qmWrap(panelSelectedState.intValue, delta, nav.count)
    }

    // Guest panel answers. Each releases the blocked guest thread exactly once.

    private fun answerMessageBox(button: Int) {
        val req = messageBoxRequestState.value ?: return
        session.messageBoxSubmit(req.id, button)
        messageBoxRequestState.value = null
    }

    private fun chooseDisc(path: String) {
        val req = discRequestState.value ?: return
        session.discSubmit(req.id, true, path)
        discRequestState.value = null
    }

    private fun cancelDisc() {
        val req = discRequestState.value ?: return
        session.discSubmit(req.id, false, "")
        discRequestState.value = null
    }

    private fun acceptKeyboard(text: String) {
        val req = keyboardRequestState.value ?: return
        session.keyboardSubmit(req.id, true, text)
        keyboardRequestState.value = null
    }

    private fun cancelKeyboard() {
        val req = keyboardRequestState.value ?: return
        session.keyboardSubmit(req.id, false, "")
        keyboardRequestState.value = null
    }
}

/** Brightness -> overlay contrast, collected in a coroutine so emissions never recompose.
 *  The target is quantized to 1/8 steps so EMA jitter re-targets the same value instead of
 *  restarting the tween forever. */
@Composable
private fun rememberOverlayContrast(brightness: StateFlow<Float>): State<Float> {
    val anim = remember { Animatable(0f) }
    LaunchedEffect(brightness) {
        brightness.collect { b ->
            // Below ~0.5 luminance the plain overlay reads fine; ramp to full by ~0.9.
            val target = ((b - 0.5f) / 0.4f).coerceIn(0f, 1f).let { (it * 8f).roundToInt() / 8f }
            if (target != anim.targetValue) {
                launch { anim.animateTo(target, tween(400)) }   // MutatorMutex cancels the in-flight tween
            }
        }
    }
    return anim.asState()
}

/** Shown from launch until the first presented frame; a tap dismisses it early. */
@Composable
private fun BootSplash(gameName: String, onDismiss: () -> Unit, modifier: Modifier = Modifier) {
    Box(
        modifier.background(Color.Black).clickable(
            interactionSource = remember { androidx.compose.foundation.interaction.MutableInteractionSource() },
            indication = null,
            onClick = onDismiss,
        ),
        contentAlignment = Alignment.Center,
    ) {
        androidx.compose.foundation.layout.Column(
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            if (gameName.isNotBlank()) {
                Text(gameName, color = Color.White, fontSize = 20.sp)
            }
            Text(
                "Starting the game…",
                color = Color.White.copy(alpha = 0.7f), fontSize = 13.sp,
                modifier = Modifier.padding(top = 6.dp, bottom = 14.dp),
            )
            androidx.compose.material3.LinearProgressIndicator(
                modifier = Modifier.clip(RoundedCornerShape(2.dp)),
            )
        }
    }
}
