package xendroid.compose.ui.settings

import xendroid.compose.l10n
import xendroid.compose.UiLanguage
import android.app.Activity
import androidx.compose.ui.platform.LocalContext

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items as gridItems
import androidx.compose.foundation.border
import xendroid.compose.ui.library.NxeSurface
import xendroid.compose.ui.library.LibraryStyle
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Search
import xendroid.compose.settings.Setting
import xendroid.compose.ui.theme.XdColors
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import xendroid.compose.settings.SettingValue
import xendroid.compose.settings.SettingsCategory
import xendroid.compose.settings.SettingsViewModel

/**
 * Two-level settings: an INDEX of sections (the 124-entry schema is too long for one list), and
 * a per-section DETAIL with that section's rows. Navigation between the two is internal state so
 * the single SettingsViewModel (and its config handle / flush lifecycle) is shared; the system
 * back button goes detail->index->exit via [BackHandler].
 */
@Composable
fun SettingsScreen(vm: SettingsViewModel, onBack: () -> Unit) {
    val values by vm.values.collectAsStateWithLifecycle()

    // Durable flush on pause; re-open on resume. Dispose flush = backstop.
    val owner = LocalLifecycleOwner.current
    DisposableEffect(owner) {
        val obs = LifecycleEventObserver { _, e ->
            when (e) {
                Lifecycle.Event.ON_PAUSE -> vm.flush()
                Lifecycle.Event.ON_RESUME -> vm.onResume()
                else -> {}
            }
        }
        owner.lifecycle.addObserver(obs)
        onDispose { owner.lifecycle.removeObserver(obs); vm.flush() }
    }

    var selected by remember { mutableStateOf<SettingsCategory?>(null) }
    val section = selected
    if (section == null) {
        SettingsIndex(
            categories = vm.categories,
            modifiedCountOf = { cat -> cat.settings.count { values[it.key]?.modified == true } },
            onOpen = { selected = it },
            onBack = { vm.flush(); onBack() },
            values = values,
            vm = vm,
        )
    } else {
        BackHandler { selected = null }
        SettingsCategoryDetail(
            category = section,
            values = values,
            vm = vm,
            onBack = { selected = null },
        )
    }
}

/** Pure: every setting whose title, cvar name or description contains all words of [query]. */
fun searchSettings(categories: List<SettingsCategory>, query: String): List<Setting> {
    val words = query.trim().lowercase().split(Regex("\\s+")).filter { it.isNotEmpty() }
    if (words.isEmpty()) return emptyList()
    return categories.flatMap { it.settings }.distinctBy { it.key }.filter { s ->
        val hay = (s.title + " " + s.name + " " + s.desc).lowercase()
        words.all { it in hay }
    }
}

@Composable
private fun SettingsIndex(
    categories: List<SettingsCategory>,
    modifiedCountOf: (SettingsCategory) -> Int,
    onOpen: (SettingsCategory) -> Unit,
    onBack: () -> Unit,
    values: Map<String, SettingValue>,
    vm: SettingsViewModel,
) {
    var query by remember { mutableStateOf("") }
    val total = remember(categories) { categories.sumOf { it.settings.size } }
    val modifiedTotal = categories.sumOf(modifiedCountOf)
    NxeSurface(enabled = true) {
        val results = remember(categories, query) { searchSettings(categories, query) }
        LazyVerticalGrid(
            columns = GridCells.Fixed(2),
            modifier = Modifier.fillMaxSize().systemBarsPadding(),
            contentPadding = PaddingValues(start = 18.dp, end = 18.dp, bottom = 24.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item(key = "header", span = { GridItemSpan(maxLineSpan) }) {
                NxeHeader(
                    l10n("Settings", "Impostazioni"),
                    l10n("$total options · ", "$total opzioni · ") + if (modifiedTotal == 1) l10n("1 modified", "1 modificata") else l10n("$modifiedTotal modified", "$modifiedTotal modificate"),
                    onBack,
                )
            }
            item(key = "search", span = { GridItemSpan(maxLineSpan) }) {
                TextField(
                    value = query,
                    onValueChange = { query = it },
                    placeholder = { Text(l10n("Search settings", "Cerca tra le impostazioni")) },
                    leadingIcon = { Icon(Icons.Default.Search, contentDescription = null) },
                    trailingIcon = {
                        if (query.isNotEmpty()) IconButton(onClick = { query = "" }) {
                            Icon(Icons.Default.Close, contentDescription = l10n("Clear", "Cancella"))
                        }
                    },
                    singleLine = true,
                    shape = RoundedCornerShape(50),
                    colors = TextFieldDefaults.colors(
                        focusedContainerColor = GlassFill,
                        unfocusedContainerColor = GlassFill,
                        focusedIndicatorColor = Color.Transparent,
                        unfocusedIndicatorColor = Color.Transparent,
                    ),
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 6.dp)
                        .border(1.dp, GlassEdge, RoundedCornerShape(50)),
                )
            }
            if (query.isBlank()) item(key = "language", span = { GridItemSpan(maxLineSpan) }) { LanguageRow() }
            if (query.isBlank()) item(key = "library_style", span = { GridItemSpan(maxLineSpan) }) { LibraryStyleRow() }
            if (query.isNotBlank()) {
                if (results.isEmpty()) item(key = "none", span = { GridItemSpan(maxLineSpan) }) {
                    Text(l10n("No settings contain “${query.trim()}”", "Nessuna impostazione contiene “${query.trim()}”"),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(16.dp))
                }
                gridItems(results, key = { "r:" + it.key }, span = { GridItemSpan(maxLineSpan) }) { setting ->
                    val sv = values[setting.key]
                    GlassRow { SettingRow(vm, setting, modified = sv?.modified == true, raw = sv?.raw) }
                }
            } else gridItems(categories, key = { it.title }) { cat ->
                SectionTile(sectionLook(cat.title), cat.settings.size, modifiedCountOf(cat)) { onOpen(cat) }
            }
        }
    }
}

