package xendroid.compose.core

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import java.io.File

/** utime + stime (clock ticks) from a /proc/<pid>/stat line; null if it does not parse.
 *  The process name may contain spaces and parentheses, so fields count from the LAST ')'. */
fun parseProcStatCpuTicks(line: String): Long? {
    val rest = line.substringAfterLast(')', "").trim().split(' ')
    // rest[0] is field 3 (state); utime and stime are fields 14 and 15.
    val utime = rest.getOrNull(11)?.toLongOrNull() ?: return null
    val stime = rest.getOrNull(12)?.toLongOrNull() ?: return null
    return utime + stime
}

/** Resident memory in kB from /proc/<pid>/status text; null if absent. */
fun parseVmRssKb(status: String): Long? =
    status.lineSequence().firstOrNull { it.startsWith("VmRSS:") }
        ?.substringAfter(':')?.trim()?.substringBefore(' ')?.toLongOrNull()

/** Share of the whole CPU (all cores = 100) used between two samples; null if not computable. */
fun cpuPercent(ticksDelta: Long, ticksPerSecond: Long, elapsedMs: Long, cores: Int): Float? {
    if (ticksDelta < 0 || ticksPerSecond <= 0 || elapsedMs <= 0 || cores <= 0) return null
    val coreSeconds = ticksDelta.toFloat() / ticksPerSecond
    return (coreSeconds / (elapsedMs / 1000f) / cores * 100f).coerceIn(0f, 100f)
}

/**
 * Cheap process/device readings for the in-game stats readout: a few small procfs reads and
 * one sticky broadcast per sample. Sample about once a second, off the main thread.
 */
class PerfStatsSampler(private val appContext: Context) {
    data class Sample(
        val cpuPercent: Float?,      // this process, share of all cores
        val ramMb: Int?,             // this process, resident
        val gpuPercent: Int?,        // null where the device exposes no readable GPU load
        val batteryTempC: Float?,
        val gpuFrameMs: Double? = null, // native Vulkan execution average per guest frame
    )

    private val ticksPerSecond = runCatching { Os.sysconf(OsConstants._SC_CLK_TCK) }.getOrDefault(100L)
    private val cores = Runtime.getRuntime().availableProcessors()
    private var lastTicks = -1L
    private var lastAtMs = 0L
    private val gpuLoad = GpuLoadReader()
    private var nativeGpuTimingSupported = true

    fun sample(): Sample {
        val now = SystemClock.elapsedRealtime()
        val ticks = runCatching { parseProcStatCpuTicks(File("/proc/self/stat").readText()) }.getOrNull()
        val cpu = if (ticks != null && lastTicks >= 0) {
            cpuPercent(ticks - lastTicks, ticksPerSecond, now - lastAtMs, cores)
        } else null
        if (ticks != null) { lastTicks = ticks; lastAtMs = now }
        val ram = runCatching { parseVmRssKb(File("/proc/self/status").readText()) }.getOrNull()
        val gpu = gpuLoad.samplePercent()
        val temp = runCatching {
            appContext.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
                ?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
                ?.takeIf { it != Int.MIN_VALUE }?.let { it / 10f }
        }.getOrNull()
        val gpuMs = (if (nativeGpuTimingSupported) {
            try { EmulatorRuntime.emulator?.gpu_frame_time_ms() }
            catch (_: LinkageError) { nativeGpuTimingSupported = false; null }
            catch (_: RuntimeException) { null }
        } else null)?.takeIf { it.isFinite() && it >= 0 }
        return Sample(cpu, ram?.let { (it / 1024).toInt() }, gpu, temp, gpuMs)
    }
}
