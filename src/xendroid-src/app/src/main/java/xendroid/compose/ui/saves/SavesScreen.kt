package xendroid.compose.ui.saves

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SavesScreen(titleId: String, gameName: String, onBack: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }

    var saves by remember { mutableStateOf<List<SaveEntry>>(emptyList()) }
    var loading by remember { mutableStateOf(true) }
    var pendingDelete by remember { mutableStateOf<SaveEntry?>(null) }

    LaunchedEffect(titleId) {
        loading = true
        saves = withContext(Dispatchers.IO) { SavesStore.list(context, titleId) }
        loading = false
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        if (gameName.isNotBlank()) "$gameName saves" else "Saves",
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            when {
                loading -> CircularProgressIndicator(Modifier.align(Alignment.Center))
                saves.isEmpty() -> Text(
                    "No saves yet",
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier.align(Alignment.Center).padding(24.dp),
                )
                else -> LazyColumn(Modifier.fillMaxSize()) {
                    items(saves, key = { it.dir.absolutePath }) { entry ->
                        SaveRow(
                            entry = entry,
                            onExport = {
                                scope.launch {
                                    val outcome = withContext(Dispatchers.IO) {
                                        runCatching { SavesStore.export(context, entry, gameName) }
                                    }
                                    snackbar.showSnackbar(
                                        outcome.fold(
                                            onSuccess = { "Exported to ${it.absolutePath}" },
                                            onFailure = { "Export failed: ${it.message ?: "unknown error"}" },
                                        )
                                    )
                                }
                            },
                            onDelete = { pendingDelete = entry },
                        )
                        HorizontalDivider()
                    }
                }
            }
        }
    }

    pendingDelete?.let { entry ->
        AlertDialog(
            onDismissRequest = { pendingDelete = null },
            title = { Text("Delete save?") },
            text = { Text("Delete “${entry.displayName}”? This can't be undone.") },
            confirmButton = {
                TextButton(onClick = {
                    pendingDelete = null
                    scope.launch {
                        val deleted = withContext(Dispatchers.IO) { SavesStore.delete(entry) }
                        if (deleted) {
                            saves = saves.filterNot { it.dir.absolutePath == entry.dir.absolutePath }
                            snackbar.showSnackbar("Deleted “${entry.displayName}”.")
                        } else {
                            snackbar.showSnackbar("Couldn't delete “${entry.displayName}”.")
                        }
                    }
                }) { Text("Delete") }
            },
            dismissButton = {
                TextButton(onClick = { pendingDelete = null }) { Text("Cancel") }
            },
        )
    }
}

@Composable
private fun SaveRow(entry: SaveEntry, onExport: () -> Unit, onDelete: () -> Unit) {
    ListItem(
        headlineContent = {
            Text(entry.displayName, maxLines = 1, overflow = TextOverflow.Ellipsis)
        },
        supportingContent = {
            Text("${formatDate(entry.modifiedAt)}  ·  ${formatBytes(entry.sizeBytes)}")
        },
        trailingContent = {
            Row {
                TextButton(onClick = onExport) { Text("Export") }
                TextButton(onClick = onDelete) { Text("Delete") }
            }
        },
    )
}

private fun formatDate(epochMillis: Long): String =
    SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault()).format(Date(epochMillis))

private fun formatBytes(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val units = arrayOf("KB", "MB", "GB")
    var value = bytes.toDouble() / 1024
    var unit = 0
    while (value >= 1024 && unit < units.lastIndex) {
        value /= 1024
        unit++
    }
    return "%.1f %s".format(value, units[unit])
}
