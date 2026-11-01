@file:OptIn(ExperimentalFoundationApi::class)

package xendroid.compose.ui.library

import xendroid.compose.l10n

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.VectorConverter
import androidx.compose.animation.core.animate
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.animateValue
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.LocalOverscrollConfiguration
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.Orientation
import androidx.compose.foundation.gestures.draggable
import androidx.compose.foundation.gestures.rememberDraggableState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableFloatState
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.draw.blur
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.geometry.CornerRadius
import android.graphics.BlurMaskFilter
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathBuilder
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEvent
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.unit.IntOffset
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.offset
import androidx.compose.animation.Crossfade
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.staticCompositionLocalOf
import coil.compose.AsyncImagePainter
import coil.compose.rememberAsyncImagePainter
import kotlinx.coroutines.delay
import xendroid.compose.ui.theme.XdFont
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.coerceAtLeast
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.zIndex
import androidx.core.view.WindowCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.currentStateAsState
import coil.compose.AsyncImage
import coil.request.ImageRequest
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import xendroid.compose.data.Game
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.rememberUpdatedState
import coil.imageLoader
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.drawText
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Density
import androidx.compose.ui.graphics.painter.Painter
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.geometry.RoundRect
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sign
import kotlin.math.sin
import kotlin.math.cos

/*
 * Library dashboard in the spirit of the Xbox 360 "New Xbox Experience": the focused game's
 * Marketplace background behind everything (a game theme, as on the console), a channel row
 * on top and a cover flow of box art with reflections in the middle. Artwork comes from
 * [GameArt]; without it the dashboard falls back to its own dark green backdrop and the
 * icons embedded in the games.
 *
 * Cost budget (the phone also runs the emulator): nothing animates while the dashboard sits
 * still; tile motion only touches graphicsLayer properties, so a drag or a glide never
 * recomposes or re-lays-out anything.
 */

// ---- palette -------------------------------------------------------------------------------

private val NxeInk = Color.White
private val NxeInkSoft = Color.White.copy(alpha = 0.78f)
private val NxeGreen = Color(0xFF107C10)
private val NxeGreenBright = Color(0xFF6CCB2C)
private val NxeBgCenter = Color(0xFF24461F)
private val NxeBgMid = Color(0xFF10230F)
private val NxeBgEdge = Color(0xFF050B05)
private val NxeGlyphY = Color(0xFFE2A800)
private val NxeGlass = Color.White.copy(alpha = 0.14f)
private val NxeGlassEdge = Color.White.copy(alpha = 0.34f)
private val NxeTextShadow = Shadow(Color.Black.copy(alpha = 0.7f), Offset(0f, 3f), 12f)

/** Dark scheme for every Material widget (top bar, menus) over the game-themed backdrop. */
private val NxeColorScheme = darkColorScheme(
    primary = NxeGreenBright,
    onPrimary = Color.White,
    primaryContainer = NxeGreen,
    onPrimaryContainer = Color.White,
    secondary = Color(0xFFB9CCB3),
    onSecondary = Color(0xFF1B2A1A),
    secondaryContainer = Color(0xFF243425),
    onSecondaryContainer = Color(0xFFD5E8CF),
    background = NxeBgEdge,
    onBackground = Color.White,
    surface = Color(0xFF142014),
    onSurface = Color.White,
    surfaceVariant = Color(0xFF1E2B1E),
    onSurfaceVariant = NxeInkSoft,
    surfaceContainerLowest = Color(0xFF070D07),
    surfaceContainerLow = Color(0xFF0F1A0F),
    surfaceContainer = Color(0xFF142014),
    surfaceContainerHigh = Color(0xFF1A281A),
    surfaceContainerHighest = Color(0xFF213121),
    outline = Color.White.copy(alpha = 0.4f),
    outlineVariant = Color.White.copy(alpha = 0.18f),
)

private val TileShape = RoundedCornerShape(6.dp)
private val IconShape = RoundedCornerShape(percent = 10)
private val TileBaseBrush = Brush.verticalGradient(listOf(Color(0xFF2B3A2B), Color(0xFF0E160E)))
private val PrimaryShape = RoundedCornerShape(16.dp)
private val PrimaryBrush = Brush.verticalGradient(listOf(Color(0xFF66C629), Color(0xFF168B16)))
private val PrimarySheen = Brush.verticalGradient(
    0f to Color.White.copy(alpha = 0.24f), 0.5f to Color.White.copy(alpha = 0.04f), 0.5001f to Color.Transparent,
)
/** Legibility over any game background: dark under the header and the info block. */
private val ScrimBrush = Brush.verticalGradient(
    0f to Color.Black.copy(alpha = 0.50f), 0.14f to Color.Black.copy(alpha = 0.22f),
    0.45f to Color.Black.copy(alpha = 0.14f), 0.68f to Color.Black.copy(alpha = 0.52f),
    1f to Color.Black.copy(alpha = 0.86f),
)

// Cover flow shape, indexed by distance from the focused tile (0 = focused). Spread is in
// focused-tile widths; the side tiles turn towards the centre like the NXE stack.
private val TILE_SCALE = floatArrayOf(1f, 0.86f, 0.74f, 0.64f, 0.56f)
private val TILE_SPREAD = floatArrayOf(0f, 0.8f, 1.3f, 1.7f, 2.05f)
private val TILE_ALPHA = floatArrayOf(1f, 1f, 0.85f, 0.5f, 0f)
private const val TILE_TURN_DEG = 50f       // rotationY of a neighbour at rest
private const val REFLECTION = 0.3f         // reflection height, in tile heights
private const val REFLECTION_LANDSCAPE = 0.16f  // shorter on a screen on its side: bigger cards
private const val REFLECTION_GAP_DP = 5
private const val TILE_WINDOW = 4          // tiles composed on each side of the focused one
private const val TILE_TAPPABLE = 3        // the outermost slot is fading in/out: not a target


