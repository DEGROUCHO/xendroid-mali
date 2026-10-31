package xendroid.compose

import kotlin.math.sqrt

/** Maps PC input to the existing Xbox sticks/buttons; no platform or core changes. */
internal class PcInputMapper(
    private val button: (Int, Boolean) -> Unit,
    private val stick: (Boolean, Float, Float) -> Unit,
) {
    enum class Key { W, A, S, D, SPACE, R, F, E, SHIFT, CTRL, C, Q, G, V, ENTER, TAB, X }
    var enabled = false
        private set
    private var halo3 = false

    /** Halo uses different Xbox buttons for reload, crouch, equipment and zoom. */
    fun setTitleId(titleId: String?) {
        reset()
        halo3 = titleId.equals("4D5307E6", ignoreCase = true)
    }

    private val held = mutableSetOf<Key>()
    private val emitted = BooleanArray(16)
    private var fire = false
    private var aim = false
    private var dx = 0f
    private var dy = 0f

    fun setEnabled(value: Boolean) { reset(); enabled = value }

    fun key(key: Key, down: Boolean) {
        if (!enabled) return
        if (down) held.add(key) else held.remove(key)
        val x = (if (Key.D in held) 1f else 0f) - (if (Key.A in held) 1f else 0f)
        val y = (if (Key.S in held) 1f else 0f) - (if (Key.W in held) 1f else 0f)
        val length = maxOf(1f, sqrt(x*x+y*y))
        stick(false, x/length, y/length)
        buttons()
    }

    fun mouse(x: Float, y: Float, primary: Boolean, secondary: Boolean) {
        if (!enabled) return
        if (x.isFinite() && y.isFinite()) { dx += x; dy += y }
        fire = primary; aim = secondary
        buttons()
    }

    /** Consume deltas once. An idle tick returns the camera stick to neutral. */
    fun tick() {
        if (!enabled) return
        val x = (dx * 0.045f).coerceIn(-1f, 1f)
        val y = (dy * 0.045f).coerceIn(-1f, 1f)
        dx = 0f; dy = 0f
        val length = maxOf(1f, sqrt(x*x+y*y))
        stick(true, x/length, y/length)
    }

    private fun buttons() {
        for (code in 4..15) {
            val down = if (halo3) when (code) {
                4 -> Key.SPACE in held
                5 -> Key.V in held
                6 -> Key.X in held
                7 -> Key.E in held
                8 -> Key.TAB in held
                9 -> Key.ENTER in held
                10 -> Key.Q in held
                11 -> Key.R in held || Key.F in held
                12 -> Key.CTRL in held || Key.C in held
                13 -> aim
                14 -> Key.G in held
                15 -> fire
                else -> false
            } else when (code) {
                4 -> Key.SPACE in held
                5 -> Key.CTRL in held || Key.C in held
                6 -> Key.R in held || Key.F in held
                7 -> Key.E in held
                8 -> Key.TAB in held
                9 -> Key.ENTER in held
                10 -> Key.Q in held
                11 -> Key.G in held
                12 -> Key.SHIFT in held
                13 -> Key.V in held
                14 -> aim
                15 -> fire
                else -> false
            }
            if (emitted[code] != down) { emitted[code] = down; button(code, down) }
        }
    }

    /** Focus loss/mode switch must not retain a missing key-up or mouse-up. */
    fun reset() {
        held.clear(); dx = 0f; dy = 0f; fire = false; aim = false
        buttons()
        stick(false, 0f, 0f); stick(true, 0f, 0f)
    }
}
