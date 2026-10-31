package xendroid.compose

import android.content.Context
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.util.Locale
import kotlin.math.roundToInt
import xendroid.compose.core.EmulatorSession
import xendroid.compose.core.EmulatorRuntime
import androidx.compose.runtime.DisposableEffect
import xendroid.compose.core.PerfStatsSampler
import xendroid.compose.ui.theme.XdColors
import xendroid.compose.ui.theme.XdType

private val HUD_GOOD = XdColors.good
private val HUD_WARN = XdColors.caution
private val HUD_BAD = XdColors.bad
private const val GRAPH_SAMPLES = 60

/** Readout colour for a frame rate: steady, playable, struggling. */
fun fpsTier(fps: Double): Int = when {
    fps >= 50.0 -> 0
    fps >= 27.0 -> 1
    else -> 2
}

/** Readout colour for the battery temperature, by how close it is to throttling. */
fun tempTier(celsius: Float): Int = when {
    celsius < 40f -> 0
    celsius < 44f -> 1
    else -> 2
}

private fun tierColor(tier: Int) = when (tier) { 0 -> HUD_GOOD; 1 -> HUD_WARN; else -> HUD_BAD }

/**
 * Small, draggable performance readout drawn over the Vulkan SurfaceView. Shows a RenderDoc-
 * style ~1s average fps alongside the INSTANT frame time, polled from the native lock-free
 * frame-stats atomics at [pollHz]; optionally CPU / RAM / GPU / temperature and a frame-time
 * graph.
 *
 * The user can drag it anywhere; its position is persisted in a :emu-process SharedPreferences (so
 * the same DataStore the main process uses is never touched cross-process). Visibility is owned by
 * the caller via [visible] (the show_debug_overlay setting).
 */
