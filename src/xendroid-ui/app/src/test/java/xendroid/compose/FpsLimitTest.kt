package xendroid.compose

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class FpsLimitTest {

    @Test
    fun gpuBusy_isTheGpuShareOfEachFrame() {
        assertEquals(0.8, gpuBusyOf(40.0, 20.0)!!, 1e-9)
        assertEquals(1.0, gpuBusyOf(60.0, 30.0)!!, 1e-9)      // clamped
        assertNull(gpuBusyOf(40.0, null))
        assertNull(gpuBusyOf(0.0, 20.0))
    }

    @Test
    fun limit_gpuWhenBusy_capOnAPaceStep_cpuOtherwise() {
        assertEquals(FpsLimit.GPU, fpsLimitOf(27.0, 0.95))
        assertEquals(FpsLimit.GPU, fpsLimitOf(30.0, 0.93))    // a full GPU wins over the step
        assertEquals(FpsLimit.CAP, fpsLimitOf(60.0, 0.6))
        assertEquals(FpsLimit.CAP, fpsLimitOf(29.8, 0.5))
        assertEquals(FpsLimit.CPU, fpsLimitOf(42.0, 0.6))
        assertEquals(FpsLimit.GPU, fpsLimitOf(42.0, 0.87))
        assertNull(fpsLimitOf(42.0, null))
    }
}
