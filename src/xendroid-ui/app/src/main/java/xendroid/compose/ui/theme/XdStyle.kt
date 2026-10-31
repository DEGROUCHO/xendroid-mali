package xendroid.compose.ui.theme

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import xendroid.compose.R

/**
 * Selawik (Microsoft, SIL OFL 1.1 - assets/licenses/selawik-ofl.txt): the open metric
 * stand-in for Segoe, the face of the Xbox 360 dashboard. Semilight sits at weight 350.
 */
val XdFont = FontFamily(
    Font(R.font.selawik_light, FontWeight.Light),
    Font(R.font.selawik_semilight, FontWeight(350)),
    Font(R.font.selawik_regular, FontWeight.Normal),
    Font(R.font.selawik_semibold, FontWeight.SemiBold),
    Font(R.font.selawik_bold, FontWeight.Bold),
)

/**
 * The one type scale of the in-game UI (menu, readout, editor): five roles, one family.
 * Numbers use tabular figures so values do not jitter or misalign as they change.
 */
object XdType {
    /** Panel title (the game name in the menu header). */
    val title = TextStyle(fontFamily = XdFont, fontSize = 16.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold)
    /** A row's name. */
    val row = TextStyle(fontFamily = XdFont, fontSize = 14.sp, lineHeight = 20.sp, fontWeight = FontWeight.Medium)
    /** A row's one-line explanation, hints. */
    val caption = TextStyle(fontFamily = XdFont, fontSize = 12.sp, lineHeight = 16.sp, fontWeight = FontWeight.Normal)
    /** Section headers and badges: small caps look. */
    val label = TextStyle(
        fontFamily = XdFont, fontSize = 11.sp, lineHeight = 14.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 1.sp)
    /** A setting's current value. */
    val value = TextStyle(
        fontFamily = XdFont, fontSize = 14.sp, lineHeight = 20.sp, fontWeight = FontWeight.SemiBold,
        fontFeatureSettings = "tnum")
    /** Performance readout lines. */
    val hud = TextStyle(
        fontSize = 11.sp, lineHeight = 15.sp, fontWeight = FontWeight.Medium,
        fontFeatureSettings = "tnum")
    val hudLead = TextStyle(
        fontSize = 16.sp, lineHeight = 18.sp, fontWeight = FontWeight.Bold,
        fontFeatureSettings = "tnum")
}

/**
 * Colours that carry a meaning, used the same way everywhere:
 * green = on / selected / go, amber = caution or "not yet applied", red = destructive or bad,
 * blue = information only. Everything else stays neutral.
 */
object XdColors {
    val good = Color(0xFF6FD75F)
    val caution = Color(0xFFE6B450)
    val bad = Color(0xFFFF7A6B)
    val info = Color(0xFF7FB6FF)
}
