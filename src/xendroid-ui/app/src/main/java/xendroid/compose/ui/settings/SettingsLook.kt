package xendroid.compose.ui.settings

import xendroid.compose.l10n

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.addPathNodes
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import xendroid.compose.ui.theme.XdColors
import xendroid.compose.ui.theme.XdFont

/*
 * The settings screens in the dashboard's NXE look: a light header over the dark green wash,
 * a glass search field and the sections as glass tiles with their own icon.
 */

internal val GlassFill = Color.White.copy(alpha = 0.10f)
internal val GlassEdge = Color.White.copy(alpha = 0.18f)
private val TileShape = RoundedCornerShape(14.dp)
private val TileSheen = Brush.linearGradient(
    0f to Color.White.copy(alpha = 0.16f), 0.42f to Color.Transparent,
)
private val TileBase = Brush.linearGradient(
    listOf(Color.White.copy(alpha = 0.13f), Color.White.copy(alpha = 0.05f)),
)
private val IconBrush = Brush.verticalGradient(listOf(Color(0xFF66C629), Color(0xFF168B16)))

/** How a schema section is presented: Italian name, what it is about, its icon. */
internal class SectionLook(val name: String, val about: String, val icon: ImageVector)

internal fun sectionLook(title: String): SectionLook = when (title) {
    "Vulkan" -> SectionLook("Vulkan", l10n("Graphics driver", "Driver grafico"), Glyphs.board)
    "GPU" -> SectionLook(l10n("Graphics", "Grafica"), "GPU", Glyphs.chip)
    "Video" -> SectionLook("Video", l10n("Video output", "Uscita video"), Glyphs.tv)
    "Display" -> SectionLook(l10n("Display", "Schermo"), l10n("Refresh rate and format", "Frequenza e formato"), Glyphs.phone)
    "UI" -> SectionLook(l10n("Interface", "Interfaccia"), "Overlay", Glyphs.dashboard)
    "Storage" -> SectionLook(l10n("Storage", "Archiviazione"), l10n("Folders", "Cartelle"), Glyphs.storage)
    "Content" -> SectionLook(l10n("Content", "Contenuti"), l10n("DLC and updates", "DLC e aggiornamenti"), Glyphs.folder)
    "Kernel" -> SectionLook(l10n("System", "Sistema"), "Kernel", Glyphs.tune)
    "CPU" -> SectionLook(l10n("Processor", "Processore"), l10n("CPU and JIT", "CPU e JIT"), Glyphs.chip)
    "Memory" -> SectionLook(l10n("Memory", "Memoria"), "RAM", Glyphs.chip)
    "APU" -> SectionLook("Audio", "APU", Glyphs.volume)
    "Controller" -> SectionLook("Controller", "Pad", Glyphs.pad)
    "HID" -> SectionLook("Input", l10n("Keyboard and touch", "Tastiera e touch"), Glyphs.keyboard)
    "XConfig" -> SectionLook("Console", l10n("Language and region", "Lingua e regione"), Glyphs.globe)
    "Logging" -> SectionLook(l10n("Log", "Registro"), l10n("Diagnostics", "Diagnostica"), Glyphs.document)
    "General" -> SectionLook(l10n("General", "Generale"), l10n("Startup", "Avvio"), Icons.Filled.Settings)
    else -> SectionLook(title, "", Icons.Filled.Settings)
}

/** Back disc, large light title and one line of context, as on the dashboard. */
@Composable
internal fun NxeHeader(title: String, subtitle: String?, onBack: () -> Unit) {
    Column(Modifier.fillMaxWidth().padding(top = 10.dp, bottom = 6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier
                    .size(40.dp)
                    .clip(CircleShape)
                    .background(Color.White.copy(alpha = 0.12f))
                    .border(1.dp, Color.White.copy(alpha = 0.22f), CircleShape)
                    .clickable(role = Role.Button, onClick = onBack),
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = l10n("Back", "Indietro"), tint = Color.White)
            }
            Spacer(Modifier.width(14.dp))
            Text(
                title,
                fontFamily = XdFont,
                fontWeight = FontWeight.Light,
                fontSize = 32.sp,
                color = Color.White,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        if (subtitle != null) {
            Text(
                subtitle,
                fontFamily = XdFont,
                fontSize = 13.sp,
                color = Color.White.copy(alpha = 0.7f),
                modifier = Modifier.padding(start = 54.dp),
            )
        }
    }
}

