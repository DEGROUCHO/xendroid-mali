package xendroid.compose.ui.patches

import xendroid.compose.l10n

import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import xendroid.compose.patches.GamePatchesViewModel
import xendroid.compose.patches.PatchEntry
import xendroid.compose.patches.PatchFile

/**
 * Lists the bundled and imported patches for one game, grouped by file. Each `[[patch]]` is a row
 * with a Switch that toggles its on-disk `is_enabled` (effective on the next launch). "Import"
 * adds a community .patch.toml picked from storage.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GamePatchesScreen(
    vm: GamePatchesViewModel,
    gameName: String,
    onBack: () -> Unit,
) {
    val state by vm.state.collectAsStateWithLifecycle()
    val importResult by vm.importResult.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val snackbar = remember { SnackbarHostState() }

    val pickPatch = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri == null) return@rememberLauncherForActivityResult
        val resolver = context.contentResolver
        val name = runCatching {
            resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) c.getString(0) else null
            }
        }.getOrNull() ?: uri.lastPathSegment.orEmpty()
        // Patch files are a few KB; refuse anything huge rather than read it whole.
        val text = runCatching {
            resolver.openInputStream(uri)?.use { input ->
                val out = java.io.ByteArrayOutputStream()
                val buf = ByteArray(16 shl 10)
                while (out.size() < (1 shl 20)) {
                    val n = input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                }
                if (out.size() >= (1 shl 20)) null else out.toString(Charsets.UTF_8.name())
            }
        }.getOrNull()
        vm.importPatch(name, text)
    }
    val launchPicker = { pickPatch.launch(arrayOf("*/*")) }

    LaunchedEffect(importResult) {
        val r = importResult ?: return@LaunchedEffect
        val msg = when (r) {
            GamePatchesViewModel.ImportResult.Invalid ->
                l10n("That file is not a patch (.patch.toml with a title_id and [[patch]] entries).",
                    "Questo file non è una patch (.patch.toml con title_id e voci [[patch]]).")
            is GamePatchesViewModel.ImportResult.Added ->
                if (r.forThisGame) {
                    l10n("Patch added. Turn on what you need: it applies on the next launch.",
                        "Patch aggiunta. Attiva quello che ti serve: vale dal prossimo avvio.")
                } else {
                    l10n("Patch saved, but it is for ${r.file.titleName.ifEmpty { r.file.titleId }} (${r.file.titleId}): it shows under that game.",
                        "Patch salvata, ma è per ${r.file.titleName.ifEmpty { r.file.titleId }} (${r.file.titleId}): la trovi sotto quel gioco.")
                }
        }
        vm.importResultShown()
        snackbar.showSnackbar(msg)
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbar) },
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        if (gameName.isNotBlank()) "Patch · $gameName" else "Patch",
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = l10n("Back", "Indietro"))
                    }
                },
                actions = {
                    TextButton(onClick = launchPicker) {
                        Icon(Icons.Filled.Add, contentDescription = null)
                        Spacer(Modifier.width(4.dp))
                        Text(l10n("Import", "Importa"))
                    }
                },
            )
        },
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center) {
            when (val s = state) {
                GamePatchesViewModel.UiState.Loading -> CircularProgressIndicator()
                GamePatchesViewModel.UiState.Empty ->
                    Column(
                        Modifier.padding(24.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text(
                            l10n("No patches included for this game.", "Nessuna patch inclusa per questo gioco."),
                            style = MaterialTheme.typography.bodyLarge,
                            textAlign = TextAlign.Center,
                        )
                        Spacer(Modifier.height(8.dp))
                        Text(
                            l10n("Have a .patch.toml from the community? Import it here.",
                                "Hai un file .patch.toml della community? Importalo qui."),
                            style = MaterialTheme.typography.bodyMedium,
                            textAlign = TextAlign.Center,
                        )
                        Spacer(Modifier.height(16.dp))
                        Button(onClick = launchPicker) {
                            Icon(Icons.Filled.Add, contentDescription = null)
                            Spacer(Modifier.width(8.dp))
                            Text(l10n("Import a patch file", "Importa un file patch"))
                        }
                    }
                is GamePatchesViewModel.UiState.Error ->
                    Text(s.message, modifier = Modifier.padding(24.dp))
                is GamePatchesViewModel.UiState.Loaded ->
                    PatchList(files = s.files, onToggle = vm::toggle)
            }
        }
    }
}

@Composable
private fun PatchList(
    files: List<PatchFile>,
    onToggle: (PatchFile, PatchEntry, Boolean) -> Unit,
) {
    val showHeaders = files.size > 1
    LazyColumn(Modifier.fillMaxSize()) {
        item {
            Text(
                l10n("Patches apply on the next launch of this game, and only if they match the game version.", "Le patch si applicano al prossimo avvio di questo gioco, e solo se corrispondono alla versione del gioco."),
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            )
        }
        for (file in files) {
            if (showHeaders) {
                item(key = "hdr:${file.fileName}") {
                    Text(
                        file.variantLabel,
                        style = MaterialTheme.typography.titleSmall,
                        modifier = Modifier.padding(start = 16.dp, top = 12.dp, bottom = 4.dp),
                    )
                }
            }
            items(file.entries, key = { "${file.fileName}#${it.index}" }) { entry ->
                PatchRow(
                    entry = entry,
                    onCheckedChange = { checked -> onToggle(file, entry, checked) },
                )
            }
            item(key = "div:${file.fileName}") { HorizontalDivider() }
        }
    }
}

@Composable
private fun PatchRow(entry: PatchEntry, onCheckedChange: (Boolean) -> Unit) {
    ListItem(
        headlineContent = { Text(entry.name) },
        supportingContent = {
            val sub = listOfNotNull(
                entry.desc?.takeIf { it.isNotBlank() },
                entry.author?.takeIf { it.isNotBlank() }?.let { l10n("by $it", "di $it") },
            ).joinToString("\n")
            if (sub.isNotBlank()) Text(sub)
        },
        trailingContent = {
            Switch(checked = entry.isEnabled, onCheckedChange = onCheckedChange)
        },
    )
}
