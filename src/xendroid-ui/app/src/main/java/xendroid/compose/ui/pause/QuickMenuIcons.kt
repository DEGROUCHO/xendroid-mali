package xendroid.compose.ui.pause

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathBuilder
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.unit.dp

/** The few glyphs the core Material icon set lacks, drawn on the usual 24x24 grid. */
object QmIcons {
    private fun icon(name: String, block: PathBuilder.() -> Unit): ImageVector =
        ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f).apply {
            path(fill = SolidColor(Color.Black), pathFillType = PathFillType.EvenOdd, pathBuilder = block)
        }.build()

    private fun PathBuilder.dot(cx: Float, cy: Float, r: Float) {
        moveTo(cx - r, cy)
        arcToRelative(r, r, 0f, true, false, 2 * r, 0f)
        arcToRelative(r, r, 0f, true, false, -2 * r, 0f)
        close()
    }

    val Gamepad: ImageVector by lazy {
        icon("Gamepad") {
            moveTo(6f, 7f); horizontalLineToRelative(12f)
            arcToRelative(5f, 5f, 0f, false, true, 0f, 10f)
            horizontalLineToRelative(-12f)
            arcToRelative(5f, 5f, 0f, false, true, 0f, -10f); close()
            // D-pad cross and two buttons, cut out.
            moveTo(7.25f, 9.5f); horizontalLineToRelative(1.5f); verticalLineToRelative(1.75f)
            horizontalLineToRelative(1.75f); verticalLineToRelative(1.5f); horizontalLineToRelative(-1.75f)
            verticalLineToRelative(1.75f); horizontalLineToRelative(-1.5f); verticalLineToRelative(-1.75f)
            horizontalLineToRelative(-1.75f); verticalLineToRelative(-1.5f); horizontalLineToRelative(1.75f); close()
            dot(15f, 13.6f, 1.1f)
            dot(17.6f, 10.6f, 1.1f)
        }
    }

    val Camera: ImageVector by lazy {
        icon("Camera") {
            moveTo(4f, 7f); horizontalLineToRelative(3.2f); lineToRelative(1.3f, -2f)
            horizontalLineToRelative(7f); lineToRelative(1.3f, 2f); horizontalLineTo(20f)
            arcToRelative(2f, 2f, 0f, false, true, 2f, 2f); verticalLineToRelative(9f)
            arcToRelative(2f, 2f, 0f, false, true, -2f, 2f); horizontalLineTo(4f)
            arcToRelative(2f, 2f, 0f, false, true, -2f, -2f); verticalLineTo(9f)
            arcToRelative(2f, 2f, 0f, false, true, 2f, -2f); close()
            dot(12f, 13.5f, 3.5f)
        }
    }

    val Chart: ImageVector by lazy {
        icon("Chart") {
            moveTo(4f, 13f); horizontalLineToRelative(3.5f); verticalLineToRelative(7f); horizontalLineTo(4f); close()
            moveTo(10.25f, 4f); horizontalLineToRelative(3.5f); verticalLineToRelative(16f); horizontalLineToRelative(-3.5f); close()
            moveTo(16.5f, 9f); horizontalLineTo(20f); verticalLineToRelative(11f); horizontalLineToRelative(-3.5f); close()
        }
    }

    val Display: ImageVector by lazy {
        icon("Display") {
            moveTo(4f, 4f); horizontalLineTo(20f)
            arcToRelative(2f, 2f, 0f, false, true, 2f, 2f); verticalLineToRelative(10f)
            arcToRelative(2f, 2f, 0f, false, true, -2f, 2f); horizontalLineTo(4f)
            arcToRelative(2f, 2f, 0f, false, true, -2f, -2f); verticalLineTo(6f)
            arcToRelative(2f, 2f, 0f, false, true, 2f, -2f); close()
            moveTo(4f, 6f); horizontalLineTo(20f); verticalLineToRelative(10f); horizontalLineTo(4f); close()
            moveTo(8f, 19.5f); horizontalLineToRelative(8f); verticalLineToRelative(1.5f); horizontalLineToRelative(-8f); close()
        }
    }

    private fun PathBuilder.ring(cx: Float, cy: Float, outer: Float, inner: Float) {
        dot(cx, cy, outer); dot(cx, cy, inner)        // even-odd: the inner circle is a hole
    }

    val Stick: ImageVector by lazy { icon("Stick") { ring(12f, 12f, 9f, 7.4f); dot(12f, 12f, 4f) } }

    val Target: ImageVector by lazy {
        icon("Target") {
            ring(12f, 12f, 8f, 6.4f); dot(12f, 12f, 1.6f)
            moveTo(11.2f, 2f); horizontalLineToRelative(1.6f); verticalLineToRelative(4f); horizontalLineToRelative(-1.6f); close()
            moveTo(11.2f, 18f); horizontalLineToRelative(1.6f); verticalLineToRelative(4f); horizontalLineToRelative(-1.6f); close()
            moveTo(2f, 11.2f); horizontalLineToRelative(4f); verticalLineToRelative(1.6f); horizontalLineToRelative(-4f); close()
            moveTo(18f, 11.2f); horizontalLineToRelative(4f); verticalLineToRelative(1.6f); horizontalLineToRelative(-4f); close()
        }
    }

    val Sun: ImageVector by lazy {
        icon("Sun") {
            dot(12f, 12f, 4.2f)
            moveTo(11.2f, 2f); horizontalLineToRelative(1.6f); verticalLineToRelative(3f); horizontalLineToRelative(-1.6f); close()
            moveTo(11.2f, 19f); horizontalLineToRelative(1.6f); verticalLineToRelative(3f); horizontalLineToRelative(-1.6f); close()
            moveTo(2f, 11.2f); horizontalLineToRelative(3f); verticalLineToRelative(1.6f); horizontalLineToRelative(-3f); close()
            moveTo(19f, 11.2f); horizontalLineToRelative(3f); verticalLineToRelative(1.6f); horizontalLineToRelative(-3f); close()
            dot(5.4f, 5.4f, 1.1f); dot(18.6f, 5.4f, 1.1f); dot(5.4f, 18.6f, 1.1f); dot(18.6f, 18.6f, 1.1f)
        }
    }

    val Timer: ImageVector by lazy {
        icon("Timer") {
            ring(12f, 13f, 8f, 6.4f)
            moveTo(11.2f, 8.5f); horizontalLineToRelative(1.6f); verticalLineToRelative(5.3f); horizontalLineToRelative(-1.6f); close()
            moveTo(9.5f, 2f); horizontalLineToRelative(5f); verticalLineToRelative(1.6f); horizontalLineToRelative(-5f); close()
        }
    }

    val Vibration: ImageVector by lazy {
        icon("Vibration") {
            moveTo(8.5f, 4f); horizontalLineToRelative(7f)
            arcToRelative(1.5f, 1.5f, 0f, false, true, 1.5f, 1.5f); verticalLineToRelative(13f)
            arcToRelative(1.5f, 1.5f, 0f, false, true, -1.5f, 1.5f); horizontalLineToRelative(-7f)
            arcToRelative(1.5f, 1.5f, 0f, false, true, -1.5f, -1.5f); verticalLineToRelative(-13f)
            arcToRelative(1.5f, 1.5f, 0f, false, true, 1.5f, -1.5f); close()
            moveTo(3f, 9f); horizontalLineToRelative(1.6f); verticalLineToRelative(6f); horizontalLineToRelative(-1.6f); close()
            moveTo(19.4f, 9f); horizontalLineToRelative(1.6f); verticalLineToRelative(6f); horizontalLineToRelative(-1.6f); close()
        }
    }

    /** Opacity: a disc, half solid and half outline. */
    val Opacity: ImageVector by lazy {
        icon("Opacity") {
            ring(12f, 12f, 9f, 7.4f)
            moveTo(12f, 4.6f); arcToRelative(7.4f, 7.4f, 0f, false, false, 0f, 14.8f); close()
        }
    }

    val Thermo: ImageVector by lazy {
        icon("Thermo") {
            moveTo(10f, 4f); arcToRelative(2f, 2f, 0f, false, true, 4f, 0f); verticalLineToRelative(9.2f)
            arcToRelative(4f, 4f, 0f, true, true, -4f, 0f); close()
        }
    }
}