/** A section as a glass tile: icon, name, what it covers, and how many values differ. */
@Composable
internal fun SectionTile(look: SectionLook, settingsCount: Int, modified: Int, onOpen: () -> Unit) {
    Box(
        Modifier
            .fillMaxWidth()
            .height(118.dp)
            .clip(TileShape)
            .background(TileBase)
            .drawWithContent {
                drawContent()
                drawRect(TileSheen)
            }
            .border(1.dp, Color.White.copy(alpha = 0.16f), TileShape)
            .clickable(role = Role.Button, onClick = onOpen)
            .padding(14.dp),
    ) {
        Box(
            Modifier.size(40.dp).background(IconBrush, RoundedCornerShape(12.dp)),
            contentAlignment = Alignment.Center,
        ) {
            Icon(look.icon, contentDescription = null, tint = Color.White, modifier = Modifier.size(22.dp))
        }
        if (modified > 0) {
            Text(
                if (modified == 1) l10n("1 modified", "1 modificata") else l10n("$modified modified", "$modified modificate"),
                fontFamily = XdFont,
                fontSize = 11.sp,
                fontWeight = FontWeight.SemiBold,
                color = XdColors.caution,
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .background(XdColors.caution.copy(alpha = 0.16f), RoundedCornerShape(50))
                    .border(1.dp, XdColors.caution.copy(alpha = 0.45f), RoundedCornerShape(50))
                    .padding(horizontal = 8.dp, vertical = 3.dp),
            )
        }
        Column(Modifier.align(Alignment.BottomStart), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(look.name, fontFamily = XdFont, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, color = Color.White)
            Text(
                listOf(look.about, "$settingsCount " + if (settingsCount == 1) l10n("option", "opzione") else l10n("options", "opzioni"))
                    .filter { it.isNotEmpty() }.joinToString(" · "),
                fontFamily = XdFont,
                fontSize = 12.sp,
                color = Color.White.copy(alpha = 0.65f),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

/** Material icon outlines (Apache 2.0) as vectors, without the whole extended icon set. */
private object Glyphs {
    private fun glyph(name: String, d: String) = ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f)
        .addPath(addPathNodes(d), fill = SolidColor(Color.White))
        .build()

    val board by lazy { glyph("board", "M22 9V7h-2V5c0-1.1-.9-2-2-2H4c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h14c1.1 0 2-.9 2-2v-2h2v-2h-2v-2h2v-2h-2V9h2zm-4 10H4V5h14v14zM6 13h5v4H6zm6-6h4v3h-4zM6 7h5v5H6zm6 4h4v6h-4z") }
    val chip by lazy { glyph("chip", "M15 9H9v6h6V9zm-2 4h-2v-2h2v2zm8-2V9h-2V7c0-1.1-.9-2-2-2h-2V3h-2v2h-2V3H9v2H7c-1.1 0-2 .9-2 2v2H3v2h2v2H3v2h2v2c0 1.1.9 2 2 2h2v2h2v-2h2v2h2v-2h2c1.1 0 2-.9 2-2v-2h2v-2h-2v-2h2zm-4 6H7V7h10v10z") }
    val tv by lazy { glyph("tv", "M21 3H3c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h5v2h8v-2h5c1.1 0 1.99-.9 1.99-2L23 5c0-1.1-.9-2-2-2zm0 14H3V5h18v12z") }
    val phone by lazy { glyph("phone", "M17 1.01L7 1c-1.1 0-2 .9-2 2v18c0 1.1.9 2 2 2h10c1.1 0 2-.9 2-2V3c0-1.1-.9-1.99-2-1.99zM17 19H7V5h10v14z") }
    val dashboard by lazy { glyph("dashboard", "M3 13h8V3H3v10zm0 8h8v-6H3v6zm10 0h8V11h-8v10zm0-18v6h8V3h-8z") }
    val storage by lazy { glyph("storage", "M2 20h20v-4H2v4zm2-3h2v2H4v-2zM2 4v4h20V4H2zm4 3H4V5h2v2zm-4 7h20v-4H2v4zm2-3h2v2H4v-2z") }
    val folder by lazy { glyph("folder", "M10 4H4c-1.1 0-1.99.9-1.99 2L2 18c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2h-8l-2-2z") }
    val tune by lazy { glyph("tune", "M3 17v2h6v-2H3zM3 5v2h10V5H3zm10 16v-2h8v-2h-8v-2h-2v6h2zM7 9v2H3v2h4v2h2V9H7zm14 4v-2H11v2h10zm-6-4h2V7h4V5h-4V3h-2v6z") }
    val volume by lazy { glyph("volume", "M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02zM14 3.23v2.06c2.89.86 5 3.54 5 6.71s-2.11 5.85-5 6.71v2.06c4.01-.91 7-4.49 7-8.77s-2.99-7.86-7-8.77z") }
    val pad by lazy { glyph("pad", "M21.58 16.09l-1.09-7.66C20.21 6.46 18.52 5 16.53 5H7.47C5.48 5 3.79 6.46 3.51 8.43l-1.09 7.66C2.2 17.63 3.39 19 4.94 19c.68 0 1.32-.27 1.8-.75L9 16h6l2.25 2.25c.48.48 1.13.75 1.8.75 1.56 0 2.75-1.37 2.53-2.91zM11 11H9v2H8v-2H6v-1h2V8h1v2h2v1zm4-1c-.55 0-1-.45-1-1s.45-1 1-1 1 .45 1 1-.45 1-1 1zm2 3c-.55 0-1-.45-1-1s.45-1 1-1 1 .45 1 1-.45 1-1 1z") }
    val keyboard by lazy { glyph("keyboard", "M20 5H4c-1.1 0-1.99.9-1.99 2L2 17c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V7c0-1.1-.9-2-2-2zm-9 3h2v2h-2V8zm0 3h2v2h-2v-2zM8 8h2v2H8V8zm0 3h2v2H8v-2zm-1 2H5v-2h2v2zm0-3H5V8h2v2zm9 7H8v-2h8v2zm0-4h-2v-2h2v2zm0-3h-2V8h2v2zm3 3h-2v-2h2v2zm0-3h-2V8h2v2z") }
    val globe by lazy { glyph("globe", "M11.99 2C6.47 2 2 6.48 2 12s4.47 10 9.99 10C17.52 22 22 17.52 22 12S17.52 2 11.99 2zm6.93 6h-2.95c-.32-1.25-.78-2.45-1.38-3.56 1.84.63 3.37 1.91 4.33 3.56zM12 4.04c.83 1.2 1.48 2.53 1.91 3.96h-3.82c.43-1.43 1.08-2.76 1.91-3.96zM4.26 14C4.1 13.36 4 12.69 4 12s.1-1.36.26-2h3.38c-.08.66-.14 1.32-.14 2s.06 1.34.14 2H4.26zm.82 2h2.95c.32 1.25.78 2.45 1.38 3.56-1.84-.63-3.37-1.9-4.33-3.56zm2.95-8H5.08c.96-1.66 2.49-2.93 4.33-3.56C8.81 5.55 8.35 6.75 8.03 8zM12 19.96c-.83-1.2-1.48-2.53-1.91-3.96h3.82c-.43 1.43-1.08 2.76-1.91 3.96zM14.34 14H9.66c-.09-.66-.16-1.32-.16-2s.07-1.35.16-2h4.68c.09.65.16 1.32.16 2s-.07 1.34-.16 2zm.25 5.56c.6-1.11 1.06-2.31 1.38-3.56h2.95c-.96 1.65-2.49 2.93-4.33 3.56zM16.36 14c.08-.66.14-1.32.14-2s-.06-1.34-.14-2h3.38c.16.64.26 1.31.26 2s-.1 1.36-.26 2h-3.38z") }
    val document by lazy { glyph("document", "M14 2H6c-1.1 0-1.99.9-1.99 2L4 20c0 1.1.89 2 1.99 2H18c1.1 0 2-.9 2-2V8l-6-6zm2 16H8v-2h8v2zm0-4H8v-2h8v2zm-3-5V3.5L18.5 9H13z") }
}
