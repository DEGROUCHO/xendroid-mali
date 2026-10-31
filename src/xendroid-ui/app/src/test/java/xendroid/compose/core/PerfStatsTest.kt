package xendroid.compose.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PerfStatsTest {

    @Test
    fun procStat_sumsUtimeAndStime_evenWithAnOddProcessName() {
        val line = "4242 (emu) main (x) S 1 4242 0 0 -1 4194560 100 0 0 0 1500 250 0 0 20 0 30 0 99 1 2"
        assertEquals(1750L, parseProcStatCpuTicks(line))
        assertNull(parseProcStatCpuTicks("garbage"))
    }

    @Test
    fun vmRss_isReadInKb() {
        val status = "Name:\temu\nVmPeak:\t 9000000 kB\nVmRSS:\t 1843200 kB\nThreads:\t80\n"
        assertEquals(1843200L, parseVmRssKb(status))
        assertNull(parseVmRssKb("Name:\temu\n"))
    }

    @Test
    fun cpuPercent_isShareOfAllCores() {
        // 400 ticks at 100 Hz = 4 core-seconds in 1 s on 8 cores = 50%.
        assertEquals(50f, cpuPercent(400, 100, 1000, 8)!!, 0.01f)
        assertEquals(100f, cpuPercent(5000, 100, 1000, 8)!!, 0.01f)   // clamped
        assertNull(cpuPercent(10, 100, 0, 8))
        assertNull(cpuPercent(-1, 100, 1000, 8))
    }
}
