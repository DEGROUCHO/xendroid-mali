package xendroid.compose.gamepad

import kotlinx.serialization.Serializable

@Serializable
data class ControlLayoutDto(
    val id: String,            // ControlId.name
    val x: Float, val y: Float,
    val scale: Float = 1f, val visible: Boolean = true,
)

@Serializable
data class OrientationLayoutDto(val controls: List<ControlLayoutDto> = emptyList())

@Serializable
data class GamepadGlobalsDto(
    val enabled: Boolean = true,
    val opacity: Float = 0.65f,            // ~65% default (spec)
    val autoHideSeconds: Float = 8f,       // 0 disables auto-hide
    val hapticsEnabled: Boolean = true,    // short click on every digital press
    // A touch on free space in the lower-left zone places the left stick there for that gesture.
    val floatingLeftStick: Boolean = true,
    // A finger sliding from one A/B/X/Y button onto another switches the press.
    val faceButtonSlide: Boolean = true,
    // A touch on free space in the right part of the screen places the right stick there.
    val floatingRightStick: Boolean = true,
    // Dragging the finger that holds RT drives the right stick (fire and aim with one thumb).
    val aimWithTrigger: Boolean = true,
    // Games ignore the first ~20% of stick travel (their own dead zone). The touch sticks
    // start their output at this level so the slightest finger movement already registers.
    val stickDeadzoneComp: Float = 0.12f,
    // LT is a latch: one tap holds it, the next releases it (aim down sights hands-free).
    val ltToggle: Boolean = false,
    // Stick sensitivity: scales the output, so 0.5 halves the top speed (finer aim) and 2.0
    // reaches full tilt at half the travel. Look = right stick and the RT aim drag.
    val lookSensitivity: Float = 1f,
    val moveSensitivity: Float = 1f,
    // Look response curve exponent: 1 = linear, above 1 = slower near the center.
    val lookCurve: Float = 1f,
)

/** The stick settings the overlay needs on every touch sample. */
data class StickTuning(
    val comp: Float = 0f,
    val moveSensitivity: Float = 1f,
    val lookSensitivity: Float = 1f,
    val lookCurve: Float = 1f,
)

fun GamepadGlobalsDto.stickTuning() =
    StickTuning(stickDeadzoneComp, moveSensitivity, lookSensitivity, lookCurve)

@Serializable
data class GamepadConfigDto(
    val version: Int = 1,
    val globals: GamepadGlobalsDto = GamepadGlobalsDto(),
    val portrait: OrientationLayoutDto = OrientationLayoutDto(),
    val landscape: OrientationLayoutDto = OrientationLayoutDto(),
)

fun OrientationLayoutDto.applyTo(base: List<OnScreenControl>): List<OnScreenControl> {
    val byId = controls.associateBy { it.id }
    return base.map { c ->
        byId[c.id.name]?.let { c.withLayout(it.x, it.y, it.scale.coerceIn(0.5f, 3f), it.visible) } ?: c
    }
}
fun List<OnScreenControl>.toDto() =
    OrientationLayoutDto(map { ControlLayoutDto(it.id.name, it.xFraction, it.yFraction, it.scale, it.visible) })
