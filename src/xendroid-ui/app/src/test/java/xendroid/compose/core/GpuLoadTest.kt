package xendroid.compose.core
import org.junit.Assert.*
import org.junit.Test

class GpuLoadTest {
    @Test fun percentagesMustBeExplicitAndInRange() {
        assertEquals(37,parseGpuPercent(" 37%\n"));assertEquals(0,parseGpuPercent("0"))
        for (bad in listOf("180", "-1", "37 100", "busy=37", "12.4", "")) assertNull(parseGpuPercent(bad))
    }
    @Test fun countersUseBusyOverTotalWithoutClampingOrOverflow() {
        assertEquals(25,parseGpuBusyCounters("250 1000\n"))
        assertEquals(50,parseGpuBusyCounters("4000000000000000000 8000000000000000000"))
        for (bad in listOf("0 0", "120 100", "-1 100", "100", "1 2 3")) assertNull(parseGpuBusyCounters(bad))
    }
    @Test fun unavailableOrMalformedPercentageFallsBackToCounters() {
        val reader=GpuLoadReader { path -> if(path.endsWith("gpubusy")) "33 100" else "garbage" }
        assertEquals(33,reader.samplePercent())
        assertNull(GpuLoadReader { null }.samplePercent())
    }
}
