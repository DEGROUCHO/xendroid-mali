package xendroid.compose.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// XenDroid brand palette: deep charcoal surfaces + Xbox green accents + gold highlights.
// Dark is the primary experience (gaming feel); light stays clean and readable.
private val DarkColors = darkColorScheme(
    primary = Color(0xFF6FD75F),
    onPrimary = Color(0xFF06320B),
    primaryContainer = Color(0xFF107C10),
    onPrimaryContainer = Color(0xFFD8FFD0),
    inversePrimary = Color(0xFF107C10),
    secondary = Color(0xFFB9CCB3),
    onSecondary = Color(0xFF1B2A1A),
    secondaryContainer = Color(0xFF243425),
    onSecondaryContainer = Color(0xFFD5E8CF),
    tertiary = Color(0xFFE6B450),
    onTertiary = Color(0xFF3D2E00),
    tertiaryContainer = Color(0xFF5A4400),
    onTertiaryContainer = Color(0xFFFFDF9E),
    background = Color(0xFF0E1114),
    onBackground = Color(0xFFE4E9E5),
    surface = Color(0xFF14181D),
    onSurface = Color(0xFFE4E9E5),
    surfaceVariant = Color(0xFF1D232A),
    onSurfaceVariant = Color(0xFFB7C0BB),
    surfaceContainerLowest = Color(0xFF0A0D10),
    surfaceContainerLow = Color(0xFF11151A),
    surfaceContainer = Color(0xFF161B21),
    surfaceContainerHigh = Color(0xFF1C222A),
    surfaceContainerHighest = Color(0xFF232A33),
    outline = Color(0xFF3A434D),
    outlineVariant = Color(0xFF2A323A),
    inverseSurface = Color(0xFFE4E9E5),
    inverseOnSurface = Color(0xFF2A2F2C),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
    errorContainer = Color(0xFF93000A),
    onErrorContainer = Color(0xFFFFDAD6),
)
private val LightColors = lightColorScheme(
    primary = Color(0xFF107C10),
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFC0F0B4),
    onPrimaryContainer = Color(0xFF002201),
    inversePrimary = Color(0xFF6FD75F),
    secondary = Color(0xFF52634F),
    onSecondary = Color(0xFFFFFFFF),
    secondaryContainer = Color(0xFFD5E8CF),
    onSecondaryContainer = Color(0xFF101F0F),
    tertiary = Color(0xFF7A5900),
    onTertiary = Color(0xFFFFFFFF),
    tertiaryContainer = Color(0xFFFFDF9E),
    onTertiaryContainer = Color(0xFF261A00),
    background = Color(0xFFF7FAF6),
    onBackground = Color(0xFF191C19),
    surface = Color(0xFFF7FAF6),
    onSurface = Color(0xFF191C19),
    surfaceVariant = Color(0xFFDDE5DB),
    onSurfaceVariant = Color(0xFF414941),
    outline = Color(0xFF717971),
    outlineVariant = Color(0xFFC1C9BF),
)

/** Material 3 theme for the Compose frontend, themed to Xbox green. */
@Composable
fun xendroidTheme(
    darkTheme: Boolean = true,
    content: @Composable () -> Unit,
) {
    MaterialTheme(
        colorScheme = if (darkTheme) DarkColors else LightColors,
        content = content,
    )
}