/** Piecewise-linear lookup in [k] at fractional distance [a]; [extend] continues the last
 *  segment past the table instead of clamping. Allocation-free (runs in graphicsLayer). */
private fun knot(k: FloatArray, a: Float, extend: Boolean = false): Float {
    if (a <= 0f) return k[0]
    val last = k.size - 1
    if (a >= last) return if (extend) k[last] + (a - last) * (k[last] - k[last - 1]) else k[last]
    val i = a.toInt()
    return k[i] + (k[i + 1] - k[i]) * (a - i)
}

// ---- public API ----------------------------------------------------------------------------

/** One label of the channel row. [open] == null marks the games channel itself; the others
 *  receive the focused game (per-game destinations such as saves need it). */
data class NxeChannel(
    val label: String,
    /** Primary-button text while this channel is highlighted (games channel: null). */
    val actionLabel: String? = null,
    val open: ((Game?) -> Unit)? = null,
)

/** What the backdrop shows for the focused game. [titleId] finds its Marketplace art; [icon],
 *  the picture embedded in the game, stands in when the archive has none or there is no
 *  network, so every game gets a themed screen. */
@Immutable
data class NxeBackdrop(val key: String, val titleId: String?, val icon: Any)

/** The focused game's backdrop; set by [NxeDashboard]. */
private val LocalNxeBackdrop = staticCompositionLocalOf<MutableState<NxeBackdrop?>?> { null }

/** Wraps the library screen while the dashboard is active: dark NXE colour scheme, the
 *  game-themed backdrop behind everything (top bar included) and light status-bar icons.
 *  With [enabled] false it is a pass-through, and the tree shape stays the same either way. */
@Composable
fun NxeSurface(enabled: Boolean, content: @Composable () -> Unit) {
    val backdrop = remember { mutableStateOf<NxeBackdrop?>(null) }
    MaterialTheme(
        colorScheme = if (enabled) NxeColorScheme else MaterialTheme.colorScheme,
        typography = MaterialTheme.typography,
        shapes = MaterialTheme.shapes,
    ) {
        CompositionLocalProvider(LocalNxeBackdrop provides backdrop) {
            Box(Modifier.fillMaxSize()) {
                if (enabled) {
                    NxeBackground(backdrop.value, Modifier.matchParentSize())
                    NxeDarkStatusBar()
                }
                content()
            }
        }
    }
}

/**
 * The game library as an NXE-style dashboard. [games] must be the already filtered/sorted
 * list the grid would show; [onLaunch] and [onGameOptions] are the grid's own tap and
 * long-press actions. Gamepad: D-pad left/right move, A/Enter activate, Y/Menu options,
 * L1/R1 switch channel.
 */