@Composable
fun FpsOverlay(
    session: EmulatorSession,
    visible: Boolean,
    modifier: Modifier = Modifier,
    pollHz: Int = 4,
    // Extra lines: CPU, RAM, GPU (where readable) and battery temperature.
    detailed: Boolean = false,
    // A strip with the last GRAPH_SAMPLES frame times.
    graph: Boolean = false,
    // Changes when the saved position was rewritten from outside (menu: reset position).
    positionEpoch: Int = 0,
) {
    if (!visible) return

    DisposableEffect(detailed) {
        runCatching { EmulatorRuntime.emulator?.set_gpu_timing_enabled(detailed) }
        onDispose {
            runCatching { EmulatorRuntime.emulator?.set_gpu_timing_enabled(false) }
        }
    }

    val context = LocalContext.current
    val prefs = remember { context.getSharedPreferences("fps_overlay", Context.MODE_PRIVATE) }
    var offset by remember(positionEpoch) {
        mutableStateOf(Offset(prefs.getFloat("x", 0f), prefs.getFloat("y", 0f)))
    }
    var boxSize by remember { mutableStateOf(IntSize.Zero) }
    var fps by remember { mutableStateOf(0.0) }
    var frameMs by remember { mutableStateOf(0.0) }
    var shownFps by remember { mutableStateOf(0.0) }
    var stats by remember { mutableStateOf<PerfStatsSampler.Sample?>(null) }
    // Ring of recent frame times; `head` is a state so each new sample redraws the strip.
    val history = remember { FloatArray(GRAPH_SAMPLES) }
    var head by remember { mutableStateOf(0) }

    // ~4 Hz poll (250 ms) — enough for a human-readable readout while costing ~nothing.
    LaunchedEffect(pollHz) {
        val periodMs = 1000L / pollHz.coerceIn(1, 30)
        while (true) {
            fps = session.averageFps()
            frameMs = session.lastFrameTimeMs()
            shownFps = session.presentedFps()
            history[head % GRAPH_SAMPLES] = frameMs.toFloat()
            head++
            delay(periodMs)
        }
    }

    // ~1 Hz, off the main thread, and only while the detailed readout is on.
    LaunchedEffect(detailed) {
        if (!detailed) { stats = null; return@LaunchedEffect }
        val sampler = PerfStatsSampler(context.applicationContext)
        while (true) {
            stats = withContext(Dispatchers.IO) { sampler.sample() }
            delay(1000)
        }
    }

    val dim = Color.White.copy(alpha = 0.72f)
    Box(modifier.fillMaxSize().onSizeChanged { boxSize = it }) {
        Column(
            modifier = Modifier
                .offset { IntOffset(offset.x.roundToInt(), offset.y.roundToInt()) }
                .background(Color.Black.copy(alpha = 0.42f), RoundedCornerShape(7.dp))
                .padding(horizontal = 7.dp, vertical = 3.dp)
                .pointerInput(boxSize) {
                    detectDragGestures(
                        onDrag = { change, drag ->
                            change.consume()
                            // Clamp so the readout stays fully on-screen (size = this element's px).
                            val maxX = maxOf(0f, boxSize.width.toFloat() - size.width)
                            val maxY = maxOf(0f, boxSize.height.toFloat() - size.height)
                            offset = Offset(
                                (offset.x + drag.x).coerceIn(0f, maxX),
                                (offset.y + drag.y).coerceIn(0f, maxY),
                            )
                        },
                        onDragEnd = {
                            prefs.edit().putFloat("x", offset.x).putFloat("y", offset.y).apply()
                        },
                    )
                },
        ) {
            Row(verticalAlignment = Alignment.Bottom) {
                Text(
                    String.format(Locale.US, "%.0f", fps),
                    color = tierColor(fpsTier(fps)), style = XdType.hudLead,
                )
                Text(
                    // With frame generation: game fps -> fps on screen.
                    text = if (shownFps > fps + 1.0)
                        String.format(Locale.US, " FPS → %.0f FG  ·  %.1f ms", shownFps, frameMs)
                    else String.format(Locale.US, " FPS  ·  %.1f ms", frameMs),
                    color = dim, style = XdType.hud,
                )
            }
            stats?.let { s ->
                val line = perfStatsLine(s)
                if (line.isNotEmpty()) {
                    Text(line, color = dim, style = XdType.hud)
                }
                s.batteryTempC?.let { t ->
                    Text(
                        String.format(Locale.US, "%.1f °C", t),
                        color = tierColor(tempTier(t)), style = XdType.hud,
                    )
                }
            }
            if (graph) {
                val now = head                                   // state read: redraw per sample
                Canvas(Modifier.padding(top = 2.dp, bottom = 2.dp).size(104.dp, 24.dp)) {
                    // Scale: 0 .. 50 ms; guide lines at 16.7 ms (60 fps) and 33.3 ms (30 fps).
                    fun y(ms: Float) = size.height * (1f - (ms / 50f).coerceIn(0f, 1f))
                    val guide = Color.White.copy(alpha = 0.22f)
                    drawLine(guide, Offset(0f, y(16.7f)), Offset(size.width, y(16.7f)), 1f)
                    drawLine(guide, Offset(0f, y(33.3f)), Offset(size.width, y(33.3f)), 1f)
                    val count = minOf(now, GRAPH_SAMPLES)
                    if (count >= 2) {
                        val path = Path()
                        for (i in 0 until count) {
                            val v = history[(now - count + i) % GRAPH_SAMPLES]
                            val x = size.width * i / (GRAPH_SAMPLES - 1)
                            if (i == 0) path.moveTo(x, y(v)) else path.lineTo(x, y(v))
                        }
                        drawPath(path, HUD_GOOD.copy(alpha = 0.9f), style = Stroke(1.5.dp.toPx()))
                    }
                }
            }
        }
    }
}

/** Missing GPU data remains explicit; never substitute frame rate for GPU utilization. */
fun perfStatsLine(s: PerfStatsSampler.Sample): String = listOfNotNull(
    s.cpuPercent?.let { String.format(Locale.US, "CPU %.0f%%", it) },
    s.gpuPercent?.let { "GPU $it%" } ?: if (s.gpuFrameMs == null) "GPU —" else null,
    s.gpuFrameMs?.let { String.format(Locale.US, l10n("GPU time %.1f ms", "Tempo GPU %.1f ms"), it) },
    s.ramMb?.let { if (it >= 1024) String.format(Locale.US, "RAM %.2f GB", it / 1024f) else "RAM $it MB" },
).joinToString("  ·  ")
