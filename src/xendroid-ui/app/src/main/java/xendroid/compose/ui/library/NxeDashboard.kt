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

/** Title id of the game whose background the dashboard shows; set by [NxeDashboard]. */
private val LocalNxeBackdrop = staticCompositionLocalOf<MutableState<String?>?> { null }

/** Wraps the library screen while the dashboard is active: dark NXE colour scheme, the
 *  game-themed backdrop behind everything (top bar included) and light status-bar icons.
 *  With [enabled] false it is a pass-through, and the tree shape stays the same either way. */
@Composable
fun NxeSurface(enabled: Boolean, content: @Composable () -> Unit) {
    val backdrop = remember { mutableStateOf<String?>(null) }
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
    // Visual position = focused + offset (in tiles). Only graphicsLayer blocks read it.
    val offset = remember { mutableFloatStateOf(0f) }
    val motion = remember { arrayOfNulls<Job>(1) }
    val ch = channel.coerceIn(0, channels.lastIndex)

    fun currentIndex(): Int = indexById[focusedId] ?: 0

    /** Moves focus by [n] tiles keeping the visual position continuous; returns the move. */
    fun shift(n: Int): Int {
        val idx = currentIndex()
        val target = (idx + n).coerceIn(0, games.lastIndex)
        val moved = target - idx
        if (moved != 0) {
            focusedId = games[target].stableId
            offset.floatValue = (offset.floatValue - moved).coerceIn(-3f, 3f)
        }
        return moved
    }

    fun glide(velocity: Float, stiffness: Float) {
        motion[0]?.cancel()
        motion[0] = scope.launch {
            animate(offset.floatValue, 0f, velocity, spring(dampingRatio = 0.9f, stiffness = stiffness)) { v, _ ->
                offset.floatValue = v
            }
        }
    }

    fun step(n: Int) { if (shift(n) != 0) glide(0f, 320f) }

    fun activate() {
        val game = games.getOrNull(currentIndex()) ?: return
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
    // The backdrop follows the focused game once the stack settles, not every tile of a fling.
    val backdrop = LocalNxeBackdrop.current
    val focusedTitleId = games.getOrNull(focused)?.titleId
    LaunchedEffect(focusedTitleId) {
        delay(160)
        backdrop?.value = focusedTitleId
    }
    val flowAlpha by animateFloatAsState(if (ch == 0) 1f else 0.5f, label = "nxeFlowDim")

    BoxWithConstraints(modifier.fillMaxSize()) {
        val viewportH = if (constraints.hasBoundedHeight) maxHeight else 520.dp
        val wide = maxWidth >= 600.dp
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
                        onSelect = { i ->
                            val open = channels[i].open
                            if (open == null) channel = i
                            else { channel = 0; open(games.getOrNull(currentIndex())) }
                        },
                    )

                    BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
                        // Room for the card and its reflection: the pair is centred together.
                        val tile = minOf(maxHeight / (1f + REFLECTION) * 0.98f, maxWidth * 0.5f / GameArt.BOX_ASPECT, 320.dp)
                            .coerceAtLeast(80.dp)
                        val tilePx = with(LocalDensity.current) { tile.toPx() * GameArt.BOX_ASPECT }
                        val stepPx = tilePx * TILE_SPREAD[1]
                        val dragState = rememberDraggableState { delta ->
                            var off = offset.floatValue - delta / stepPx
                            var idx = currentIndex()
                            while (off > 0.5f && idx < games.lastIndex) { idx++; off -= 1f }
                            while (off < -0.5f && idx > 0) { idx--; off += 1f }
                            if (idx == 0) off = max(off, -0.35f)
                            if (idx == games.lastIndex) off = min(off, 0.35f)
                            val id = games[idx].stableId
                            if (id != focusedId) focusedId = id
                            offset.floatValue = off
                        }
                        Box(
                            Modifier
                                .fillMaxSize()
                                .graphicsLayer { alpha = flowAlpha }
                                .draggable(
                                    state = dragState,
                                    orientation = Orientation.Horizontal,
                                    onDragStarted = { motion[0]?.cancel() },
                                    onDragStopped = { velocity ->
                                        // Fling: project a little ahead, land on a whole tile.
                                        val v = -velocity / stepPx
                                        val idx = currentIndex()
                                        val n = (offset.floatValue + v * 0.22f).roundToInt()
                                            .coerceIn(-idx, games.lastIndex - idx)
                                            .coerceIn(-6, 6)
                                        shift(n)
                                        glide(v, 170f)
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
                                            modifier = Modifier.zIndex(-abs(slot).toFloat()),
                                            onClick = {
                                                when {
                                                    slot == 0 -> { channel = 0; onLaunch(game) }
                                                    abs(slot) <= TILE_TAPPABLE -> if (shift(slot) != 0) glide(0f, 220f)
                                                }
                                            },
                                            onLongClick = {
                                                when {
                                                    slot == 0 -> onGameOptions(game)
                                                    abs(slot) <= TILE_TAPPABLE -> if (shift(slot) != 0) glide(0f, 220f)
                                                }
                                            },
                                        )
                                    }
                                }
                            }
                        }
                    }

                    NxeDots(focused, games.size)
                    val game = games[focused.coerceIn(0, games.lastIndex)]
                    val isContinue = continueGame != null && continueGame.stableId == game.stableId
                    val primaryLabel = channels[ch].actionLabel ?: if (isContinue) l10n("Continue", "Continua") else l10n("Play", "Gioca")
                    NxeInfo(
                        game = game,
                        position = l10n("${focused + 1} of ${games.size}", "${focused + 1} di ${games.size}"),
                        isContinue = isContinue,
                        primaryLabel = primaryLabel,
                        showPrimary = channels[ch].open != null || canLaunch,
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

// ---- pieces --------------------------------------------------------------------------------

@Composable
private fun NxeChannelRow(
    channels: List<NxeChannel>,
    current: Int,
    wide: Boolean,
    onSelect: (Int) -> Unit,
) {
    val requesters = remember(channels.size) { List(channels.size) { BringIntoViewRequester() } }
    LaunchedEffect(current) { requesters.getOrNull(current)?.bringIntoView() }
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 24.dp, vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(if (wide) 36.dp else 26.dp),
        verticalAlignment = Alignment.Bottom,
    ) {
        channels.forEachIndexed { i, c ->
            val on = i == current
            Text(
                c.label,
                style = TextStyle(fontFamily = XdFont, shadow = if (on) NxeTextShadow else null),
                fontSize = if (wide) 30.sp else 27.sp,
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
                    .padding(bottom = 8.dp),
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
    onClick: () -> Unit,
    onLongClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    // The embedded 360 icon shows until (or instead of) the Marketplace box art.
    val iconModel = remember(game.stableId) { viewModel.iconFileOrFallback(game) }
    val iconRequest = remember(iconModel) { ImageRequest.Builder(context).data(iconModel).build() }
    val art = rememberAsyncImagePainter(GameArt.url(game.titleId, GameArt.Kind.BoxArt))
    val hasArt = art.state is AsyncImagePainter.State.Success
    val focused = slot == 0
    val width = tile * GameArt.BOX_ASPECT
    Box(
        modifier
            .size(width, tile)
            .graphicsLayer {
                val d = slot - offset.floatValue
                val a = abs(d)
                val s = knot(TILE_SCALE, a)
                translationX = sign(d) * knot(TILE_SPREAD, a, extend = true) * size.width
                // Card and reflection are centred together.
                translationY = -size.height * REFLECTION * 0.5f
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
            // Coil starts the request when the painter is first drawn, so it is always drawn.
            if (!hasArt) {
                AsyncImage(
                    model = iconRequest,
                    contentDescription = null,
                    contentScale = ContentScale.Fit,
                    modifier = Modifier
                        .align(Alignment.Center)
                        .fillMaxWidth(0.62f)
                        .clip(IconShape),
                )
                Text(
                    game.name,
                    style = TextStyle(fontFamily = XdFont, fontSize = 13.sp, fontWeight = FontWeight.SemiBold),
                    color = Color.White,
                    textAlign = TextAlign.Center,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.align(Alignment.BottomCenter).padding(10.dp),
                )
            }
            Image(art, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
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
        // Mirror of the bottom of the box art on the floor, fading out.
        if (hasArt) {
            Spacer(
                Modifier
                    .align(Alignment.TopCenter)
                    .offset { IntOffset(0, (tile + REFLECTION_GAP_DP.dp).roundToPx()) }
                    .size(width, tile * REFLECTION)
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
                            // Image row y lands at full.height - y: its bottom edge on top.
                            scale(scaleX = 1f, scaleY = -1f, pivot = Offset.Zero) {
                                translate(top = -full.height) { with(art) { draw(full) } }
                            }
                            drawRect(fade, blendMode = BlendMode.DstIn)
                        }
                    },
            )
        }
    }
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
    val details = buildString {
        append(formatBadge(game.format))
        if (game.isMultiDisc) append(l10n("  •  Disc ${game.discNumber} of ${game.discCount}", "  •  Disco ${game.discNumber} di ${game.discCount}"))
        if (isContinue) append(l10n("  •  Last launched", "  •  Ultimo avviato"))
        append("  •  ").append(position)
    }
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

@Composable
private fun NxePrimaryButton(label: String, onClick: () -> Unit) {
    Row(
        Modifier
            .height(56.dp)
            .width(232.dp)
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
        Text(
            sortLabel,
            fontSize = 12.sp,
            fontFamily = XdFont,
            color = NxeInk,
            maxLines = 1,
            modifier = Modifier
                .clip(RoundedCornerShape(50))
                .background(NxeGlass)
                .border(1.dp, NxeGlassEdge, RoundedCornerShape(50))
                .focusProperties { canFocus = false }
                .clickable(role = Role.Button, onClick = onToggleSort)
                .padding(horizontal = 12.dp, vertical = 6.dp),
        )
    }
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
 * The focused game's Marketplace background ([titleId], cross-faded on change) over a dark
 * green wash, under a legibility scrim. Nothing here animates on its own: the dashboard
 * costs no frames while it sits still.
 */
@Composable
fun NxeBackground(titleId: String?, modifier: Modifier = Modifier) {
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
        Crossfade(titleId, animationSpec = tween(500), label = "nxeGameTheme") { id ->
            val url = GameArt.url(id, GameArt.Kind.Background)
            if (url != null) NxeGameTheme(url)
        }
        Spacer(Modifier.matchParentSize().background(ScrimBrush))
    }
}

/**
 * A 16:9 Marketplace background on a screen of any shape: a blurred, slightly enlarged copy
 * fills the screen as colour and light, and the picture itself stays whole across the top,
 * fading into it. Cropping the 16:9 picture to a portrait screen would keep an arbitrary
 * middle slice.
 */
@Composable
private fun NxeGameTheme(url: String) {
    val context = LocalContext.current
    // The blurred copy is decoded small: the blur throws the detail away anyway.
    val ambient = remember(url) { ImageRequest.Builder(context).data(url).size(320, 180).build() }
    Box(Modifier.fillMaxSize()) {
        AsyncImage(
            model = ambient,
            contentDescription = null,
            contentScale = ContentScale.Crop,
            modifier = Modifier
                .fillMaxSize()
                .graphicsLayer { scaleX = 1.2f; scaleY = 1.2f }
                .blur(32.dp),
        )
        AsyncImage(
            model = url,
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