@Composable
fun NxeDashboard(
    games: List<Game>,
    viewModel: GameLibraryViewModel,
    channels: List<NxeChannel>,
    continueGame: Game?,
    sortLabel: String,
    onToggleSort: () -> Unit,
    onLaunch: (Game) -> Unit,
    onGameOptions: (Game) -> Unit,
    takeFocus: Boolean,
    modifier: Modifier = Modifier,
) {
    if (games.isEmpty() || channels.isEmpty()) return
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    // A PackageManager query: once per dashboard, not per recomposition.
    val canLaunch = remember(viewModel) { viewModel.canLaunchGames }

    // Focus is kept by game id so a rescan, a re-sort or a search keeps the same game in front.
    var focusedId by rememberSaveable { mutableStateOf(continueGame?.stableId) }
    var channel by rememberSaveable { mutableIntStateOf(0) }
    var usingPad by remember { mutableStateOf(false) }
    val indexById = remember(games) {
        HashMap<String, Int>(games.size * 2).also { m -> games.forEachIndexed { i, g -> m[g.stableId] = i } }
    }
    val focused = indexById[focusedId] ?: 0
    // The flow sits at focused + offset (in tiles); only graphicsLayer blocks read the offset.
    // It never leaves [-0.5, 0.5]: a move across many games walks the focus through each of
    // them, so the tiles composed around the focus always cover what is on screen, however
    // fast the flow runs.
    val offset = remember { mutableFloatStateOf(0f) }
    val motion = remember { arrayOfNulls<Job>(1) }
    // Where the running glide lands (-1: none), so quick repeated presses add up.
    val glideTarget = remember { mutableIntStateOf(-1) }
    // The glide outlives a recomposition that brings a new list: it reads the latest one.
    val gamesNow by rememberUpdatedState(games)
    val indexNow by rememberUpdatedState(indexById)
    val ch = channel.coerceIn(0, channels.lastIndex)

    fun currentIndex(): Int = indexNow[focusedId] ?: 0

    fun position(): Float = currentIndex() + offset.floatValue

    /** Puts the flow at [p] tiles, moving the focus along; the two ends give a little. */
    fun setPosition(p: Float) {
        val list = gamesNow
        val clamped = p.coerceIn(-0.35f, list.lastIndex + 0.35f)
        val idx = clamped.roundToInt().coerceIn(0, list.lastIndex)
        val id = list[idx].stableId
        if (id != focusedId) focusedId = id
        offset.floatValue = clamped - idx
    }

    fun glideTo(index: Int, velocity: Float = 0f, stiffness: Float = 300f) {
        val dest = index.coerceIn(0, gamesNow.lastIndex)
        glideTarget.intValue = dest
        motion[0]?.cancel()
        motion[0] = scope.launch {
            animate(position(), dest.toFloat(), velocity, spring(dampingRatio = 0.92f, stiffness = stiffness)) { v, _ ->
                setPosition(v)
            }
            glideTarget.intValue = -1
        }
    }

    fun step(n: Int) = glideTo((glideTarget.intValue.takeIf { it >= 0 } ?: currentIndex()) + n)

    fun activate() {
        // A press that lands mid-glide means the game the glide is heading to.
        val game = games.getOrNull(glideTarget.intValue.takeIf { it >= 0 } ?: currentIndex()) ?: return
        val open = channels[ch].open
        if (open == null) onLaunch(game) else { channel = 0; open(game) }
    }

    val onKey: (KeyEvent) -> Boolean = onKey@{ e ->
        when (e.key) {
            Key.DirectionLeft, Key.DirectionRight, Key.ButtonL1, Key.ButtonR1,
            Key.ButtonA, Key.Enter, Key.NumPadEnter, Key.DirectionCenter,
            Key.ButtonY, Key.Menu -> Unit
            else -> return@onKey false
        }
        // Down acts, Up is swallowed so nothing underneath reacts to the same press.
        if (e.type != KeyEventType.KeyDown) return@onKey true
        usingPad = true
        val first = e.nativeKeyEvent.repeatCount == 0
        when (e.key) {
            Key.DirectionLeft -> step(-1)
            Key.DirectionRight -> step(1)
            Key.ButtonL1 -> if (first) channel = (ch - 1).coerceAtLeast(0)
            Key.ButtonR1 -> if (first) channel = (ch + 1).coerceAtMost(channels.lastIndex)
            Key.ButtonY, Key.Menu -> if (first) games.getOrNull(currentIndex())?.let(onGameOptions)
            else -> if (first) activate()
        }
        true
    }

    val focusRequester = remember { FocusRequester() }
    LaunchedEffect(takeFocus) { if (takeFocus) runCatching { focusRequester.requestFocus() } }
    // The backdrop follows the focused game once the flow settles, not every tile of a fling.
    val backdrop = LocalNxeBackdrop.current
    val focusedGame = games.getOrNull(focused)
    LaunchedEffect(focusedGame?.stableId) {
        val game = focusedGame ?: return@LaunchedEffect
        delay(160)
        backdrop?.value = NxeBackdrop(game.stableId, game.titleId, viewModel.iconFileOrFallback(game))
    }
    // Box art for the whole library up front (tens of KB each), so a fast run through the flow
    // finds the covers ready; the large backgrounds only around the focused game.
    LaunchedEffect(games) {
        val loader = context.imageLoader
        for (g in games) GameArt.boxArtRequest(context, g.titleId)?.let(loader::enqueue)
    }
    LaunchedEffect(focused, games) {
        delay(400)
        val loader = context.imageLoader
        for (k in -2..2) {
            val g = games.getOrNull(focused + k) ?: continue
            GameArt.backgroundPrefetch(context, g.titleId)?.let(loader::enqueue)
        }
    }
    val flowAlpha by animateFloatAsState(if (ch == 0) 1f else 0.5f, label = "nxeFlowDim")

    BoxWithConstraints(modifier.fillMaxSize()) {
        val viewportH = if (constraints.hasBoundedHeight) maxHeight else 520.dp
        val wide = maxWidth >= 600.dp
        // A phone on its side: width to spare, little height.
        val landscape = maxWidth > viewportH * 1.25f
        // The vertical scroll never scrolls (content == viewport); it only gives the
        // pull-to-refresh around the library a nested-scroll source to pull from.
        CompositionLocalProvider(LocalOverscrollConfiguration provides null) {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                Column(
                    Modifier
                        .fillMaxWidth()
                        .height(viewportH)
                        .onPreviewKeyEvent(onKey)
                        .focusRequester(focusRequester)
                        .focusable(),
                ) {
                    NxeChannelRow(
                        channels = channels,
                        current = ch,
                        wide = wide,
                        compact = landscape,
                        onSelect = { i ->
                            val open = channels[i].open
                            if (open == null) channel = i
                            else { channel = 0; open(games.getOrNull(currentIndex())) }
                        },
                    )

                    // The cover flow in [mod]: each card and its reflection centred together.
                    // A phone held upright has height to spare, so the card takes more width.
                    val flow: @Composable (Modifier, Float) -> Unit = { mod, reflection ->
                        BoxWithConstraints(mod) {
                            // Room for the card and its reflection: the pair is centred together.
                            // A phone held upright has height to spare, so the card takes more width.
                            val tile = minOf(
                                maxHeight / (1f + reflection) * 0.98f,
                                maxWidth * (if (wide) 0.5f else 0.56f) / GameArt.BOX_ASPECT,
                                340.dp,
                            ).coerceAtLeast(80.dp)
                            val tilePx = with(LocalDensity.current) { tile.toPx() * GameArt.BOX_ASPECT }
                            val stepPx = tilePx * TILE_SPREAD[1]
                            val dragState = rememberDraggableState { delta -> setPosition(position() - delta / stepPx) }
                            Box(
                                Modifier
                                    .fillMaxSize()
                                    .graphicsLayer { alpha = flowAlpha }
                                    .draggable(
                                        state = dragState,
                                        orientation = Orientation.Horizontal,
                                        onDragStarted = {
                                            motion[0]?.cancel()
                                            glideTarget.intValue = -1
                                        },
                                        onDragStopped = { velocity ->
                                            // Fling, in tiles per second: a flick moves one game,
                                            // a long swipe runs through several and lands on one.
                                            val v = -velocity / stepPx
                                            val travel = (v * 0.3f).coerceIn(-15f, 15f)
                                            glideTo((position() + travel).roundToInt(), v, stiffness = 150f)
                                        },
                                    ),
                                contentAlignment = Alignment.Center,
                            ) {
                                for (slot in -TILE_WINDOW..TILE_WINDOW) {
                                    val i = focused + slot
                                    if (i in games.indices) {
                                        val game = games[i]
                                        key(game.stableId) {
                                            NxeTile(
                                                game = game,
                                                viewModel = viewModel,
                                                slot = slot,
                                                offset = offset,
                                                tile = tile,
                                            reflection = reflection,
                                                modifier = Modifier.zIndex(-abs(slot).toFloat()),
                                                onClick = {
                                                    when {
                                                        slot == 0 -> { channel = 0; onLaunch(game) }
                                                        abs(slot) <= TILE_TAPPABLE -> glideTo(i, stiffness = 240f)
                                                    }
                                                },
                                                onLongClick = {
                                                    when {
                                                        slot == 0 -> onGameOptions(game)
                                                        abs(slot) <= TILE_TAPPABLE -> glideTo(i, stiffness = 240f)
                                                    }
                                                },
                                            )
                                        }
                                    }
                                }
                            }
                        }
                    }

                    val game = games[focused.coerceIn(0, games.lastIndex)]
                    val isContinue = continueGame != null && continueGame.stableId == game.stableId
                    val primaryLabel = channels[ch].actionLabel ?: if (isContinue) l10n("Continue", "Continua") else l10n("Play", "Gioca")
                    val position = l10n("${focused + 1} of ${games.size}", "${focused + 1} di ${games.size}")
                    val showPrimary = channels[ch].open != null || canLaunch
                    if (landscape) {
                        // On its side the details move to a panel beside the flow, which keeps
                        // the whole height for the covers.
                        Row(Modifier.weight(1f).fillMaxWidth()) {
                            Column(Modifier.weight(1f).fillMaxHeight()) {
                                flow(Modifier.weight(1f).fillMaxWidth(), REFLECTION_LANDSCAPE)
                                NxeDots(focused, games.size)
                            }
                            NxeSidePanel(
                                game = game,
                                position = position,
                                isContinue = isContinue,
                                primaryLabel = primaryLabel,
                                showPrimary = showPrimary,
                                usingPad = usingPad,
                                sortLabel = sortLabel,
                                onToggleSort = onToggleSort,
                                onPrimary = { activate() },
                                onOptions = { onGameOptions(game) },
                                modifier = Modifier.width(if (wide) 330.dp else 290.dp).fillMaxHeight(),
                            )
                        }
                    } else {
                        flow(Modifier.weight(1f).fillMaxWidth(), REFLECTION)
                        NxeDots(focused, games.size)
                        NxeInfo(
                            game = game,
                            position = position,
                            isContinue = isContinue,
                            primaryLabel = primaryLabel,
                            showPrimary = showPrimary,
                            wide = wide,
                            onPrimary = { activate() },
                            onOptions = { onGameOptions(game) },
                        )
                        NxeFooter(
                            usingPad = usingPad,
                            wide = wide,
                            primaryLabel = primaryLabel,
                            sortLabel = sortLabel,
                            onToggleSort = onToggleSort,
                        )
                    }
                }
            }
        }
    }
}

