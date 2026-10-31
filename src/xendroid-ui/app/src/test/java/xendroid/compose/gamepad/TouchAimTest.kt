package xendroid.compose.gamepad

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.IntSize
import kotlin.math.hypot
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchAimTest {

    private val size = IntSize(1000, 1000)

    @Test
    fun stickResponse_restingFinger_isNeutral() {
        assertEquals(0f to 0f, stickResponse(0f, 0f, 0.12f))
        assertEquals(0f to 0f, stickResponse(0.03f, -0.02f, 0.12f))
    }

    @Test
    fun stickResponse_justPastSlop_startsAtTheBoost() {
        val (x, y) = stickResponse(STICK_TOUCH_SLOP + 0.001f, 0f, 0.12f)
        assertEquals(0.12f, x, 0.01f)
        assertEquals(0f, y, 0f)
    }

    @Test
    fun stickResponse_onTheRing_isFullTilt_andNeverMore() {
        assertEquals(1f, stickResponse(1f, 0f, 0.12f).first, 1e-4f)
        val (x, y) = stickResponse(3f, 4f, 0.12f)          // far outside the ring
        assertEquals(1f, hypot(x, y), 1e-4f)
        assertEquals(0.6f, x, 1e-4f)                        // direction kept
        assertEquals(0.8f, y, 1e-4f)
    }

    @Test
    fun stickResponse_growsWithTravel() {
        var last = 0f
        for (i in 6..100) {
            val v = stickResponse(i / 100f, 0f, 0.12f).first
            assertTrue("not monotonic at $i", v >= last)
            last = v
        }
    }

    @Test
    fun stickResponse_noBoost_isPlainTravelPastSlop() {
        assertEquals(0f, stickResponse(STICK_TOUCH_SLOP, 0f, 0f).first, 1e-4f)
        assertEquals(1f, stickResponse(1f, 0f, 0f).first, 1e-4f)
    }

    @Test
    fun aimDrag_insideDeadRadius_doesNotMoveTheCamera() {
        assertEquals(Offset.Zero, aimDragDeflection(Offset(500f, 500f), Offset(510f, 500f), 14f, 50f))
    }

    @Test
    fun aimDrag_pastDeadRadius_scalesByStickRadius() {
        // 14px dead + 25px travel on a 50px stick = half tilt to the right.
        val d = aimDragDeflection(Offset(500f, 500f), Offset(539f, 500f), 14f, 50f)
        assertEquals(0.5f, d.x, 1e-4f)
        assertEquals(0f, d.y, 1e-4f)
        // Straight up.
        val u = aimDragDeflection(Offset(500f, 500f), Offset(500f, 436f), 14f, 50f)
        assertEquals(0f, u.x, 1e-4f)
        assertEquals(-1f, u.y, 1e-4f)
    }

    @Test
    fun floatingLookZone_isRightSideBelowTheTopStrip() {
        assertEquals(true, inFloatingLookZone(Offset(800f, 600f), size))
        assertEquals(false, inFloatingLookZone(Offset(300f, 600f), size))   // left side
        assertEquals(false, inFloatingLookZone(Offset(800f, 100f), size))   // top strip
        // The two floating zones never overlap.
        assertEquals(false, inFloatingStickZone(Offset(800f, 600f), size))
    }

    @Test
    fun hitTest_hiddenControl_onlyHitsInTheEditor() {
        val density = Density(density = 1f, fontScale = 1f)
        val a = OnScreenControl.Button(ControlId.A, Kc.A, "A", 0.5f, 0.5f, baseSizeDp = 100f, visible = false)
        assertNull(hitTest(listOf(a), Offset(500f, 500f), size, density))
        assertEquals(a, hitTest(listOf(a), Offset(500f, 500f), size, density, includeHidden = true))
    }
}

class StickSensitivityTest {
    @org.junit.Test fun lowerSensitivity_capsTheTopSpeed() {
        org.junit.Assert.assertEquals(0.5f, stickResponse(1f, 0f, 0.12f, sensitivity = 0.5f).first, 1e-4f)
    }

    @org.junit.Test fun higherSensitivity_reachesFullTiltEarlier_andNeverExceedsIt() {
        val half = stickResponse(0.5f, 0f, 0f, sensitivity = 2f).first
        org.junit.Assert.assertTrue(half > 0.9f)
        org.junit.Assert.assertEquals(1f, stickResponse(1f, 0f, 0f, sensitivity = 2f).first, 1e-4f)
    }

    @org.junit.Test fun preciseCurve_isSlowerNearTheCenter_sameAtTheEdge() {
        val linear = stickResponse(0.4f, 0f, 0f).first
        val precise = stickResponse(0.4f, 0f, 0f, curve = 1.6f).first
        org.junit.Assert.assertTrue(precise < linear)
        org.junit.Assert.assertEquals(1f, stickResponse(1f, 0f, 0f, curve = 1.6f).first, 1e-4f)
    }

    @org.junit.Test fun defaults_matchTheOldResponse() {
        org.junit.Assert.assertEquals(
            stickResponse(0.6f, -0.3f, 0.12f), stickResponse(0.6f, -0.3f, 0.12f, 1f, 1f))
    }
}
