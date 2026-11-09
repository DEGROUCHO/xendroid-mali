package xendroid.compose.ui.settings

import xendroid.compose.l10n

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import xendroid.compose.settings.GameSettingsViewModel
import xendroid.compose.settings.PerGamePresets
import xendroid.compose.settings.SettingsCategory
import xendroid.compose.settings.autoPreset
import xendroid.compose.settings.hostIsAdreno

/**
 * The per-game override editor: the same two-level INDEX -> DETAIL shape as
 * [SettingsScreen], but each detail row is an [OverrideRow] (a leading switch that
 * overrides/inherits the key), the index "changed" count is the overridden count, and
 * the header shows the game name. The override config is SPARSE — only toggled-on keys
 * are written, and the file is rebuilt/deleted on flush (no native key-erase exists).
 * A title id is keyed PER GAME (not per file), so this applies to every copy of the game.
 */
@Composable
fun PerGameSettingsScreen(
    vm: GameSettingsViewModel,
    gameName: String,
    onBack: () -> Unit,
) {
    val overrides by vm.overrides.collectAsStateWithLifecycle()

    // Durable flush on pause; re-open on resume. Dispose flush = backstop. (Mirrors SettingsScreen.)
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
        PerGameIndex(
            gameName = gameName,
            vm = vm,
            categories = vm.categories,
            overriddenCountOf = { cat -> cat.settings.count { overrides.containsKey(it.key) } },
            onOpen = { selected = it },
            onBack = { vm.flush(); onBack() },
        )
    } else {
        BackHandler { selected = null }
        PerGameCategoryDetail(
            category = section,
            overrides = overrides,
            vm = vm,
            onBack = { selected = null },
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PerGameIndex(
    gameName: String,
    vm: GameSettingsViewModel,
    categories: List<SettingsCategory>,
    overriddenCountOf: (SettingsCategory) -> Int,
    onOpen: (SettingsCategory) -> Unit,
    onBack: () -> Unit,
) {
    var appliedPreset by remember { mutableStateOf<String?>(null) }
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(if (gameName.isNotEmpty()) l10n("Settings for $gameName", "Impostazioni di $gameName") else l10n("Per-game settings", "Impostazioni per gioco")) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = l10n("Back", "Indietro"))
                    }
                },
            )
        }
    ) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding)) {
            // Header note: title id is keyed per game, so overrides apply to every copy.
            item {
                Text(
                    l10n("Overrides apply to all copies of this game.", "Le personalizzazioni valgono per tutte le copie di questo gioco."),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                )
                HorizontalDivider()
            }
            item {
                Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
                    Text(l10n("Quick presets", "Preset rapidi"), style = MaterialTheme.typography.titleSmall)
                    Spacer(Modifier.height(8.dp))
                    Row(
                        Modifier.horizontalScroll(rememberScrollState()),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        FilterChip(
                            selected = appliedPreset == "auto",
                            onClick = {
                                vm.applyPreset(autoPreset(hostIsAdreno()))
                                vm.flush()
                                appliedPreset = "auto"
                            },
                            label = { Text(l10n("Automatic", "Automatico")) },
                        )
                        PerGamePresets.all.forEach { p ->
                            FilterChip(
                                selected = appliedPreset == p.id,
                                onClick = {
                                    vm.applyPreset(p)
                                    vm.flush()
                                    appliedPreset = p.id
                                },
                                label = { Text(p.title) },
                            )
                        }
                    }
                    appliedPreset?.let {
                        Spacer(Modifier.height(8.dp))
                        Text(
                            l10n("Preset applied. Restart the game to apply it.", "Preset applicato. Riavvia il gioco per renderlo effettivo."),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                HorizontalDivider()
            }
            items(categories, key = { it.title }) { cat ->
                val overridden = overriddenCountOf(cat)
                ListItem(
                    headlineContent = { Text(cat.title) },
                    supportingContent = {
                        Text(buildString {
                            append(l10n("${cat.settings.size} settings", "${cat.settings.size} impostazioni"))
                            if (overridden > 0) append(if (overridden == 1) l10n("  ·  1 customized", "  ·  1 personalizzata") else l10n("  ·  $overridden customized", "  ·  $overridden personalizzate"))
                        })
                    },
                    trailingContent = {
                        Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, contentDescription = l10n("Open", "Apri"))
                    },
                    modifier = Modifier.clickable { onOpen(cat) },
                )
                HorizontalDivider()
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PerGameCategoryDetail(
    category: SettingsCategory,
    overrides: Map<String, String>,
    vm: GameSettingsViewModel,
    onBack: () -> Unit,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(category.title) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = l10n("Back to sections", "Torna alle sezioni"))
                    }
                },
            )
        }
    ) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding)) {
            items(category.settings, key = { it.key }) { setting ->
                OverrideRow(
                    host = vm,
                    s = setting,
                    overrideValue = overrides[setting.key],
                    onOverrideToggle = { vm.setOverride(setting, it) },
                )
                HorizontalDivider()
            }
        }
    }
}