// ---- pieces --------------------------------------------------------------------------------

@Composable
private fun NxeChannelRow(
    channels: List<NxeChannel>,
    current: Int,
    wide: Boolean,
    compact: Boolean,
    onSelect: (Int) -> Unit,
) {
    val requesters = remember(channels.size) { List(channels.size) { BringIntoViewRequester() } }
    LaunchedEffect(current) { requesters.getOrNull(current)?.bringIntoView() }
    val scroll = rememberScrollState()
    Row(
        Modifier
            .fillMaxWidth()
            // Labels that run past the edge fade out there instead of being cut.
            .graphicsLayer { compositingStrategy = CompositingStrategy.Offscreen }
            .drawWithContent {
                drawContent()
                if (scroll.canScrollForward) {
                    drawRect(
                        Brush.horizontalGradient(0.8f to Color.White, 1f to Color.Transparent),
                        blendMode = BlendMode.DstIn,
                    )
                }
            }
            .horizontalScroll(scroll)
            .padding(horizontal = 24.dp, vertical = if (compact) 0.dp else 4.dp),
        horizontalArrangement = Arrangement.spacedBy(if (wide) 36.dp else 26.dp),
        verticalAlignment = Alignment.Bottom,
    ) {
        channels.forEachIndexed { i, c ->
            val on = i == current
            Text(
                c.label,
                style = TextStyle(fontFamily = XdFont, shadow = if (on) NxeTextShadow else null),
                fontSize = when { compact -> 22.sp; wide -> 30.sp; else -> 26.sp },
                fontWeight = if (on) FontWeight(350) else FontWeight.Light,
                color = if (on) NxeInk else NxeInk.copy(alpha = 0.45f),
                maxLines = 1,
                modifier = Modifier
                    .bringIntoViewRequester(requesters[i])
                    .drawBehind {
                        if (on) {
                            val bar = 3.dp.toPx()
                            drawRect(
                                NxeGreenBright,
                                topLeft = Offset(0f, size.height - bar),
                                size = Size(size.width * 0.6f, bar),
                            )
                        }
                    }
                    // Not a focus stop: the pad moves channels with L1/R1 from the dashboard.
                    .focusProperties { canFocus = false }
                    .clickable(interactionSource = null, indication = null, role = Role.Tab) { onSelect(i) }
                    .padding(bottom = if (compact) 5.dp else 8.dp),
            )
        }
    }
}

