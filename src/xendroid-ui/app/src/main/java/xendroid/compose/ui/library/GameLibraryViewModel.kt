package xendroid.compose.ui.library

import xendroid.compose.l10n

import xendroid.compose.core.R
import android.content.Context
import android.content.Intent
import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.graphics.BitmapFactory
import android.graphics.drawable.Icon
import android.os.Build
import android.os.SystemClock
import android.util.Log
import androidx.core.content.getSystemService
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import xendroid.compose.core.EmulatorRuntime
import xendroid.compose.core.ContentPaths
import xendroid.compose.ui.content.saveOwnerXuid
import java.io.File
import xendroid.compose.core.GameMetadataSource
import xendroid.compose.core.ProfileBootstrap
import xendroid.compose.core.ProfilePaths
import xendroid.compose.settings.ConfigStore
import xendroid.compose.data.Game
import xendroid.compose.data.GameFormat
import xendroid.compose.data.GameLibraryRepository
import xendroid.compose.data.IconCache
import kotlinx.coroutines.Job
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** The launch action emitted on game tap. Resolves once a host Activity registers
 *  for this action; until then the Intent will not resolve (no-op resolveActivity). */
const val ACTION_LAUNCH_GAME = "xendroid.intent.action.xendroid"
const val EXTRA_GAME_URI = "game_uri"
const val EXTRA_DISC_LABELS = "disc_labels"
const val EXTRA_DISC_PATHS = "disc_paths"
const val EXTRA_TITLE_ID = "title_id"
const val EXTRA_GAME_NAME = "game_name"

/** Minimum time the pull-to-refresh indicator stays up, so a fast warm-cache rescan
 *  doesn't outrun its reveal animation and leave it visually stuck. */
private const val MIN_REFRESH_INDICATOR_MS = 500L

/** Which long-press action triggered title-id resolution (both need the id, then branch). */
enum class GameAction { PER_GAME_SETTINGS, GAME_PATCHES, MANAGE_CONTENT, SAVES }

/** Async resolution of a game's title id (needed before the per-game settings editor or the
 *  patches screen can open). Driven by the long-press dialog; all formats resolve boot-free. */
sealed interface TitleIdState {
    data object Idle : TitleIdState
    data class Loading(val game: Game, val action: GameAction) : TitleIdState
    data class Resolved(val game: Game, val titleId: String, val action: GameAction) : TitleIdState
    data class Error(val game: Game, val message: String) : TitleIdState
}

