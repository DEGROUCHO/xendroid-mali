package xendroid.compose

import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import android.hardware.input.InputManager
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
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
import xendroid.compose.ui.keyboard.GuestKeyboardPanel
import xendroid.compose.ui.keyboard.clampToUtf16Units
import xendroid.compose.ui.messagebox.GuestMessageBoxPanel
import xendroid.compose.ui.pause.PAUSE_OPTION_COUNT
import xendroid.compose.ui.pause.PAUSE_OPTION_GRAPHICS
import xendroid.compose.ui.pause.PAUSE_OPTION_SAVES
import xendroid.compose.settings.ConfigStore
import xendroid.compose.ui.pause.PAUSE_OPTION_METRICS
import xendroid.compose.ui.pause.PAUSE_OPTION_QUIT
import xendroid.compose.ui.pause.PAUSE_OPTION_TOUCH_OVERLAY
import xendroid.compose.ui.pause.PAUSE_OPTION_RESUME
import xendroid.compose.ui.pause.PauseMenuPanel
import xendroid.compose.ui.theme.xendroidTheme
import xendroid.compose.gamepad.GamepadConfigDto
import xendroid.compose.gamepad.GamepadController
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
    }

    private val session = EmulatorSession()
    private var surfaceView: SurfaceView? = null
    private var started = false          // surface-callback boot guard

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
    // Null until the first post-boot poll reads HID|show_touch_overlay. Mounting the overlay
    // on an assumed value and unmounting a tick later fires its release-all teardown while
    // the core is still booting.
    private val showTouchOverlay = mutableStateOf<Boolean?>(null)
    private val menuOpenState = mutableStateOf(false)
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
    // Edge-detect state for analog triggers reported as axes.
    private var lTriggerDown = false
    private var rTriggerDown = false
    // L3 (THUMBL) / R3 (THUMBR) held-state for the metrics-toggle chord.
    private var l3Down = false
    private var r3Down = false

    // PC controls opt in via F8; the normal controller path remains the default.
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
    ).apply { setEnabled(true) }
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
        lTriggerDown = false; rTriggerDown = false
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
        super.onCreate(savedInstanceState)
        getSystemService(InputManager::class.java).registerInputDeviceListener(inputDevices, mainHandler)
        enterImmersiveMode()

        // Keep the launcher out of the cached band so lmkd stops reaping it mid-game, and
        // notice if the main process dies anyway.
        EmuProcessLink.bindToMainProcess(this)

        currentTitleId = intent.getStringExtra(EXTRA_TITLE_ID)?.takeIf { it.isNotBlank() }
        currentGameName = intent.getStringExtra(EXTRA_GAME_NAME).orEmpty()
        pcInput.setTitleId(currentTitleId)

        val gameUri = FrontendLaunch.resolveGamePath(this, intent)
        if (gameUri.isNullOrEmpty()) {
            Log.e(TAG, "No bootable game in launch intent; finishing")
            Toast.makeText(this, "XenDroid: no game in launch intent", Toast.LENGTH_LONG).show()
            finish(); return
        }
        if (!EmulatorRuntime.supportsVulkan) {
            Toast.makeText(this, "No Vulkan GPU; cannot boot", Toast.LENGTH_LONG).show()
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
            if (session.booted && !menuOpenState.value) session.resumeIfPaused()
        } else {
            // Focus can return while Mali is still compiling. Pausing the core
            // here blocks the Android input looper on GPU/audio fences; real
            // backgrounding is still paused by onStop/surfaceDestroyed.
            releasePcInput()
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
                val landscape = resources.configuration.orientation ==
                    Configuration.ORIENTATION_LANDSCAPE
                val booted by bootedState
                val controls = remember(cfg, landscape) { gamepad.controlsFor(cfg, landscape) }
                val (visible, poke) = rememberAutoHide(cfg.globals.autoHideSeconds)
                val alpha by animateFloatAsState(
                    if (visible) cfg.globals.opacity else 0f, tween(500), label = "padAlpha")

                val padVisible = showTouchOverlay.value == true
                val overlayActive = booted && cfg.globals.enabled && padVisible
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
                    if (booted && cfg.globals.enabled && padVisible) {
                        GamepadOverlay(
                            controls = controls,
                            opacity = alpha,
                            floatingLeftStick = cfg.globals.floatingLeftStick,
                            faceButtonSlide = cfg.globals.faceButtonSlide,
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
                                session.keyEvent(kc, pressed, v)
                            },
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
                            // Machine-readable FPS timeline for on-device measurements.
                            Log.i(TAG, "fps=" + session.averageFps() +
                                " frameMs=" + session.lastFrameTimeMs())
                            delay(1000)
                        }
                    }
                    FpsOverlay(
                        session = session,
                        visible = booted && showFps,
                        modifier = Modifier.fillMaxSize(),
                    )

                    // Always-available touch door into the pause menu: gamepad users have Back,
                    // touch-only users had no way in. Hidden while a guest prompt owns input.
                    if (booted && !menuOpenState.value &&
                        keyboardRequestState.value == null &&
                        messageBoxRequestState.value == null &&
                        discRequestState.value == null) {
                        Box(
                            modifier = Modifier
                                .align(Alignment.TopCenter)
                                .padding(top = 4.dp)
                                .clip(RoundedCornerShape(8.dp))
                                .background(Color.Black.copy(alpha = 0.35f))
                                .clickable {
                                    panelSelectedState.intValue = PAUSE_OPTION_RESUME
                                    menuOpenState.value = true
                                    if (session.booted) session.pause()
                                }
                                .padding(horizontal = 10.dp, vertical = 4.dp),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text("MENU", color = Color.White, fontSize = 12.sp)
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
                LaunchedEffect(booted, menuOpen, keyboardOpen, discOpen, messageBoxOpen) {
                    if (menuOpen || keyboardOpen || discOpen || messageBoxOpen) releasePcInput()
                    else updatePcCapture()
                }
                BackHandler(enabled = keyboardOpen) { cancelKeyboard() }
                BackHandler(enabled = discOpen) { cancelDisc() }
                BackHandler(enabled = messageBoxOpen) {
                    messageBoxRequestState.value?.let { answerMessageBox(it.activeButton) }
                }
                BackHandler(enabled = !menuOpen && !keyboardOpen && !discOpen && !messageBoxOpen) {
                    panelSelectedState.intValue = PAUSE_OPTION_RESUME
                    menuOpenState.value = true
                    if (session.booted) session.pause()
                }
                BackHandler(enabled = menuOpen) { closeMenuAndResume() }
                if (menuOpen) xendroidTheme {
                    PauseMenuPanel(
                        selected = panelSelectedState.intValue,
                        touchOverlayShown = showTouchOverlay.value == true,
                        metricsShown = showFpsOverlay.value,
                        graphicsLabel = graphicsPresetNames.value
                            .getOrNull(graphicsPresetIndex.intValue)
                            ?.removeSuffix(".toml") ?: "Default",
                        onToggleTouchOverlay = { toggleTouchOverlay() },
                        onToggleMetrics = { toggleFpsOverlay() },
                        onCycleGraphics = { cycleGraphicsPreset() },
                        onBackupSaves = { backupSaves() },
                        onResume = { closeMenuAndResume() },
                        onQuit = { finish() },
                        modifier = Modifier.fillMaxSize(),
                    )
                }
            }
        }
        setContentView(compose)
        sv.requestFocus()
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
        vibrator?.vibrate(VibrationEffect.createOneShot(25, VibrationEffect.DEFAULT_AMPLITUDE))
    }

    // SurfaceHolder.Callback: the load-bearing surface->boot ordering.

    override fun surfaceCreated(holder: SurfaceHolder) {
        // Run the panel at its native maximum refresh: the game is frame-rate unlocked and
        // the native framerate_limit pacer is the authoritative limiter, so pinning the
        // surface to 60 Hz strangled every frame above 60.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            runCatching {
                val d = display ?: windowManager.defaultDisplay
                val maxRate = d.supportedModes.maxOfOrNull { it.refreshRate }
                    ?.takeIf { it > 0f } ?: d.mode?.refreshRate?.takeIf { it > 0f }
                if (maxRate != null) {
                    holder.surface.setFrameRate(
                        maxRate, android.view.Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
                    Log.i(TAG, "surface frame-rate hint: $maxRate Hz")
                }
            }
        }
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
            if (!menuOpenState.value) session.resumeIfPaused()
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (!started) return
        if (width == 0 || height == 0) return
        session.changeSurface(width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
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
        EmuProcessLink.setEmuForeground(true)
        if (overlayWantsBrightness) surfaceView?.let { brightnessSampler.start(it) }
        // resumeIfPaused (not bare resume) is idempotent: surfaceCreated already resumes on
        // swapchain-recreate, so this is a no-op there; onStart additionally covers the pure
        // screen-sleep case where the surface was NOT destroyed. Stay paused if the menu is open.
        if (session.booted && !menuOpenState.value) session.resumeIfPaused()
    }

    override fun onDestroy() {
        super.onDestroy()
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
                Toast.makeText(this, if (next) "PC controls: WASD + mouse (F8: controller)"
                    else "Controller controls (F8: WASD + mouse)", Toast.LENGTH_SHORT).show()
            }
            return true
        }
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
        if (event.repeatCount == 0) {
            session.keyEvent(gameKey, true, KEY_VALUE_UNUSED)
            return true
        }
        return super.onKeyDown(keyCode, event)            // ignore auto-repeats
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_F8) return true
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
        session.keyEvent(gameKey, false, KEY_VALUE_UNUSED)
        return true
    }

    /** Consumes unmapped controller buttons (never BACK): unhandled gamepad input is what
     *  OEM overlays latch onto. */
    private fun consumeIfGamepad(event: KeyEvent): Boolean =
        event.keyCode != KeyEvent.KEYCODE_BACK &&
            (event.source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK)

    /** The SurfaceView listener only fires while that view holds focus. */
    override fun onGenericMotionEvent(event: MotionEvent): Boolean = onGenericMotion(event)

    /** Joystick axes + hat (D-pad). */
    private fun onGenericMotion(event: MotionEvent): Boolean {
        panelNav()?.let { nav ->
            panelHat(nav, event)
            return true
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
        emitAxisPair(event.getAxisValue(MotionEvent.AXIS_X),
            negKey = KC_LTHUMB_LEFT, posKey = KC_LTHUMB_RIGHT, invert = false)
        emitAxisPair(event.getAxisValue(MotionEvent.AXIS_Y),
            negKey = KC_LTHUMB_UP, posKey = KC_LTHUMB_DOWN, invert = true)
        emitAxisPair(event.getAxisValue(MotionEvent.AXIS_Z),
            negKey = KC_RTHUMB_LEFT, posKey = KC_RTHUMB_RIGHT, invert = false)
        emitAxisPair(event.getAxisValue(MotionEvent.AXIS_RZ),
            negKey = KC_RTHUMB_UP, posKey = KC_RTHUMB_DOWN, invert = true)
        // Most pads report triggers as ANALOG axes (LTRIGGER/RTRIGGER, or BRAKE/GAS). Emit a
        // press past the threshold, edge-detected so we don't spam.
        lTriggerDown = emitTrigger(
            maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE)),
            KC_TRIGGER_L, lTriggerDown)
        rTriggerDown = emitTrigger(
            maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS)),
            KC_TRIGGER_R, rTriggerDown)
        return true
    }

    /** Analog trigger -> digital game key. Emits only on the down/up edge (>0.5 = down). */
    private fun emitTrigger(value: Float, gameKey: Int, wasDown: Boolean): Boolean {
        val down = value > 0.5f
        if (down != wasDown) session.keyEvent(gameKey, down, KEY_VALUE_UNUSED)
        return down
    }

    /** For one axis emit the opposing thumb directions, scaled to signed-short range.
     *  invert flips sign first (screen Y is up-negative; X360 up is positive). */
    private fun emitAxisPair(axis: Float, negKey: Int, posKey: Int, invert: Boolean) {
        val raw = if (invert) -axis else axis
        // Snap to exactly zero so emitAxis' equality check can match.
        val v = if (abs(raw) < AXIS_DEADZONE) 0f else raw
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
        // Last: a guest prompt opened over the menu owns the input until it is answered.
        if (menuOpenState.value) {
            return PanelNav(PAUSE_OPTION_COUNT,
                { i ->
                    when (i) {
                        PAUSE_OPTION_QUIT -> finish()
                        PAUSE_OPTION_TOUCH_OVERLAY -> toggleTouchOverlay()
                        PAUSE_OPTION_METRICS -> toggleFpsOverlay()
                        PAUSE_OPTION_GRAPHICS -> cycleGraphicsPreset()
                        PAUSE_OPTION_SAVES -> backupSaves()
                        else -> closeMenuAndResume()
                    }
                },
                { closeMenuAndResume() })
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

    private fun closeMenuAndResume() {
        menuOpenState.value = false
        if (session.booted) session.resumeIfPaused()
    }

    /** Same pattern as the controls toggle: flip the live UI state for an immediate effect,
     *  persist the native cvar async so it survives the next boot. */
    private fun toggleFpsOverlay() {
        val next = !showFpsOverlay.value
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

    /** Cycles the per-title profiles shipped in assets/profiles/<titleId>/ and writes the
     *  selected one as the per-game TOML. The core reads it at boot, so the change lands on
     *  the next launch (the toast says so). */
    private fun cycleGraphicsPreset() {
        val titleId = currentTitleId
        val names = graphicsPresetNames.value
        if (titleId.isNullOrBlank() || names.isEmpty()) {
            Toast.makeText(this, "No graphics profiles for this game", Toast.LENGTH_SHORT).show()
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
                File(dir, "$titleId.config.toml").writeText(text)
            }.onFailure { Log.w(TAG, "writing graphics profile failed", it) }
        }
        Toast.makeText(this, "${name.removeSuffix(".toml")}: restart the game to apply",
            Toast.LENGTH_LONG).show()
    }

    /** Copies every save of the current title to Download/XenDroid-Saves/<game>/. */
    private fun backupSaves() {
        val titleId = currentTitleId
        if (titleId.isNullOrBlank()) {
            Toast.makeText(this, "Unknown title id", Toast.LENGTH_SHORT).show()
            return
        }
        val gameName = currentGameName.ifBlank { titleId }
        Toast.makeText(this, "Backing up saves...", Toast.LENGTH_SHORT).show()
        lifecycleScope.launch(Dispatchers.IO) {
            val result = runCatching {
                val entries = SavesStore.list(applicationContext, titleId)
                entries.forEach { SavesStore.export(applicationContext, it, gameName) }
                entries.size
            }
            mainHandler.post {
                val msg = result.fold(
                    { n -> "Saved $n save(s) to Download/XenDroid-Saves/$gameName" },
                    { t -> "Save backup failed: ${t.message}" },
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
        KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_LEFT -> {
            movePanelSelection(nav, -1); true
        }
        KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT -> {
            movePanelSelection(nav, 1); true
        }
        KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
        KeyEvent.KEYCODE_BUTTON_A -> {
            nav.activate(panelSelectedState.intValue); true
        }
        KeyEvent.KEYCODE_BUTTON_B -> {
            nav.cancel(); true
        }
        else -> false
    }

    // Edge-detected, and shared by hat and left stick so holding either only steps once.
    private var panelNavPrev = false
    private var panelNavNext = false

    private fun panelHat(nav: PanelNav, event: MotionEvent) {
        val y = event.getAxisValue(MotionEvent.AXIS_HAT_Y) + event.getAxisValue(MotionEvent.AXIS_Y)
        val x = event.getAxisValue(MotionEvent.AXIS_HAT_X) + event.getAxisValue(MotionEvent.AXIS_X)
        val prev = y < -0.5f || x < -0.5f
        val next = y > 0.5f || x > 0.5f
        if (prev != panelNavPrev) {
            panelNavPrev = prev
            if (prev) movePanelSelection(nav, -1)
        }
        if (next != panelNavNext) {
            panelNavNext = next
            if (next) movePanelSelection(nav, 1)
        }
    }

    private fun movePanelSelection(nav: PanelNav, delta: Int) {
        if (nav.count <= 0) return
        val next = panelSelectedState.intValue + delta
        panelSelectedState.intValue = ((next % nav.count) + nav.count) % nav.count
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