@Composable
private fun NxeTile(
    game: Game,
    viewModel: GameLibraryViewModel,
    slot: Int,
    offset: MutableFloatState,
    tile: Dp,
    reflection: Float,
    onClick: () -> Unit,
    onLongClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    // The icon embedded in the game builds the cover until (or instead of) the Marketplace art.
    val iconModel = remember(game.stableId) { viewModel.iconFileOrFallback(game) }
    val icon = rememberAsyncImagePainter(remember(iconModel) { ImageRequest.Builder(context).data(iconModel).build() })
    // Re-picked when the archive turns out not to have a picture (the PNG box art, the cover).
    val artVersion = GameArt.missingVersion.intValue
    val art = rememberAsyncImagePainter(remember(game.titleId, artVersion) { GameArt.boxArtRequest(context, game.titleId) })
    val focused = slot == 0
    val width = tile * GameArt.BOX_ASPECT
    // Not named density: the graphicsLayer block below reads its own density.
    val screenDensity = LocalDensity.current
    val measurer = rememberTextMeasurer()
    val coverText = remember(game.name, width, screenDensity) {
        measureCoverText(measurer, game.name, with(screenDensity) { width.toPx() }, screenDensity)
    }
    Box(
        modifier
            .size(width, tile)
            .graphicsLayer {
                val d = slot - offset.floatValue
                val a = abs(d)
                val s = knot(TILE_SCALE, a)
                translationX = sign(d) * knot(TILE_SPREAD, a, extend = true) * size.width
                // Card and reflection are centred together.
                translationY = -size.height * reflection * 0.5f
                scaleX = s
                scaleY = s
                rotationY = -d.coerceIn(-1f, 1f) * TILE_TURN_DEG
                cameraDistance = 10f * density
                alpha = knot(TILE_ALPHA, a)
                compositingStrategy = CompositingStrategy.ModulateAlpha
            }
            .focusProperties { canFocus = false }
            .combinedClickable(
                interactionSource = null,
                indication = null,
                onClickLabel = if (focused) l10n("Play", "Gioca") else l10n("Select", "Seleziona"),
                onLongClickLabel = if (focused) l10n("Game options", "Opzioni gioco") else null,
                onLongClick = onLongClick,
                onClick = onClick,
            )
            .semantics { contentDescription = game.name },
    ) {
        Box(
            Modifier
                .matchParentSize()
                .then(if (focused) Modifier.drawBehind { drawFocusGlow() } else Modifier)
                .clip(TileShape)
                .background(TileBaseBrush)
                // Every game gets a case: the Marketplace box art over a cover built from the
                // game's own icon and name, which shows while the art loads or when there is none.
                .drawBehind { drawCover(size, icon, art, coverText) }
                // Tiles further from the centre sink into the backdrop.
                .drawWithContent {
                    drawContent()
                    val dim = (abs(slot - offset.floatValue) * 0.22f).coerceAtMost(0.5f)
                    if (dim > 0f) drawRect(Color.Black.copy(alpha = dim))
                }
                .border(
                    if (focused) 3.dp else 1.dp,
                    if (focused) Color.White else Color.White.copy(alpha = 0.35f),
                    TileShape,
                ),
        ) {
            // Glass highlight across the top corner.
            Spacer(
                Modifier
                    .matchParentSize()
                    .drawWithCache {
                        val gloss = Brush.linearGradient(
                            0f to Color.White.copy(alpha = 0.30f), 0.38f to Color.White.copy(alpha = 0.06f),
                            0.4f to Color.Transparent,
                            start = Offset.Zero, end = Offset(size.width * 0.55f, size.height * 1.1f),
                        )
                        onDrawBehind { drawRect(gloss) }
                    },
            )
            if (game.isMultiDisc) {
                Text(
                    l10n("Disc ${game.discNumber}", "Disco ${game.discNumber}"),
                    fontSize = 11.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = Color.White,
                    modifier = Modifier
                        .align(Alignment.TopEnd)
                        .padding(8.dp)
                        .background(Color.Black.copy(alpha = 0.6f), RoundedCornerShape(50))
                        .padding(horizontal = 8.dp, vertical = 2.dp),
                )
            }
        }
        // Mirror of the bottom of the case on the floor, fading out.
        Spacer(
            Modifier
                .align(Alignment.TopCenter)
                .offset { IntOffset(0, (tile + REFLECTION_GAP_DP.dp).roundToPx()) }
                .size(width, tile * reflection)
                .graphicsLayer {
                    compositingStrategy = CompositingStrategy.Offscreen
                    clip = true
                }
                .drawWithCache {
                    val fade = Brush.verticalGradient(
                        0f to Color.White.copy(alpha = 0.30f), 1f to Color.Transparent,
                    )
                    val full = Size(size.width, size.width / GameArt.BOX_ASPECT)
                    onDrawBehind {
                        // Case row y lands at full.height - y: its bottom edge on top.
                        scale(scaleX = 1f, scaleY = -1f, pivot = Offset.Zero) {
                            translate(top = -full.height) { drawCover(full, icon, art, coverText) }
                        }
                        drawRect(fade, blendMode = BlendMode.DstIn)
                    }
                },
        )
    }
}

/** The two lines a built cover writes: the game's name and the case header. */
private class CoverText(val title: TextLayoutResult, val header: TextLayoutResult)

private fun measureCoverText(measurer: TextMeasurer, name: String, widthPx: Float, density: Density): CoverText {
    val titleSize = with(density) { (widthPx * 0.095f).toSp() }
    val title = measurer.measure(
        text = name,
        style = TextStyle(
            fontFamily = XdFont, fontSize = titleSize, lineHeight = titleSize * 1.12f,
            fontWeight = FontWeight.SemiBold, color = Color.White, textAlign = TextAlign.Center,
            shadow = Shadow(Color.Black.copy(alpha = 0.8f), Offset(0f, 2f), 8f),
        ),
        overflow = TextOverflow.Ellipsis,
        maxLines = 3,
        constraints = Constraints.fixedWidth((widthPx * 0.86f).roundToInt().coerceAtLeast(1)),
    )
    val header = measurer.measure(
        text = "XBOX 360",
        style = TextStyle(
            fontFamily = XdFont, fontSize = with(density) { (widthPx * 0.064f).toSp() },
            fontWeight = FontWeight.SemiBold, color = CoverHeaderInk,
        ),
        maxLines = 1,
    )
    return CoverText(title, header)
}

private val CoverHeader = Color(0xFFF2F2F2)
private val CoverHeaderInk = Color(0xFF5E5E5E)

/** A case at [full] size from the origin: the built cover, and the box art over it once loaded. */
private fun DrawScope.drawCover(full: Size, icon: Painter, art: Painter, text: CoverText) {
    drawBuiltCover(full, icon, text)
    with(art) { draw(full) }
}

/** A cover in the spirit of a 360 case for a game without Marketplace art: the light header
 *  with the green rule, the game's icon framed in the middle over its own colours, the name
 *  across the bottom. */
