package xendroid.compose.gamepad

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.IntSize
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import xendroid.compose.gamepad.ControlId
import xendroid.compose.gamepad.Kc
import xendroid.compose.gamepad.OnScreenControl
import xendroid.compose.gamepad.controlCenterPx
import xendroid.compose.gamepad.controlRadiusPx
import xendroid.compose.gamepad.hitTest
import xendroid.compose.gamepad.inFloatingStickZone

class GamepadHitTestTest {

    // density=1 so dp == px; makes radii easy to reason about.
    private val density = Density(density = 1f, fontScale = 1f)
    private val size = IntSize(1000, 1000)

    @Test
    fun centerPx_isFractionTimesDimension() {
        val c = OnScreenControl.Button(ControlId.A, Kc.A, "A", 0.5f, 0.25f)
        val p = controlCenterPx(c, size)
        assertEquals(500f, p.x)
        assertEquals(250f, p.y)
    }

    @Test
    fun buttonRadius_isHalfBaseSizeTimesScale() {
        val c = OnScreenControl.Button(ControlId.A, Kc.A, "A", 0.5f, 0.5f, scale = 2f, baseSizeDp = 64f)
        // 64dp diameter / 2 = 32, * scale 2 = 64.
        assertEquals(64f, controlRadiusPx(c, density))
    }

    @Test
    fun stickAndDpad_getLargerGrabRadius() {
        val stick = OnScreenControl.AnalogStick(ControlId.LEFT_STICK, true, 0.5f, 0.5f, baseSizeDp = 100f)
        assertEquals(50f * 1.15f, controlRadiusPx(stick, density))
        val dpad = OnScreenControl.Dpad(ControlId.DPAD, 0.5f, 0.5f, baseSizeDp = 100f)
        assertEquals(50f * 1.15f, controlRadiusPx(dpad, density))
    }

    @Test
    fun hitTest_insideRadius_hits_outside_misses() {
        val a = OnScreenControl.Button(ControlId.A, Kc.A, "A", 0.5f, 0.5f, baseSizeDp = 100f) // r=50
        val list = listOf(a)
        // center is (500,500); r=50.
        assertEquals(a, hitTest(list, Offset(530f, 500f), size, density))    // 30px away -> hit
        assertNull(hitTest(list, Offset(600f, 500f), size, density))          // 100px away -> miss
    }

    @Test
    fun hitTest_invisibleControl_neverHits() {
        val a = OnScreenControl.Button(ControlId.A, Kc.A, "A", 0.5f, 0.5f, baseSizeDp = 100f, visible = false)
        assertNull(hitTest(listOf(a), Offset(500f, 500f), size, density))
    }

    @Test
    fun hitTest_sameCenter_keepsListOrder() {
        val a = OnScreenControl.Button(ControlId.A, Kc.A, "A", 0.5f, 0.5f, baseSizeDp = 200f)
        val b = OnScreenControl.Button(ControlId.B, Kc.B, "B", 0.5f, 0.5f, baseSizeDp = 200f)
        // Both cover (500,500) equally deep; first in list wins.
        assertEquals(a, hitTest(listOf(a, b), Offset(500f, 500f), size, density))
        assertEquals(b, hitTest(listOf(b, a), Offset(500f, 500f), size, density))
    }

    @Test
    fun hitTest_overlap_picksNearerControl() {
        val a = OnScreenControl.Button(ControlId.A, Kc.A, "A", 0.5f, 0.5f, baseSizeDp = 200f) // r=100
        val b = OnScreenControl.Button(ControlId.B, Kc.B, "B", 0.6f, 0.5f, baseSizeDp = 200f) // r=100
        // (580,500) is inside both, 80px from A and 20px from B: B, whatever the list order.
        assertEquals(b, hitTest(listOf(a, b), Offset(580f, 500f), size, density))
        assertEquals(b, hitTest(listOf(b, a), Offset(580f, 500f), size, density))
    }

    @Test
    fun hitTest_triggerBar_endsAreTouchable() {
        // r=32: the trigger bar is 4.2r x 1.625r = 134.4 x 52 px around (500,500).
        val lt = OnScreenControl.Button(ControlId.LT, Kc.TRIGGER_L, "LT", 0.5f, 0.5f)
        assertEquals(lt, hitTest(listOf(lt), Offset(560f, 500f), size, density))   // bar end
        assertNull(hitTest(listOf(lt), Offset(500f, 530f), size, density))        // below the bar
    }

    @Test
    fun floatingStickZone_isLowerLeft() {
        assertEquals(true, inFloatingStickZone(Offset(200f, 700f), size))
        assertEquals(false, inFloatingStickZone(Offset(700f, 700f), size))  // right side
        assertEquals(false, inFloatingStickZone(Offset(200f, 100f), size))  // top
    }
}
