package xendroid.compose.ui.keymap

import xendroid.compose.l10n

import android.view.KeyEvent as AndroidKeyEvent
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.key.*
import androidx.lifecycle.compose.collectAsStateWithLifecycle

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun KeymapScreen(vm: KeymapViewModel, onBack: () -> Unit) {
    val state by vm.state.collectAsStateWithLifecycle()
    var capturing by remember { mutableStateOf<KeymapRow?>(null) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(l10n("Key mapping", "Mappatura tasti")) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = l10n("Back", "Indietro"))
                    }
                },
                actions = {
                    TextButton(onClick = { vm.onResetDefaults() }) { Text(l10n("Reset", "Ripristina")) }
                },
            )
        }
    ) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding)) {
            items(state.rows, key = { it.button.index }) { row ->
                ListItem(
                    headlineContent = { Text(row.button.label) },
                    supportingContent = { Text(keyLabel(row.boundKey)) },
                    trailingContent = {
                        TextButton(onClick = { vm.onClear(row.button.index) }) { Text(l10n("Clear", "Cancella")) }
                    },
                    modifier = Modifier.clickable { capturing = row },
                )
                HorizontalDivider()
            }
        }
    }

    capturing?.let { row ->
        KeyCaptureDialog(
            label = row.button.label,
            onKey = { code -> vm.onKeyCaptured(row.button.index, code); capturing = null },
            onDismiss = { capturing = null },
        )
    }
}

@Composable
private fun KeyCaptureDialog(label: String, onKey: (Int) -> Unit, onDismiss: () -> Unit) {
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    BackHandler(enabled = true, onBack = onDismiss)
    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = {},
        dismissButton = { TextButton(onClick = onDismiss) { Text(l10n("Cancel", "Annulla")) } },
        title = { Text(l10n("Press a key for $label", "Premi un tasto per $label")) },
        text = {
            Box(
                Modifier
                    .fillMaxWidth()
                    .focusRequester(focus)
                    .focusable()
                    .onKeyEvent { ev ->
                        if (ev.type == KeyEventType.KeyDown) {
                            onKey(ev.nativeKeyEvent.keyCode); true
                        } else false
                    }
            ) { Text(l10n("Waiting for a controller or keyboard key…", "In attesa di un tasto del controller o della tastiera…")) }
        },
    )
}

/** Human-readable name for a bound Android keycode (0 = unbound). */
private fun keyLabel(code: Int): String =
    if (code == 0) l10n("(unassigned)", "(non assegnato)")
    else AndroidKeyEvent.keyCodeToString(code).removePrefix("KEYCODE_")