private fun DrawScope.drawBuiltCover(full: Size, icon: Painter, text: CoverText) {
    val w = full.width
    val h = full.height
    // The game's colours: its icon stretched over the whole case, then shaded.
    clipRect(0f, 0f, w, h) {
        translate(left = (w - h) / 2f) { with(icon) { draw(Size(h, h)) } }
    }
    drawRect(
        Brush.verticalGradient(
            0f to Color.Black.copy(alpha = 0.30f), 0.5f to Color.Black.copy(alpha = 0.50f),
            1f to Color.Black.copy(alpha = 0.88f), startY = 0f, endY = h,
        ),
        size = full,
    )
    val header = h * 0.09f
    drawRect(CoverHeader, size = Size(w, header))
    drawRect(NxeGreen, topLeft = Offset(0f, header), size = Size(w, h * 0.012f))
    drawText(text.header, topLeft = Offset(w * 0.07f, (header - text.header.size.height) / 2f))
    // The icon itself, framed, in the upper part of the case.
    val side = w * 0.58f
    val left = (w - side) / 2f
    val top = header + (h * 0.62f - header - side) / 2f
    val corner = CornerRadius(side * 0.12f)
    drawRoundRect(Color.Black.copy(alpha = 0.35f), topLeft = Offset(left, top + side * 0.04f),
        size = Size(side, side), cornerRadius = corner)
    clipPath(Path().apply { addRoundRect(RoundRect(left, top, left + side, top + side, corner)) }) {
        translate(left = left, top = top) { with(icon) { draw(Size(side, side)) } }
    }
    drawRoundRect(Color.White.copy(alpha = 0.55f), topLeft = Offset(left, top), size = Size(side, side),
        cornerRadius = corner, style = Stroke(w * 0.008f))
    drawText(text.title, topLeft = Offset((w - text.title.size.width) / 2f, h - text.title.size.height - h * 0.07f))
}

/** NXE selection: a soft green halo around the focused card and a thin green ring outside
 *  its white frame (Android's coloured elevation shadows barely show on a dark backdrop). */
private val GlowPaint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG)

private fun DrawScope.drawSoftGlow(radius: Float, alpha: Float, blur: Float) {
    GlowPaint.color = NxeGreenBright.copy(alpha = alpha).toArgb()
    GlowPaint.maskFilter = BlurMaskFilter(blur, BlurMaskFilter.Blur.NORMAL)
    drawIntoCanvas { it.nativeCanvas.drawRoundRect(0f, 0f, size.width, size.height, radius, radius, GlowPaint) }
}

private fun DrawScope.drawFocusGlow() {
    val radius = 6.dp.toPx()
    drawSoftGlow(radius, 0.9f, 26.dp.toPx())
    val ring = 2.dp.toPx()
    drawRoundRect(
        NxeGreenBright,
        topLeft = Offset(-ring, -ring),
        size = Size(size.width + 2 * ring, size.height + 2 * ring),
        cornerRadius = CornerRadius(radius + ring),
        style = Stroke(ring * 1.4f),
    )
}

/** Where the focused game sits in the library: a short pill among dots (at most 15 shown). */
@Composable
private fun NxeDots(index: Int, count: Int) {
    if (count < 2) return
    val shown = min(count, 15)
    val first = (index - shown / 2).coerceIn(0, count - shown)
    Row(
        Modifier.fillMaxWidth().padding(bottom = 10.dp),
        horizontalArrangement = Arrangement.Center,
    ) {
        for (i in first until first + shown) {
            val on = i == index
            Box(
                Modifier
                    .padding(horizontal = 3.dp)
                    .size(width = if (on) 18.dp else 6.dp, height = 6.dp)
                    .background(if (on) Color.White else Color.White.copy(alpha = 0.35f), RoundedCornerShape(50)),
            )
        }
    }
}

@Composable
private fun NxeInfo(
    game: Game,
    position: String,
    isContinue: Boolean,
    primaryLabel: String,
    showPrimary: Boolean,
    wide: Boolean,
    onPrimary: () -> Unit,
    onOptions: () -> Unit,
) {
    val details = nxeDetails(game, isContinue, position)
    val title: @Composable (TextAlign) -> Unit = { align ->
        Text(
            game.name,
            style = TextStyle(fontFamily = XdFont, shadow = NxeTextShadow),
            fontSize = if (wide) 30.sp else 28.sp,
            fontWeight = FontWeight(350),
            color = NxeInk,
            textAlign = align,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.fillMaxWidth(),
        )
        Text(
            details,
            style = TextStyle(fontFamily = XdFont, shadow = NxeTextShadow),
            fontSize = 13.sp,
            color = NxeInkSoft,
            textAlign = align,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.fillMaxWidth(),
        )
    }
    val actions: @Composable () -> Unit = {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            if (showPrimary) NxePrimaryButton(primaryLabel, onPrimary)
            NxeRoundButton(onOptions)
        }
    }
    if (wide) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 32.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f).padding(end = 16.dp)) { title(TextAlign.Start) }
            actions()
        }
    } else {
        Column(
            Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 6.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            title(TextAlign.Center)
            Spacer(Modifier.height(18.dp))
            actions()
        }
    }
}

/** The line under a game's name: container, disc, last launched, place in the library. */
private fun nxeDetails(game: Game, isContinue: Boolean, position: String): String = buildString {
    append(formatBadge(game.format))
    if (game.isMultiDisc) append(l10n("  •  Disc ${game.discNumber} of ${game.discCount}", "  •  Disco ${game.discNumber} di ${game.discCount}"))
    if (isContinue) append(l10n("  •  Last launched", "  •  Ultimo avviato"))
    append("  •  ").append(position)
}

/** The focused game's details on a panel beside the flow, for a screen on its side: name,
 *  detail line, the buttons, then the pad hints and the sort order. */