class GameLibraryViewModel(
    private val repo: GameLibraryRepository,
    private val iconCache: IconCache,
    private val appContext: Context,
) : ViewModel() {

    private val _state = MutableStateFlow<LibraryUiState>(LibraryUiState.Loading)
    val state: StateFlow<LibraryUiState> = _state.asStateFlow()

    private val _titleId = MutableStateFlow<TitleIdState>(TitleIdState.Idle)
    val titleIdState: StateFlow<TitleIdState> = _titleId.asStateFlow()

    /** True while a scan is running. Drives the swipe-down (pull-to-refresh) indicator
     *  WITHOUT flashing the full-screen spinner over an already-loaded list. */
    private val _isRefreshing = MutableStateFlow(false)
    val isRefreshing: StateFlow<Boolean> = _isRefreshing.asStateFlow()

    private val _loadingMessage = MutableStateFlow(l10n("Checking the library…", "Controllo della libreria…"))
    val loadingMessage: StateFlow<String> = _loadingMessage.asStateFlow()

    private var refreshJob: Job? = null
    private val bootstrapMutex = Mutex()

    init { refresh() }

    fun refresh() {
        if (refreshJob?.isActive == true) return
        if (!EmulatorRuntime.supportsVulkan) { _state.value = LibraryUiState.NoVulkan; return }
        // Keep an existing list visible during a pull-to-refresh (show only the pull
        // indicator); the full-screen spinner is for the first/empty load.
        val wasLoaded = _state.value is LibraryUiState.Loaded
        if (!wasLoaded) _state.value = LibraryUiState.Loading
        _isRefreshing.value = true
        refreshJob = viewModelScope.launch {
            val startMs = SystemClock.elapsedRealtime()
            var indicatorStartMs = startMs
            var displayedCached = false
            _state.value = runCatching {
                when (val cached = timedStage("cached_library") { repo.cachedGames() }) {
                    GameLibraryRepository.ScanResult.NoFolder -> {
                        _isRefreshing.value = false
                        return@launch run { _state.value = LibraryUiState.NoFolder }
                    }
                    GameLibraryRepository.ScanResult.PermissionLost -> {
                        _isRefreshing.value = false
                        return@launch run { _state.value = LibraryUiState.PermissionLost }
                    }
                    is GameLibraryRepository.ScanResult.Games -> {
                        _state.value = LibraryUiState.Loaded(cached.games, fromCache = true)
                        displayedCached = true
                        indicatorStartMs = SystemClock.elapsedRealtime()
                    }
                    null -> Unit // Missing/corrupt cache: retain the ordinary first scan.
                }
                // ensureLoaded() can sleep + System.loadLibrary on delay-load devices
                // (Adreno 5xx/6xx) -> never on the main thread.
                withContext(Dispatchers.IO) {
                    bootstrapMutex.withLock {
                        timedStage("native_load") { EmulatorRuntime.ensureLoaded() }
                        timedStage("profile") { ProfileBootstrap.ensureDefaultProfile(appContext) }
                    }
                }
                when (val r = timedStage("scan") { repo.scan() }) {
                    GameLibraryRepository.ScanResult.NoFolder -> LibraryUiState.NoFolder
                    GameLibraryRepository.ScanResult.PermissionLost -> LibraryUiState.PermissionLost
                    is GameLibraryRepository.ScanResult.Games -> LibraryUiState.Loaded(r.games)
                }
            }.getOrElse { LibraryUiState.Error(it.message ?: l10n("Could not load the library", "Impossibile caricare la libreria")) }
            // A warm-cache rescan finishes faster than the PullToRefreshBox reveal animation,
            // which leaves the indicator visually stuck; hold it to a floor so it settles before
            // retracting (pull-to-refresh only -- the cold load shows the full-screen spinner).
            if (wasLoaded || displayedCached) {
                val elapsed = SystemClock.elapsedRealtime() - indicatorStartMs
                if (elapsed < MIN_REFRESH_INDICATOR_MS) delay(MIN_REFRESH_INDICATOR_MS - elapsed)
            }
            _isRefreshing.value = false
        }
    }

    /** Emit phase durations without changing loading behavior or touching game files. */
    private suspend fun <T> timedStage(stage: String, block: suspend () -> T): T {
        _loadingMessage.value = when (stage) {
            "cached_library" -> l10n("Checking the library…", "Controllo della libreria…")
            "native_load" -> l10n("Preparing the emulator…", "Preparazione dell'emulatore…")
            "profile" -> l10n("Checking the player profile…", "Controllo del profilo giocatore…")
            else -> l10n("Searching for games…", "Ricerca dei giochi…")
        }
        val startMs = SystemClock.elapsedRealtime()
        return try {
            block()
        } finally {
            Log.i("LibraryTiming", "stage=$stage elapsed_ms=${SystemClock.elapsedRealtime() - startMs}")
        }
    }

    /** Real-path (All Files Access) folder chosen in the built-in browser: persist the
     *  abs path + rescan. */
    fun onRealPathFolderPicked(path: String) {
        viewModelScope.launch {
            if (repo.saveGameDirPath(path)) {
                refreshJob?.cancel()
                refreshJob?.join()
                _state.value = LibraryUiState.Loading
                refresh()
            }
        }
    }

    fun requestPerGameSettings(game: Game) = request(game, GameAction.PER_GAME_SETTINGS)
    fun requestGamePatches(game: Game) = request(game, GameAction.GAME_PATCHES)
    fun requestContentManager(game: Game) = request(game, GameAction.MANAGE_CONTENT)
    fun requestSaves(game: Game) = request(game, GameAction.SAVES)

    /** Resolve a game's title id off-main, then the long-press dialog opens the matching screen. */
    private fun request(game: Game, action: GameAction) {
        _titleId.value = TitleIdState.Loading(game, action)
        viewModelScope.launch(Dispatchers.IO) {
            _titleId.value = runCatching {
                EmulatorRuntime.ensureLoaded()
                val tid = repo.readTitleId(appContext, game)
                // 00000000 is the unknown/placeholder title id (no real game carries it).
                if (tid.isNullOrBlank() || tid == "00000000")
                    TitleIdState.Error(game, l10n("Could not read the Title ID of this game", "Impossibile leggere il Title ID di questo gioco"))
                else TitleIdState.Resolved(game, tid, action)
            }.getOrElse { TitleIdState.Error(game, it.message ?: l10n("Could not read the Title ID", "Impossibile leggere il Title ID")) }
        }
    }

    fun clearTitleIdRequest() { _titleId.value = TitleIdState.Idle }

    /** Build the launch Intent that the host Activity resolves. Caller startActivity()s it. */
    /** Packages [game]'s disc carries that are not installed yet, i.e. the payload a
     *  mandatory-install title (GTA V and friends) needs on the HDD before it will run.
     *  Empty for an ordinary disc, or once its content is installed. Off-main: walks the
     *  disc image. */
    suspend fun uninstalledDiscContent(game: Game): List<GameMetadataSource.DiscContent> =
        withContext(Dispatchers.IO) {
            // A cached game may be tapped while refresh is preparing the profile.
            // Await only bootstrap, never the full scan, and never block the UI thread.
            bootstrapMutex.withLock {
                EmulatorRuntime.ensureLoaded()
                ProfileBootstrap.ensureDefaultProfile(appContext)
            }
            if (game.format != GameFormat.ISO && game.format != GameFormat.ZAR) {
                return@withContext emptyList()
            }
            GameMetadataSource().listDiscContent(game.launchUri).filterNot { item ->
                val titleId = item.titleId ?: return@filterNot false
                File(ContentPaths.contentDir(titleId, item.contentType), item.displayName)
                    .exists()
            }
        }

    /** Add-on packages (DLC, title updates, saves) for [game]'s title that the scan found in
     *  the games folder and that are not installed yet. Off-main. */
    suspend fun uninstalledAddOns(game: Game): List<GameLibraryRepository.AddOn> =
        withContext(Dispatchers.IO) {
            val titleId = game.titleId?.takeIf { it.isNotBlank() && it != "00000000" }
                ?: return@withContext emptyList()
            val found = repo.addOnsFor(titleId)
            if (found.isEmpty()) return@withContext emptyList()
            val saveOwner by lazy { saveOwnerXuid(appContext).ifEmpty { ContentPaths.MACHINE_XUID } }
            found.filterNot { addOn ->
                val owner = if (addOn.contentType == ContentPaths.SAVED_GAME_CONTENT_TYPE) saveOwner
                    else ContentPaths.MACHINE_XUID
                File(ContentPaths.contentDir(titleId, addOn.contentType, owner), File(addOn.path).name)
                    .exists()
            }
        }

    fun buildLaunchIntent(game: Game): Intent =
        Intent(ACTION_LAUNCH_GAME).apply {
            setPackage(appContext.packageName)          // self; host is in this app
            putExtra(EXTRA_GAME_URI, game.launchUri)
            game.titleId?.takeIf { it.isNotBlank() && it != "00000000" }?.let {
                putExtra(EXTRA_TITLE_ID, it)
            }
            if (game.name.isNotBlank()) putExtra(EXTRA_GAME_NAME, game.name)
            // :emu cannot read the library, so the discs travel with the launch.
            val discs = discsOfTitle(game)
            putExtra(EXTRA_DISC_LABELS, discs.map { discLabelOf(it) }.toTypedArray())
            putExtra(EXTRA_DISC_PATHS, discs.map { it.launchUri }.toTypedArray())
        }

    /** Every disc of [game]'s title, including the one being launched: after a swap
     *  the launched disc becomes a swap target again (install disc -> play disc).
     *  Matched on title id, which a set shares; STFS is excluded because updates
     *  and DLC share it too without being discs. */
    fun discsOfTitle(game: Game): List<Game> {
        val titleId = game.titleId?.takeIf { it.isNotBlank() && it != "00000000" }
            ?: return emptyList()
        val all = (_state.value as? LibraryUiState.Loaded)?.games ?: return emptyList()
        return all.filter {
            it.format != GameFormat.STFS &&
                it.titleId?.equals(titleId, ignoreCase = true) == true
        }.sortedWith(compareBy({ if (it.discNumber > 0) it.discNumber else Int.MAX_VALUE },
                                { it.name.lowercase() }))
    }

    /** A set shares one XDBF title, so prefer the header's disc number and fall
     *  back to the file name. */
    fun discLabelOf(game: Game): String = when {
        game.discNumber > 0 && game.discCount > 1 ->
            l10n("Disc ${game.discNumber} of ${game.discCount}", "Disco ${game.discNumber} di ${game.discCount}")
        game.discNumber > 0 -> l10n("Disc ${game.discNumber}", "Disco ${game.discNumber}")
        else -> java.io.File(game.launchUri).name
    }

    /** Coil model for a game's icon: the cached PNG File when present, else the
     *  app_icon drawable resource id. Kept here so the View carries no IconCache dep. */
    /** The signed-in profile shown in the dashboard header. */
    data class ActiveProfile(val gamertag: String, val picture: File?)

    private val _profile = MutableStateFlow<ActiveProfile?>(null)
    val profile: StateFlow<ActiveProfile?> = _profile.asStateFlow()

    /** Reads the profile signed in to slot 0 (else the first one) and its gamer picture. */
    fun loadProfile() = viewModelScope.launch(Dispatchers.IO) {
        try {
            EmulatorRuntime.ensureLoaded()
            val emu = EmulatorRuntime.emulator ?: return@launch
            val config = ConfigStore(appContext).openLiveSnapshot()
            val active = try {
                config.getString("Profiles", "logged_profile_slot_0_xuid") ?: ""
            } finally {
                config.closeString()
            }
            val profiles = emu.list_profiles(ContentPaths.contentRoot().absolutePath) ?: return@launch
            val p = profiles.firstOrNull { it.xuid.equals(active, ignoreCase = true) }
                ?: profiles.firstOrNull() ?: return@launch
            val picture = ProfilePaths.tile64Path(p.xuid).takeIf { p.hasAvatar && it.exists() }
            _profile.value = ActiveProfile(p.gamertag.orEmpty(), picture)
        } catch (t: RuntimeException) {
            Log.w("GameLibraryViewModel", "profile read failed", t)
        }
    }

    fun iconFileOrFallback(game: Game): Any {
        val file = game.iconCacheName?.let { iconCache.fileFor(it) }
        return if (file != null && file.exists()) file
        else R.drawable.app_icon
    }

    val isPinShortcutSupported: Boolean
        get() = appContext.getSystemService<ShortcutManager>()
            ?.isRequestPinShortcutSupported == true

    /** True once a host Activity resolves the launch action.
     *  Until then, suppress launch-dependent affordances so we never pin a dead shortcut. */
    val canLaunchGames: Boolean
        get() = Intent(ACTION_LAUNCH_GAME)
            .setPackage(appContext.packageName)
            .resolveActivity(appContext.packageManager) != null

    /** Pin a launcher shortcut. Stable id = game.stableId (uri) to avoid name
     *  collisions (legacy used the display name -> collisions). */
    fun createShortcut(game: Game) {
        if (!isPinShortcutSupported || Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val sm = appContext.getSystemService<ShortcutManager>() ?: return
        val intent = buildLaunchIntent(game).apply { action = Intent.ACTION_VIEW }
        // No resolving host yet; don't pin a shortcut that goes nowhere.
        if (intent.resolveActivity(appContext.packageManager) == null) return
        val icon = game.iconCacheName
            ?.let { iconCache.fileFor(it) }
            ?.takeIf { it.exists() }
            ?.let { BitmapFactory.decodeFile(it.absolutePath) }
            ?.let { Icon.createWithBitmap(it) }
            ?: Icon.createWithResource(appContext, R.drawable.app_icon)
        sm.requestPinShortcut(
            ShortcutInfo.Builder(appContext, game.stableId)
                .setShortLabel(game.name)
                .setIcon(icon)
                .setIntent(intent)
                .build(),
            null
        )
    }
}
