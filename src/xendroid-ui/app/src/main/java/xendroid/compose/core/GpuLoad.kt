package xendroid.compose.core

import java.io.File
import kotlin.math.roundToInt

/** KGSL's percentage node is already normalized; reject other schemas and invalid values. */
fun parseGpuPercent(text: String): Int? =
    Regex("\\s*(\\d{1,3})\\s*%?\\s*").matchEntire(text)
        ?.groupValues?.get(1)?.toIntOrNull()?.takeIf { it in 0..100 }

/** KGSL gpubusy reports busy_old and total_old for the same kernel sample interval. */
fun parseGpuBusyCounters(text: String): Int? {
    val parts = text.trim().split(Regex("\\s+"))
    if (parts.size != 2) return null
    val busy = parts[0].toLongOrNull() ?: return null
    val total = parts[1].toLongOrNull() ?: return null
    if (busy < 0 || total <= 0 || busy > total) return null
    return (busy.toDouble() / total.toDouble() * 100).roundToInt()
}

/** Device-wide GPU activity, not this app's share. Same app UID: no permission bypass. */
class GpuLoadReader(
    private val read: (String) -> String? = { path ->
        runCatching { File(path).readText() }.getOrNull()
    },
) {
    fun samplePercent(): Int? {
        read("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage")?.let {
            parseGpuPercent(it)?.let { value -> return value }
        }
        return read("/sys/class/kgsl/kgsl-3d0/gpubusy")?.let(::parseGpuBusyCounters)
    }
}