@Composable
private fun NxeSidePanel(
    game: Game,
    position: String,
    isContinue: Boolean,
    primaryLabel: String,
    showPrimary: Boolean,
    usingPad: Boolean,
    sortLabel: String,
    onToggleSort: () -> Unit,
    onPrimary: () -> Unit,
    onOptions: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(modifier.padding(start = 4.dp, end = 22.dp, bottom = 8.dp), verticalArrangement = Arrangement.Center) {
        Text(
            game.name,
            style = TextStyle(fontFamily = XdFont, shadow = NxeTextShadow),
            fontSize = 25.sp,
            lineHeight = 29.sp,
            fontWeight = FontWeight(350),
            color = NxeInk,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
        Text(
            nxeDetails(game, isContinue, position),
            style = TextStyle(fontFamily = XdFont, shadow = NxeTextShadow),
            fontSize = 13.sp,
            color = NxeInkSoft,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(top = 2.dp),
        )
        Spacer(Modifier.height(16.dp))
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            if (showPrimary) NxePrimaryButton(primaryLabel, onPrimary, width = 200.dp)
            NxeRoundButton(onOptions)
        }
        Spacer(Modifier.height(14.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (usingPad) {
                PadGlyph("A", NxeGreenBright)
                HintText(primaryLabel)
                PadGlyph("Y", NxeGlyphY)
                HintText(l10n("Options", "Opzioni"))
            }
            Spacer(Modifier.weight(1f))
            NxeSortChip(sortLabel, onToggleSort)
        }
    }
}

@Composable
private fun NxePrimaryButton(label: String, onClick: () -> Unit, width: Dp = 232.dp) {
    Row(
        Modifier
            .height(56.dp)
            .width(width)
            .drawBehind { drawSoftGlow(16.dp.toPx(), 0.45f, 18.dp.toPx()) }
            .clip(PrimaryShape)
            .background(PrimaryBrush)
            .drawWithContent {
                drawContent()
                drawRect(PrimarySheen)
            }
            .border(1.dp, Color.White.copy(alpha = 0.22f), PrimaryShape)
            .focusProperties { canFocus = false }
            .clickable(role = Role.Button, onClick = onClick)
            .padding(horizontal = 13.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        PadButtonA()
        Text(
            label,
            color = Color.White,
            fontFamily = XdFont,
            fontSize = 20.sp,
            fontWeight = FontWeight.SemiBold,
            textAlign = TextAlign.Center,
            maxLines = 1,
            modifier = Modifier.weight(1f),
        )
        // Balances the glyph so the label sits in the middle of the button.
        Spacer(Modifier.width(30.dp))
    }
}

/** The controller's A button as the 360 draws it: a dark green disc, white ring, white A. */
@Composable
private fun PadButtonA() {
    Box(
        Modifier
            .size(30.dp)
            .background(Brush.radialGradient(listOf(Color(0xFF1C8A1C), Color(0xFF0A4F0A))), CircleShape)
            .border(1.5.dp, Color.White.copy(alpha = 0.9f), CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Text("A", color = Color.White, fontFamily = XdFont, fontSize = 15.sp, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun NxeRoundButton(onClick: () -> Unit) {
    Box(
        Modifier
            .size(52.dp)
            .clip(CircleShape)
            .background(NxeGlass)
            .border(1.dp, NxeGlassEdge, CircleShape)
            .focusProperties { canFocus = false }
            .clickable(role = Role.Button, onClick = onClick)
            .semantics { contentDescription = l10n("Game options", "Opzioni gioco") },
        contentAlignment = Alignment.Center,
    ) {
        Icon(Icons.Filled.MoreVert, contentDescription = null, tint = Color.White)
    }
}

@Composable
private fun NxeFooter(
    usingPad: Boolean,
    wide: Boolean,
    primaryLabel: String,
    sortLabel: String,
    onToggleSort: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().padding(start = 24.dp, end = 16.dp, top = 4.dp, bottom = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Button hints only once a pad/keyboard is actually in use.
        if (usingPad) {
            PadGlyph("A", NxeGreenBright)
            HintText(primaryLabel)
            PadGlyph("Y", NxeGlyphY)
            HintText(l10n("Options", "Opzioni"))
            if (wide) {
                PadGlyph("LB", NxeGlassEdge)
                Spacer(Modifier.width(3.dp))
                PadGlyph("RB", NxeGlassEdge)
                HintText(l10n("Channels", "Canali"))
            }
        }
        Spacer(Modifier.weight(1f))
        NxeSortChip(sortLabel, onToggleSort)
    }
}

/** The library order as a glass chip; a tap switches it. */
@Composable
private fun NxeSortChip(label: String, onClick: () -> Unit) {
    Text(
        label,
        fontSize = 12.sp,
        fontFamily = XdFont,
        color = NxeInk,
        maxLines = 1,
        modifier = Modifier
            .clip(RoundedCornerShape(50))
            .background(NxeGlass)
            .border(1.dp, NxeGlassEdge, RoundedCornerShape(50))
            .focusProperties { canFocus = false }
            .clickable(role = Role.Button, onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 6.dp),
    )
}

@Composable
private fun PadGlyph(label: String, color: Color) {
    Box(
        Modifier
            .height(18.dp)
            .background(color, if (label.length == 1) CircleShape else RoundedCornerShape(50))
            .padding(horizontal = if (label.length == 1) 5.dp else 6.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, fontSize = 10.sp, fontFamily = XdFont, fontWeight = FontWeight.Bold, color = Color.White, maxLines = 1)
    }
}

@Composable
private fun HintText(text: String) {
    Text(
        text,
        fontSize = 12.sp,
        fontFamily = XdFont,
        color = NxeInkSoft,
        maxLines = 1,
        modifier = Modifier.padding(start = 5.dp, end = 12.dp),
    )
}

// ---- backdrop ------------------------------------------------------------------------------

/**
 * The focused game's theme ([src], cross-faded on change) over a dark green wash, under a
 * legibility scrim. Nothing here animates on its own: the dashboard costs no frames while it
 * sits still.
 */
@Composable
fun NxeBackground(src: NxeBackdrop?, modifier: Modifier = Modifier) {
    Box(modifier) {
        Spacer(
            Modifier.matchParentSize().drawWithCache {
                val wash = Brush.radialGradient(
                    0f to NxeBgCenter, 0.55f to NxeBgMid, 1f to NxeBgEdge,
                    center = Offset(size.width * 0.5f, size.height * 0.42f),
                    radius = hypot(size.width, size.height) * 0.62f,
                )
                onDrawBehind { drawRect(wash) }
            },
        )
        Crossfade(src, animationSpec = tween(500), label = "nxeGameTheme") { s ->
            if (s != null) NxeGameTheme(s)
        }
        Spacer(Modifier.matchParentSize().background(ScrimBrush))
    }
}

/**
 * A game's theme on a screen of any shape. Its 16:9 Marketplace background stays whole across
 * the top, fading into a blurred, slightly enlarged copy that fills the screen as colour and
 * light (cropping the picture to a portrait screen would keep an arbitrary middle slice).
 * More than half of the games have no such background: their box art takes its place, out of
 * focus across the top over its own light, and a game without box art lights the screen
 * with its embedded icon. Every game gets a themed screen, the same way.
 */
@Composable
private fun NxeGameTheme(src: NxeBackdrop) {
    val context = LocalContext.current
    // Re-picked when the archive turns out not to have a picture.
    val artVersion = GameArt.missingVersion.intValue
    // Blurred copies are decoded small: the blur throws the detail away anyway.
    val ambient = rememberAsyncImagePainter(remember(src.titleId, artVersion) {
        GameArt.backgroundRequest(context, src.titleId, small = true)
    })
    Box(Modifier.fillMaxSize()) {
        // The cover theme stays underneath, so a background arriving later fades in over it.
        val boxArt = rememberAsyncImagePainter(remember(src.titleId, artVersion) {
            GameArt.boxArtRequest(context, src.titleId)
        })
        if (boxArt.state is AsyncImagePainter.State.Success) {
            NxeAmbient(boxArt)
            NxeCoverHero(boxArt)
        } else {
            NxeAmbient(rememberAsyncImagePainter(src.icon))
        }
        NxeAmbient(ambient)
        val picture = remember(src.titleId, artVersion) { GameArt.backgroundRequest(context, src.titleId, small = false) }
        if (picture != null) {
            AsyncImage(
                model = picture,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier
                    .fillMaxWidth()
                    .aspectRatio(16f / 9f)
                    .graphicsLayer { compositingStrategy = CompositingStrategy.Offscreen }
                    .drawWithCache {
                        val fade = Brush.verticalGradient(0.55f to Color.White, 1f to Color.Transparent)
                        onDrawWithContent {
                            drawContent()
                            drawRect(fade, blendMode = BlendMode.DstIn)
                        }
                    },
            )
        }
    }
}

/** The box art itself, slightly out of focus, filling the width from the top and fading out
 *  below: the theme of a game the Marketplace gave no background. The white header of the
 *  case is cut off, it would light the top of the screen. */
@Composable
private fun NxeCoverHero(painter: Painter) {
    Spacer(
        Modifier
            .fillMaxSize()
            .blur(9.dp)
            .graphicsLayer { compositingStrategy = CompositingStrategy.Offscreen }
            .drawWithCache {
                val art = Size(size.width, size.width / GameArt.BOX_ASPECT)
                val top = -art.height * GameArt.CASE_HEADER
                val bottom = (top + art.height).coerceAtMost(size.height)
                val fade = Brush.verticalGradient(
                    0f to Color.White, 1f to Color.Transparent,
                    startY = bottom * 0.55f, endY = bottom,
                )
                onDrawBehind {
                    translate(top = top) { with(painter) { draw(art) } }
                    drawRect(fade, blendMode = BlendMode.DstIn)
                }
            },
    )
}

/** A picture as screen-filling colour and light: enlarged and blurred. */
@Composable
private fun NxeAmbient(painter: Painter) {
    Image(
        painter,
        contentDescription = null,
        contentScale = ContentScale.Crop,
        modifier = Modifier
            .fillMaxSize()
            .graphicsLayer { scaleX = 1.2f; scaleY = 1.2f }
            .blur(32.dp),
    )
}

/** Light status-bar icons over the dark backdrop; the previous look returns on dispose. */
@Composable
private fun NxeDarkStatusBar() {
    val view = LocalView.current
    if (view.isInEditMode) return
    DisposableEffect(view) {
        val window = view.context.findActivity()?.window
        if (window == null) {
            onDispose { }
        } else {
            val controller = WindowCompat.getInsetsController(window, view)
            val previous = controller.isAppearanceLightStatusBars
            controller.isAppearanceLightStatusBars = false
            onDispose { controller.isAppearanceLightStatusBars = previous }
        }
    }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}

// ---- icons for the view toggle (own vectors) -----------------------------------------------

internal object NxeIcons {
    /** Four squares: "switch to grid". */
    val Grid: ImageVector by lazy {
        ImageVector.Builder("NxeGrid", 24.dp, 24.dp, 24f, 24f)
            .path(fill = SolidColor(Color.Black)) {
                box(4f, 4f, 7f, 7f); box(13f, 4f, 7f, 7f)
                box(4f, 13f, 7f, 7f); box(13f, 13f, 7f, 7f)
            }
            .build()
    }

    /** A tall tile between two smaller ones: "switch to dashboard". */
    val Dashboard: ImageVector by lazy {
        ImageVector.Builder("NxeDashboard", 24.dp, 24.dp, 24f, 24f)
            .path(fill = SolidColor(Color.Black)) {
                box(7.5f, 4f, 9f, 16f)
                box(1.5f, 7.5f, 4.5f, 9f)
                box(18f, 7.5f, 4.5f, 9f)
            }
            .build()
    }

    private fun PathBuilder.box(x: Float, y: Float, w: Float, h: Float) {
        moveTo(x, y)
        horizontalLineToRelative(w)
        verticalLineToRelative(h)
        horizontalLineToRelative(-w)
        close()
    }
}
