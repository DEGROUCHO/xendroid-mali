package xendroid.compose.ui.pause

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class QuickMenuModelTest {

    @Test
    fun wrap_cyclesBothWays() {
        assertEquals(1, qmWrap(0, 1, 4))
        assertEquals(0, qmWrap(3, 1, 4))
        assertEquals(3, qmWrap(0, -1, 4))
        assertEquals(0, qmWrap(2, 1, 0))
    }

    @Test
    fun sliderStep_movesOneStep_andStopsAtTheEnds() {
        assertEquals(0.70f, qmSliderStep(0.65f, 0.2f, 1f, 0.05f, 1), 1e-4f)
        assertEquals(0.60f, qmSliderStep(0.65f, 0.2f, 1f, 0.05f, -1), 1e-4f)
        assertEquals(1f, qmSliderStep(1f, 0.2f, 1f, 0.05f, 1), 1e-4f)
        assertEquals(0.2f, qmSliderStep(0.2f, 0.2f, 1f, 0.05f, -1), 1e-4f)
    }

    @Test
    fun sliderStep_snapsAnOffGridValue() {
        // 0.63 sits between steps: one press right lands on the next grid value, not 0.68.
        assertEquals(0.70f, qmSliderStep(0.63f, 0.2f, 1f, 0.05f, 1), 1e-4f)
    }

    @Test
    fun activate_runsTheRowsOwnAction() {
        var clicked = 0; var toggled: Boolean? = null; var next = 0
        qmActivate(QmAction("a", onClick = { clicked++ }))
        qmActivate(QmToggle("t", checked = true, onToggle = { toggled = it }))
        qmActivate(QmChoice("c", valueLabel = "x", onNext = { next++ }))
        assertEquals(1, clicked); assertEquals(false, toggled); assertEquals(1, next)
    }

    @Test
    fun adjust_onlySlidersUseLeftRight() {
        var v = 0.5f
        val slider = QmSlider("s", value = v, min = 0f, max = 1f, step = 0.1f, valueLabel = "", onChange = { v = it })
        assertTrue(qmAdjust(slider, 1))
        assertEquals(0.6f, v, 1e-4f)
        assertFalse(qmAdjust(QmAction("a", onClick = {}), 1))      // falls through to tab switch
    }
}

class QuickMenuSectionsTest {
    private val items = listOf<QmItem>(
        QmSection("A"), QmAction("one", onClick = {}), QmNote("n"),
        QmSection("B"), QmAction("two", onClick = {}),
    )

    @org.junit.Test fun highlight_skipsHeadersAndNotes_bothWays() {
        assertEquals(1, qmFirstSelectable(items))
        assertEquals(4, qmMoveSelection(items, 1, 1))
        assertEquals(1, qmMoveSelection(items, 4, 1))      // wraps past the header
        assertEquals(1, qmMoveSelection(items, 4, -1))
        assertEquals(4, qmMoveSelection(items, 1, -1))
    }

    @org.junit.Test fun nothingSelectable_staysPut() {
        assertEquals(0, qmMoveSelection(listOf(QmSection("x")), 0, 1))
    }

    @Test
    fun segmented_leftRightMovesWithinTheOptions_andActivateCycles() {
        var picked = -1
        val row = QmSegmented("fps", options = listOf("Auto", "30", "Free"), selected = 1) { picked = it }
        assertTrue(qmAdjust(row, 1)); assertEquals(2, picked)
        picked = -1
        assertTrue(qmAdjust(row.copy(selected = 2), 1)); assertEquals(-1, picked)   // already last
        qmActivate(row.copy(selected = 2)); assertEquals(0, picked)                  // wraps
    }
}