@Composable
private fun SettingsCategoryDetail(
    category: SettingsCategory,
    values: Map<String, SettingValue>,
    vm: SettingsViewModel,
    onBack: () -> Unit,
) {
    val look = sectionLook(category.title)
    NxeSurface(enabled = true) {
        LazyColumn(
            Modifier.fillMaxSize().systemBarsPadding(),
            contentPadding = PaddingValues(start = 18.dp, end = 18.dp, bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            item(key = "header") {
                NxeHeader(look.name, look.about.ifEmpty { null }, onBack)
            }
            item(key = "legend") {
                Text(l10n("Amber names differ from the default value.", "I nomi in ambra sono diversi dal valore predefinito."),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 4.dp, vertical = 6.dp))
            }
            items(category.settings, key = { it.key }) { setting ->
                val sv = values[setting.key]
                GlassRow { SettingRow(vm, setting, modified = sv?.modified == true, raw = sv?.raw) }
            }
        }
    }
}

/** UI language: follow the system, English or Italian. Changing it restarts the app. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LanguageRow() {
    val activity = LocalContext.current as? Activity ?: return
    val current = remember { UiLanguage.get(activity) }
    GlassRow {
        // The name sits above the choices: beside them it was squeezed onto three lines.
        Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 8.dp)) {
            Text("Language / Lingua", style = MaterialTheme.typography.titleSmall)
            Row(verticalAlignment = Alignment.CenterVertically) {
            listOf(
                UiLanguage.SYSTEM to l10n("System", "Sistema"),
                UiLanguage.ENGLISH to "English",
                UiLanguage.ITALIAN to "Italiano",
            ).forEach { (value, label) ->
                FilterChip(
                    selected = current == value,
                    onClick = { if (value != current) UiLanguage.setAndRestart(activity, value) },
                    label = { Text(label) },
                    modifier = Modifier.padding(end = 6.dp),
                )
            }
            }
        }
    }
}

/** The library's face: the Xbox dashboard with the game covers, or the classic grid. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LibraryStyleRow() {
    val context = LocalContext.current
    var dashboard by remember { mutableStateOf(LibraryStyle.isDashboard(context)) }
    GlassRow {
        Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 8.dp)) {
            Text(l10n("Library", "Libreria"), style = MaterialTheme.typography.titleSmall)
            Row(verticalAlignment = Alignment.CenterVertically) {
            listOf(
                true to l10n("Xbox covers", "Copertine Xbox"),
                false to l10n("Classic grid", "Griglia classica"),
            ).forEach { (value, label) ->
                FilterChip(
                    selected = dashboard == value,
                    onClick = { dashboard = value; LibraryStyle.setDashboard(context, value) },
                    label = { Text(label) },
                    modifier = Modifier.padding(end = 6.dp),
                )
            }
            }
        }
    }
}

/** A setting row on a glass panel over the backdrop. */
@Composable
private fun GlassRow(content: @Composable () -> Unit) {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = GlassFill,
        border = BorderStroke(1.dp, GlassEdge),
        content = content,
    )
}
