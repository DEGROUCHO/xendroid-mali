package xendroid.compose.ui.library

import xendroid.compose.l10n

import android.app.Activity
import android.content.Context
import android.util.Log
import android.widget.Toast
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Search
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.sp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import coil.compose.AsyncImage
import coil.request.ImageRequest
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import xendroid.compose.core.AllFilesAccess
import xendroid.compose.core.EmuProcessLink
import xendroid.compose.data.Game
import xendroid.compose.data.GameFormat
import xendroid.compose.ui.compress.GameCompressViewModel
import xendroid.compose.ui.compress.GameCompressViewModel.CompressState
import xendroid.compose.ui.userdata.openUserData
import xendroid.compose.updater.CooldownDialog
import xendroid.compose.updater.getRemainingCooldown
import xendroid.compose.updater.LatestVersionDialog
import xendroid.compose.updater.UpdateDialog
import xendroid.compose.updater.UpdateResult
import xendroid.compose.updater.checkForUpdates
import xendroid.compose.updater.shouldCheckForUpdates
import xendroid.compose.updater.saveLastCheck


@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GameLibraryScreen(
    viewModel: GameLibraryViewModel,
    onOpenSettings: () -> Unit,
    onOpenKeymap: () -> Unit,
    onOpenAbout: () -> Unit,
    onOpenProfiles: () -> Unit,
    onOpenTouchControls: () -> Unit,
    onOpenPerGameSettings: (titleId: String, gameName: String, format: GameFormat, launchUri: String) -> Unit,
    onOpenGamePatches: (titleId: String, gameName: String) -> Unit,
    onOpenContentManager: (titleId: String, gameName: String) -> Unit,
    onOpenSaves: (titleId: String, gameName: String) -> Unit,
    onOpenInstallContent: () -> Unit,
    onInstallFromDisc: (String) -> Unit,
    compressVm: GameCompressViewModel,
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var updateResult by remember { mutableStateOf<UpdateResult?>(null) }

    var pendingGame by remember { mutableStateOf<Game?>(null) }
    // A disc whose content is not installed yet; the launch waits on the answer.
    var pendingDiscInstall by remember { mutableStateOf<Pair<Game, Int>?>(null) }
    var compressConfirmFor by remember { mutableStateOf<Game?>(null) }
    val compressState by compressVm.state.collectAsStateWithLifecycle()
    val titleIdState by viewModel.titleIdState.collectAsStateWithLifecycle()
    val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
    val loadingMessage by viewModel.loadingMessage.collectAsStateWithLifecycle()
    val profile by viewModel.profile.collectAsStateWithLifecycle()
    LaunchedEffect(viewModel) { viewModel.loadProfile() }

    var showBrowser by remember { mutableStateOf(false) }
    var searching by remember { mutableStateOf(false) }
    // The game launched last, offered as a one-tap "continue" banner above the grid.
    val uiPrefs = remember { context.getSharedPreferences("library_ui", Context.MODE_PRIVATE) }
    var lastPlayedId by remember { mutableStateOf(uiPrefs.getString("last_played", null)) }
    var recentFirst by remember { mutableStateOf(uiPrefs.getBoolean("recent_first", false)) }
    // NXE-style dashboard is the face of the library; the classic grid is one tap away.
    var dashboardView by remember { mutableStateOf(LibraryStyle.isDashboard(context)) }
    var query by remember { mutableStateOf("") }
    fun recordLaunch(game: Game) {
        uiPrefs.edit().putString("last_played", game.stableId)
            .putLong("played_${game.stableId}", System.currentTimeMillis()).apply()
        lastPlayedId = game.stableId
    }
    val toggleSort: () -> Unit = {
        recentFirst = !recentFirst
        uiPrefs.edit().putBoolean("recent_first", recentFirst).apply()
    }
    // One launch path for the grid cell, the continue banner and the dashboard.
    val launchFromLibrary: (Game) -> Unit = { game ->
        scope.launch {
            // A mandatory-install disc is still bootable, so this asks
            // rather than diverting the launch on its own.
            val pending = viewModel.uninstalledDiscContent(game)
            if (pending.isNotEmpty()) {
                pendingDiscInstall = game to pending.size
            } else {
                recordLaunch(game)
                launchGame(context, viewModel, game)
            }
        }
    }
    var allFilesGranted by remember { mutableStateOf(AllFilesAccess.isGranted()) }
    // Not-yet-granted sends the user to Settings; the grant returns no result, so it is
    // observed on the next ON_START.
    val startRealPathMode: () -> Unit = {
        if (AllFilesAccess.isGranted()) showBrowser = true
        else AllFilesAccess.requestAccess(context)
    }

    // Re-scan on return to foreground to pick up games added while backgrounded. The
    // ViewModel's init does the first cold-start load, so the first ON_START is skipped.
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        var firstStart = true
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_START) {
                allFilesGranted = AllFilesAccess.isGranted()
                if (firstStart) firstStart = false else viewModel.refresh()
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    if (showBrowser) {
        FolderBrowserScreen(
            onFolderChosen = { path ->
                showBrowser = false
                viewModel.onRealPathFolderPicked(path)
            },
            onCancel = { showBrowser = false },
        )
        return
    }

    NxeSurface(enabled = dashboardView) {
    Scaffold(
        // The dashboard paints its own animated backdrop behind the whole screen.
        containerColor = if (dashboardView) Color.Transparent else MaterialTheme.colorScheme.background,
        contentColor = MaterialTheme.colorScheme.onBackground,
        topBar = {
            TopAppBar(
                title = {
                    if (searching) {
                        val focus = remember { FocusRequester() }
                        LaunchedEffect(Unit) { focus.requestFocus() }
                        TextField(
                            value = query,
                            onValueChange = { query = it },
                            placeholder = { Text(l10n("Search games", "Cerca giochi")) },
                            singleLine = true,
                            colors = TextFieldDefaults.colors(
                                focusedContainerColor = MaterialTheme.colorScheme.surfaceContainerHigh,
                                unfocusedContainerColor = MaterialTheme.colorScheme.surfaceContainerHigh,
                                focusedIndicatorColor = androidx.compose.ui.graphics.Color.Transparent,
                                unfocusedIndicatorColor = androidx.compose.ui.graphics.Color.Transparent,
                            ),
                            shape = RoundedCornerShape(50),
                            modifier = Modifier.fillMaxWidth().padding(end = 8.dp).focusRequester(focus),
                        )
                    } else Row(verticalAlignment = Alignment.CenterVertically) {
                        // The dashboard greets the signed-in profile, like the console.
                        val signedIn = profile?.takeIf { dashboardView && it.gamertag.isNotEmpty() }
                        if (signedIn != null) {
                            GamerPicture(signedIn)
                            Spacer(Modifier.width(12.dp))
                        }
                        Column {
                        Text(
                            signedIn?.gamertag ?: l10n("Library", "Libreria"),
                            style = if (dashboardView) MaterialTheme.typography.titleMedium.copy(fontSize = 18.sp)
                                else MaterialTheme.typography.titleLarge,
                            fontWeight = if (dashboardView) FontWeight.SemiBold else FontWeight.Bold,
                            maxLines = 1,
                        )
                        (state as? LibraryUiState.Loaded)?.let { s ->
                            Text(
                                if (s.fromCache && isRefreshing)
                                    l10n("${s.games.size} games · checking…", "${s.games.size} giochi · verifica…")
                                else l10n("${s.games.size} games", "${s.games.size} giochi"),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                        }
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = if (dashboardView) Color.Transparent
                        else MaterialTheme.colorScheme.background,
                ),
                actions = {
                    // On the dashboard the actions are glass discs over the game art.
                    val actionMod = if (dashboardView) Modifier
                        .padding(horizontal = 5.dp)
                        .requiredSize(40.dp)
                        .clip(CircleShape)
                        .background(Color.White.copy(alpha = 0.12f))
                        .border(1.dp, Color.White.copy(alpha = 0.22f), CircleShape)
                    else Modifier
                    // The dashboard keeps three discs; its way back to the grid is in the menu.
                    if (!dashboardView) IconButton(onClick = {
                        dashboardView = true
                        LibraryStyle.setDashboard(context, true)
                    }) {
                        Icon(NxeIcons.Dashboard, contentDescription = l10n("Xbox covers view", "Vista copertine Xbox"))
                    }
                    IconButton(modifier = actionMod, onClick = {
                        if (searching) query = ""
                        searching = !searching
                    }) {
                        Icon(
                            if (searching) Icons.Default.Close else Icons.Default.Search,
                            contentDescription = if (searching) l10n("Close search", "Chiudi ricerca") else l10n("Search", "Cerca"),
                        )
                    }
                    IconButton(onClick = onOpenSettings, modifier = actionMod) {
                        Icon(Icons.Default.Settings, contentDescription = l10n("Settings", "Impostazioni"))
                    }
                    var menuOpen by remember { mutableStateOf(false) }
                    IconButton(onClick = { menuOpen = true }, modifier = actionMod) {
                        Icon(Icons.Default.MoreVert, contentDescription = l10n("More", "Altro"))
                    }
                    DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                        if (dashboardView) {
                            DropdownMenuItem(
                                text = { Text(l10n("Classic grid view", "Vista a griglia classica")) },
                                onClick = {
                                    menuOpen = false
                                    dashboardView = false
                                    LibraryStyle.setDashboard(context, false)
                                },
                            )
                        }
                        // Only offered where All Files Access exists (API 30+); on API 29 the
                        // empty state explains why.
                        if (AllFilesAccess.isSupported) {
                            DropdownMenuItem(
                                text = { Text(l10n("Games folder", "Cartella dei giochi")) },
                                onClick = { menuOpen = false; startRealPathMode() },
                            )
                        }
                        DropdownMenuItem(
                            text = { Text(l10n("Install content", "Installa contenuti")) },
                            onClick = { menuOpen = false; onOpenInstallContent() },
                        )
                        DropdownMenuItem(
                            text = { Text(l10n("Profiles", "Profili")) },
                            onClick = { menuOpen = false; onOpenProfiles() },
                        )
                        DropdownMenuItem(
                            text = { Text(l10n("Key mapping", "Mappatura tasti")) },
                            onClick = { menuOpen = false; onOpenKeymap() },
                        )
                        DropdownMenuItem(
                            text = { Text(l10n("Touch controls", "Comandi touch")) },
                            onClick = { menuOpen = false; onOpenTouchControls() },
                        )
                        DropdownMenuItem(
                            text = { Text(l10n("Open user data", "Apri dati utente")) },
                            onClick = {
                                menuOpen = false
                                openUserData(context)
                            },
                        )
                        DropdownMenuItem(
                            text = { Text(l10n("About", "Informazioni")) },
                            onClick = { menuOpen = false; onOpenAbout() },
                        )

                        DropdownMenuItem(
                            text = { Text(l10n("Check for updates", "Cerca aggiornamenti")) },
                            onClick = {
                                menuOpen = false

                                checkForUpdatesClicked(
                                    context = context,
                                    scope = scope,
                                    onResult = { updateResult = it }
                                )
                            }
                        )
                    }
                }
            )
        }
    ) { padding ->
        PullToRefreshBox(
            // The dashboard header already says a check is running; no spinner over the art.
            isRefreshing = isRefreshing && !dashboardView,
            onRefresh = { viewModel.refresh() },
            modifier = Modifier.fillMaxSize().padding(padding),
        ) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            val setFolderLabel = if (allFilesGranted) l10n("Choose the games folder", "Scegli la cartella dei giochi") else l10n("Allow access to files", "Consenti l'accesso ai file")
            when (val s = state) {
                LibraryUiState.NoVulkan ->
                    NoVulkanDialog(onQuit = { (context as? Activity)?.finish() })
                LibraryUiState.Loading -> Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(16.dp),
                ) {
                    CircularProgressIndicator()
                    Text(loadingMessage, style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                // All Files Access is API 30+; on API 29 there is no games path at all.
                LibraryUiState.NoFolder ->
                    if (AllFilesAccess.isSupported)
                        EmptyMessage(l10n("No games folder", "Nessuna cartella dei giochi"), setFolderLabel,
                            onAction = startRealPathMode)
                    else
                        EmptyMessage(
                            l10n("Choosing a games folder needs Android 11 or newer.", "Per scegliere una cartella dei giochi serve Android 11 o più recente."),
                            "OK", onAction = {})
                LibraryUiState.PermissionLost ->
                    EmptyMessage(l10n("Folder access lost", "Accesso alla cartella perso"), setFolderLabel,
                        onAction = startRealPathMode)
                is LibraryUiState.Error ->
                    EmptyMessage(s.message, l10n("Retry", "Riprova"), onAction = { viewModel.refresh() })
                is LibraryUiState.Loaded ->
                    if (s.games.isEmpty())
                        EmptyMessage(l10n("No games in this folder", "Nessun gioco in questa cartella"), l10n("Choose another", "Scegline un'altra"),
                            onAction = startRealPathMode)
                    else {
                        val shown = remember(s.games, query, recentFirst, lastPlayedId) {
                            orderLibraryGames(filterGames(s.games, query), recentFirst) { game ->
                                uiPrefs.getLong("played_${game.stableId}",
                                    if (game.stableId == lastPlayedId) 1L else 0L)
                            }
                        }
                        val continueGame = if (query.isBlank() && viewModel.canLaunchGames)
                            s.games.firstOrNull { it.stableId == lastPlayedId } else null
                        val sortLabel = if (recentFirst) l10n("Sort: recently launched", "Ordine: ultimi avviati") else l10n("Sort: name", "Ordine: nome")
                        if (dashboardView) {
                            if (shown.isEmpty()) NoMatchMessage(query)
                            else NxeDashboard(
                                games = shown,
                                viewModel = viewModel,
                                // Only destinations the library already reaches. Saves are per
                                // game: same title-id request as the options sheet's "Saves".
                                channels = listOf(
                                    NxeChannel(l10n("Games", "Giochi")),
                                    NxeChannel(l10n("Saves", "Salvataggi"), l10n("Open saves", "Apri salvataggi")) { g ->
                                        g?.let { viewModel.requestSaves(it) }
                                    },
                                    NxeChannel(l10n("Content", "Contenuti"), l10n("Install content", "Installa contenuti")) { onOpenInstallContent() },
                                    NxeChannel(l10n("Settings", "Impostazioni"), l10n("Open settings", "Apri impostazioni")) { onOpenSettings() },
                                ),
                                continueGame = continueGame,
                                sortLabel = sortLabel,
                                onToggleSort = toggleSort,
                                onLaunch = launchFromLibrary,
                                onGameOptions = { pendingGame = it },
                                takeFocus = !searching,
                            )
                        } else Column(Modifier.fillMaxSize()) {
                            // The things people look for first, without digging in the overflow menu.
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .horizontalScroll(rememberScrollState())
                                    .padding(horizontal = 16.dp),
                                horizontalArrangement = Arrangement.spacedBy(8.dp),
                            ) {
                                AssistChip(onClick = onOpenTouchControls, label = { Text(l10n("Touch controls", "Comandi touch")) },
                                    leadingIcon = { Icon(Icons.Default.Edit, null, Modifier.size(18.dp)) })
                                AssistChip(onClick = onOpenKeymap, label = { Text("Controller") },
                                    leadingIcon = { Icon(Icons.Default.Build, null, Modifier.size(18.dp)) })
                                AssistChip(onClick = onOpenSettings, label = { Text(l10n("Settings", "Impostazioni")) },
                                    leadingIcon = { Icon(Icons.Default.Settings, null, Modifier.size(18.dp)) })
                                AssistChip(onClick = onOpenInstallContent, label = { Text(l10n("Install content", "Installa contenuti")) },
                                    leadingIcon = { Icon(Icons.Default.Add, null, Modifier.size(18.dp)) })
                                AssistChip(onClick = toggleSort, label = { Text(sortLabel) })
                            }
                            if (shown.isEmpty()) NoMatchMessage(query)
                            else GameGrid(
                                games = shown,
                                continueGame = continueGame,
                                viewModel = viewModel,
                                onLaunch = launchFromLibrary,
                                onLongPress = { pendingGame = it },
                            )
                        }
                    }
            }
        }
        }
    }
    }

   when (val result = updateResult) {
        is UpdateResult.Available -> {
            UpdateDialog(
                release = result.release,
                onDismiss = { updateResult = null }
            )
        }

        is UpdateResult.Latest -> {
            LatestVersionDialog(
                commitHash = result.commitHash,
                onDismiss = { updateResult = null }
            )
        }

        is UpdateResult.Cooldown -> {
            CooldownDialog(
                remainingMillis = result.remainingMillis,
                onDismiss = { updateResult = null }
            )
        }

        null -> {}
    }

    pendingDiscInstall?.let { (game, count) ->
        AlertDialog(
            onDismissRequest = { pendingDiscInstall = null },
            title = { Text(l10n("Disc content", "Contenuti del disco")) },
            text = {
                Text(l10n("This disc contains $count packages the game needs. ", "Questo disco contiene $count pacchetti necessari al gioco. ") +
                     l10n("You can install them now or try to launch without installing them.", "Puoi installarli adesso oppure provare ad avviare senza installarli."))
            },
            confirmButton = {
                TextButton(onClick = {
                    pendingDiscInstall = null
                    onInstallFromDisc(game.launchUri)
                }) { Text(l10n("Install", "Installa")) }
            },
            dismissButton = {
                TextButton(onClick = {
                    pendingDiscInstall = null
                    recordLaunch(game)
                    launchGame(context, viewModel, game)
                }) { Text(l10n("Launch without installing", "Avvia senza installare")) }
            },
        )
    }

    pendingGame?.let { game ->
        val dismiss = { pendingGame = null; viewModel.clearTitleIdRequest() }
        val sheetState = rememberModalBottomSheetState()
        ModalBottomSheet(
            onDismissRequest = dismiss,
            sheetState = sheetState,
        ) {
            Column {
                // The title-id status line shows ONLY while resolving or on error.
                val statusContent: (@Composable () -> Unit)? = when (val st = titleIdState) {
                    is TitleIdState.Loading -> ({
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            CircularProgressIndicator(Modifier.size(16.dp))
                            Spacer(Modifier.width(8.dp))
                            Text(l10n("Reading Title ID…", "Lettura del Title ID…"))
                        }
                    })
                    is TitleIdState.Error -> ({ Text(st.message) })
                    else -> null
                }
                ListItem(
                    headlineContent = {
                        Text(game.name, style = MaterialTheme.typography.titleLarge)
                        if (game.isMultiDisc) {
                            Text(
                                l10n("Disc ${game.discNumber} of ${game.discCount}", "Disco ${game.discNumber} di ${game.discCount}"),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    },
                    supportingContent = if (game.titleId != null || game.mediaId != null || statusContent != null) {
                        {
                            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                game.titleId?.let { Text("Title ID: $it") }
                                game.mediaId?.let { Text("Media ID: $it") }
                                statusContent?.invoke()
                            }
                        }
                    } else {
                        null
                    },
                )

                val perGameEnabled = titleIdState !is TitleIdState.Loading
                ListItem(
                    headlineContent = { Text(l10n("Game settings", "Impostazioni del gioco")) },
                    colors = if (perGameEnabled) {
                        ListItemDefaults.colors()
                    } else {
                        ListItemDefaults.colors(
                            headlineColor =
                                MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f),
                        )
                    },
                    modifier = Modifier.clickable(enabled = perGameEnabled) {
                        viewModel.requestPerGameSettings(game)
                    },
                )

                ListItem(
                    headlineContent = { Text(l10n("Game patches", "Patch del gioco")) },
                    colors = if (perGameEnabled) {
                        ListItemDefaults.colors()
                    } else {
                        ListItemDefaults.colors(
                            headlineColor =
                                MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f),
                        )
                    },
                    modifier = Modifier.clickable(enabled = perGameEnabled) {
                        viewModel.requestGamePatches(game)
                    },
                )

                ListItem(
                    headlineContent = { Text(l10n("Saves", "Salvataggi")) },
                    colors = if (perGameEnabled) {
                        ListItemDefaults.colors()
                    } else {
                        ListItemDefaults.colors(
                            headlineColor =
                                MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f),
                        )
                    },
                    modifier = Modifier.clickable(enabled = perGameEnabled) {
                        viewModel.requestSaves(game)
                    },
                )

                ListItem(
                    headlineContent = { Text(l10n("Manage content", "Gestisci contenuti")) },
                    colors = if (perGameEnabled) {
                        ListItemDefaults.colors()
                    } else {
                        ListItemDefaults.colors(
                            headlineColor =
                                MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f),
                        )
                    },
                    modifier = Modifier.clickable(enabled = perGameEnabled) {
                        viewModel.requestContentManager(game)
                    },
                )

                if (game.format == GameFormat.ISO) {
                    ListItem(
                        headlineContent = { Text(l10n("Compress to .zar", "Comprimi in .zar")) },
                        modifier = Modifier.clickable {
                            compressConfirmFor = game
                            pendingGame = null
                            viewModel.clearTitleIdRequest()
                        },
                    )
                }

                if (viewModel.canLaunchGames && viewModel.isPinShortcutSupported) {
                    ListItem(
                        headlineContent = { Text(l10n("Create shortcut", "Crea collegamento")) },
                        modifier = Modifier.clickable {
                            viewModel.createShortcut(game)
                            dismiss()
                        },
                    )
                }
            }
        }
    }

    LaunchedEffect(titleIdState) {
        // The dashboard's l10n("Saves", "Salvataggi") channel resolves without the options sheet open, so a
        // failure there has no status line to land on.
        (titleIdState as? TitleIdState.Error)?.let { e ->
            if (pendingGame == null) {
                Toast.makeText(context, e.message, Toast.LENGTH_SHORT).show()
                viewModel.clearTitleIdRequest()
            }
        }
        (titleIdState as? TitleIdState.Resolved)?.let { r ->
            when (r.action) {
                GameAction.PER_GAME_SETTINGS ->
                    onOpenPerGameSettings(r.titleId, r.game.name, r.game.format, r.game.launchUri)
                GameAction.GAME_PATCHES ->
                    onOpenGamePatches(r.titleId, r.game.name)
                GameAction.SAVES ->
                    onOpenSaves(r.titleId, r.game.name)
                GameAction.MANAGE_CONTENT ->
                    onOpenContentManager(r.titleId, r.game.name)
            }
            pendingGame = null
            viewModel.clearTitleIdRequest()
        }
    }

    compressConfirmFor?.let { game ->
        AlertDialog(
            onDismissRequest = { compressConfirmFor = null },
            title = { Text(l10n("Compress to .zar?", "Comprimere in .zar?")) },
            text = {
                Text(
                    l10n("The disc is compressed into a smaller .zar. The original .iso is not touched ", "Il disco viene compresso in un .zar più piccolo. L'.iso originale non si tocca ") +
                        l10n("until the .zar is created and verified, and you are asked before it is ", "finché il .zar non è creato e verificato, e prima di cancellarlo ti viene ") +
                        l10n("deleted. The game stays in the library.", "chiesto. Il gioco resta nella libreria."))
            },
            confirmButton = {
                TextButton(onClick = {
                    compressConfirmFor = null
                    compressVm.compress(game.launchUri)
                }) { Text(l10n("Compress", "Comprimi")) }
            },
            dismissButton = {
                TextButton(onClick = { compressConfirmFor = null }) { Text(l10n("Cancel", "Annulla")) }
            },
        )
    }

    when (val s = compressState) {
        is CompressState.Busy -> AlertDialog(
            onDismissRequest = {},   // not cancelable while running
            title = { Text(s.message) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (s.progress >= 0f) {
                        LinearProgressIndicator(
                            progress = { s.progress },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        Text(l10n("${(s.progress * 100).toInt()}%  ·  this may take a while.", "${(s.progress * 100).toInt()}%  ·  può volerci un po'."))
                    } else {
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                        Text(l10n("This may take a while.", "Può volerci un po'."))
                    }
                }
            },
            confirmButton = {},
        )
        is CompressState.ConfirmDelete -> AlertDialog(
            // Dismissing keeps it: a stray tap outside must never delete the .iso.
            onDismissRequest = compressVm::keepIso,
            title = { Text(l10n("Delete the original .iso?", "Cancellare l'.iso originale?")) },
            text = {
                Text(
                    l10n("“${s.zarName}” has been created and verified. Deleting “${s.isoName}” ", "“${s.zarName}” è stato creato e verificato. Cancellare “${s.isoName}” ") +
                        l10n("frees ${formatBytes(s.isoBytes)}.", "libera ${formatBytes(s.isoBytes)}."))
            },
            confirmButton = {
                TextButton(onClick = compressVm::deleteIso) { Text(l10n("Delete .iso", "Cancella .iso")) }
            },
            dismissButton = { TextButton(onClick = compressVm::keepIso) { Text(l10n("Keep it", "Tienilo")) } },
        )
        is CompressState.Done -> AlertDialog(
            onDismissRequest = { compressVm.dismiss(); viewModel.refresh() },
            title = { Text(l10n("Done", "Fatto")) },
            text = { Text(s.message) },
            confirmButton = {
                TextButton(onClick = { compressVm.dismiss(); viewModel.refresh() }) { Text("OK") }
            },
        )
        is CompressState.Failed -> AlertDialog(
            onDismissRequest = compressVm::dismiss,
            title = { Text(l10n("Failed", "Non riuscito")) },
            text = { Text(s.message) },
            confirmButton = { TextButton(onClick = compressVm::dismiss) { Text("OK") } },
        )
        else -> {}
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameGrid(
    games: List<Game>,
    viewModel: GameLibraryViewModel,
    onLaunch: (Game) -> Unit,
    onLongPress: (Game) -> Unit,
    continueGame: Game? = null,
) {
    LazyVerticalGrid(
        columns = GridCells.Adaptive(minSize = 150.dp),
        contentPadding = PaddingValues(16.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
        modifier = Modifier.fillMaxSize(),
    ) {
        if (continueGame != null) {
            item(key = "continue", span = { GridItemSpan(maxLineSpan) }) {
                ContinueBanner(continueGame, viewModel, onLaunch)
            }
        }
        items(games, key = { it.stableId }) { game ->
            GameCell(game, viewModel, onLaunch, onLongPress)
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameCell(
    game: Game,
    viewModel: GameLibraryViewModel,
    onLaunch: (Game) -> Unit,
    onLongPress: (Game) -> Unit,
) {
    val context = LocalContext.current
    // Once per cell: the File.exists() stat must not run on every recomposition while
    // scrolling.
    val iconModel = remember(game.stableId) { viewModel.iconFileOrFallback(game) }
    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(MaterialTheme.colorScheme.surfaceContainerHigh)
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(16.dp))
            .combinedClickable(onClick = { onLaunch(game) }, onLongClick = { onLongPress(game) }),
    ) {
    // Per-game settings, patches and saves: same sheet as a long press, but visible.
    IconButton(onClick = { onLongPress(game) }, modifier = Modifier.align(Alignment.TopEnd)) {
        Icon(Icons.Default.MoreVert, contentDescription = l10n("Game options", "Opzioni gioco"),
            tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
    Text(
        formatBadge(game.format),
        style = MaterialTheme.typography.labelSmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier
            .align(Alignment.TopStart)
            .padding(10.dp)
            .background(MaterialTheme.colorScheme.surfaceContainerLowest, RoundedCornerShape(6.dp))
            .padding(horizontal = 6.dp, vertical = 2.dp),
    )
    Column(
        Modifier
            .fillMaxWidth()
            // The badge and the options button own the strip above the cover.
            .padding(start = 12.dp, end = 12.dp, top = 40.dp, bottom = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        AsyncImage(
            model = ImageRequest.Builder(context)
                .data(iconModel)
                .build(),
            contentDescription = game.name,
            modifier = Modifier
                .size(88.dp)
                .clip(RoundedCornerShape(14.dp)),
        )
        Spacer(Modifier.height(10.dp))
        Text(
            game.name,
            style = MaterialTheme.typography.titleSmall,
            textAlign = TextAlign.Center,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
        // A set shares one title, so the tiles would otherwise be identical.
        if (game.isMultiDisc) {
            Text(
                l10n("Disc ${game.discNumber} of ${game.discCount}", "Disco ${game.discNumber} di ${game.discCount}"),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
            )
        }
        if (viewModel.canLaunchGames) {
            Spacer(Modifier.height(8.dp))
            Row(
                Modifier
                    .background(MaterialTheme.colorScheme.primaryContainer, RoundedCornerShape(50))
                    .padding(start = 8.dp, end = 12.dp, top = 3.dp, bottom = 3.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(Icons.Filled.PlayArrow, null, Modifier.size(16.dp),
                    tint = MaterialTheme.colorScheme.onPrimaryContainer)
                Text(
                    l10n("Launch", "Avvia"),
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onPrimaryContainer,
                )
            }
        }
    }
    }
}

@Composable
private fun ContinueBanner(game: Game, viewModel: GameLibraryViewModel, onLaunch: (Game) -> Unit) {
    val context = LocalContext.current
    val iconModel = remember(game.stableId) { viewModel.iconFileOrFallback(game) }
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(MaterialTheme.colorScheme.surfaceContainerHigh)
            .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(16.dp))
            .clickable { onLaunch(game) }
            .padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        AsyncImage(
            model = ImageRequest.Builder(context).data(iconModel).build(),
            contentDescription = null,
            modifier = Modifier.size(48.dp).clip(RoundedCornerShape(12.dp)),
        )
        Column(Modifier.weight(1f).padding(horizontal = 14.dp)) {
            Text(l10n("CONTINUE PLAYING", "CONTINUA A GIOCARE"), style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontWeight = FontWeight.Bold)
            Text(game.name, style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis,
                color = MaterialTheme.colorScheme.onSurface)
        }
        Row(
            Modifier
                .background(MaterialTheme.colorScheme.primary, RoundedCornerShape(50))
                .padding(start = 12.dp, end = 18.dp, top = 9.dp, bottom = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(Icons.Filled.PlayArrow, null, tint = MaterialTheme.colorScheme.onPrimary)
            Text(l10n("Launch", "Avvia"), color = MaterialTheme.colorScheme.onPrimary, fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(start = 4.dp))
        }
    }
}

/** Short container label shown on a game (grid badge, dashboard detail line). */
internal fun formatBadge(format: GameFormat): String = when (format) {
    GameFormat.ISO -> "ISO"; GameFormat.ZAR -> "ZAR"; GameFormat.GOD -> "GOD"
    GameFormat.XEX_FOLDER -> "XEX"; GameFormat.STFS -> "XBLA"
}

@Composable
private fun NoMatchMessage(query: String) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(l10n("No game contains “${query.trim()}”", "Nessun gioco contiene “${query.trim()}”"),
            color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun EmptyMessage(
    text: String,
    action: String,
    onAction: () -> Unit,
) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            Modifier
                .background(MaterialTheme.colorScheme.primaryContainer, CircleShape)
                .padding(24.dp),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                Icons.Filled.PlayArrow,
                contentDescription = null,
                modifier = Modifier.size(48.dp),
                tint = MaterialTheme.colorScheme.onPrimaryContainer,
            )
        }
        Spacer(Modifier.height(16.dp))
        Text(text, style = MaterialTheme.typography.bodyLarge)
        Spacer(Modifier.height(12.dp))
        Button(onClick = onAction) { Text(action) }
    }
}

@Composable
private fun NoVulkanDialog(onQuit: () -> Unit) {
    AlertDialog(
        onDismissRequest = onQuit,
        confirmButton = { TextButton(onClick = onQuit) { Text(l10n("Exit", "Esci")) } },
        title = { Text(l10n("Device not supported", "Dispositivo non supportato")) },
        text = { Text(l10n("This device has no Vulkan GPU: the emulator cannot run.", "Questo dispositivo non ha una GPU Vulkan: l'emulatore non può funzionare.")) },
    )
}

fun checkForUpdatesClicked(
    context: Context,
    scope: CoroutineScope,
    onResult: (UpdateResult) -> Unit
) {
    scope.launch {
        if (!shouldCheckForUpdates(context)) {
            Log.d("Updater", "Skipping update check")
            onResult(UpdateResult.Cooldown(getRemainingCooldown(context)))
            return@launch
        }

        try {
            val result = checkForUpdates()
            saveLastCheck(context)
            onResult(result)
        } catch (e: Exception) {
            Log.e("Updater", "Failed to check updates", e)
        }
    }
}

/** Same shape as the content-install formatter, which is private to that file. */
private fun formatBytes(b: Long): String {
    if (b < 1024) return "$b B"
    val u = arrayOf("KB", "MB", "GB", "TB")
    var v = b.toDouble()
    var i = -1
    do { v /= 1024.0; i++ } while (v >= 1024.0 && i < u.lastIndex)
    return "%.1f %s".format(v, u[i])
}

/** Reap any stale/orphaned :emu first (single-shot core). The new :emu links itself to the
 *  launcher by binding MainAliveService, so nothing rides on the Intent. */
private fun launchGame(context: Context, viewModel: GameLibraryViewModel, game: Game) {
    runCatching {
        EmuProcessLink.killStaleEmu(context)
        context.startActivity(viewModel.buildLaunchIntent(game))
    }
}

/** Round gamer picture with the dashboard's green ring; the initial when there is none. */
@Composable
private fun GamerPicture(profile: GameLibraryViewModel.ActiveProfile) {
    Box(
        Modifier
            .size(42.dp)
            .clip(CircleShape)
            .background(
                androidx.compose.ui.graphics.Brush.radialGradient(
                    listOf(Color(0xFF9BE15D), Color(0xFF107C10)),
                ),
            )
            .border(2.dp, Color.White.copy(alpha = 0.85f), CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        if (profile.picture != null) {
            AsyncImage(
                model = profile.picture,
                contentDescription = null,
                contentScale = androidx.compose.ui.layout.ContentScale.Crop,
                modifier = Modifier.fillMaxSize(),
            )
        } else {
            Text(
                profile.gamertag.take(1).uppercase(),
                color = Color.White,
                fontWeight = FontWeight.SemiBold,
                style = MaterialTheme.typography.titleMedium,
            )
        }
    }
}
