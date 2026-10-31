package xendroid.compose.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.File
import java.nio.file.Files

class LiveCvarsTest {
    @Test fun renders_sorted_flat_toml() {
        assertEquals(
            "framerate_limit = 30\nguest_swap_pacing = true\nthermal_governor = false\n",
            LiveCvars.render(mapOf("thermal_governor" to false, "framerate_limit" to 30,
                "guest_swap_pacing" to true)))
    }

    @Test fun rejects_strings_and_bad_names() {
        assertThrows(IllegalArgumentException::class.java) { LiveCvars.render(mapOf("a" to "x")) }
        assertThrows(IllegalArgumentException::class.java) { LiveCvars.render(mapOf("a b" to 1)) }
    }

    @Test fun write_replaces_the_file() {
        val dir = Files.createTempDirectory("livecvars").toFile()
        try {
            LiveCvars.write(dir, mapOf("a" to 1))
            LiveCvars.write(dir, mapOf("a" to 2))
            assertEquals("a = 2\n", File(dir, LiveCvars.FILE_NAME).readText())
            assertEquals(listOf(LiveCvars.FILE_NAME), dir.list()!!.toList())
        } finally {
            dir.deleteRecursively()
        }
    }
}
