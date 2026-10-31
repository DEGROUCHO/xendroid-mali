package xendroid.compose.ui.content

import xendroid.compose.l10n

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import xendroid.compose.core.ContentPaths
import java.io.File

/** Shared install flow state for both the per-game ContentManager and the global
 *  install-only entrypoint. Delete stays per-game (see ContentManagerViewModel). */
sealed interface ContentInstallState {
    data object Idle : ContentInstallState
    data class Busy(val message: String, val progress: Float = -1f) : ContentInstallState
    /** Existing package dir found -> ask the user before clobbering. */
    data class ConfirmOverwrite(val srcPath: String, val displayName: String) : ContentInstallState
    data class Done(val message: String) : ContentInstallState
    data class Failed(val message: String) : ContentInstallState
}

/** Native install_content status -> human message. Shared by both VMs. */
fun installReasonFor(status: Int): String = when (status) {
    -1 -> l10n("Emulator not loaded.", "Emulatore non caricato.")
    0xC000000D.toInt() -> l10n("The package is damaged or unsupported.", "Il pacchetto è danneggiato o non supportato.")   // X_STATUS_INVALID_PARAMETER
    0xC0000022.toInt() -> l10n("Couldn't write to the content folder.", "Impossibile scrivere nella cartella dei contenuti.")    // X_STATUS_ACCESS_DENIED
    0xC000007F.toInt() -> l10n("Not enough free space to install.", "Spazio libero insufficiente per l'installazione.")        // X_STATUS_DISK_FULL
    else -> l10n("Installation failed (0x${status.toUInt().toString(16)}).", "Installazione non riuscita (0x${status.toUInt().toString(16)}).")
}

/** Free-space pre-flight against the volume [target] lives on; 1.1x mirrors the native
 *  guard. Null when it fits or the size/free space is unknown (defer to the native guard). */
fun storageShortfallOn(target: File, requiredBytes: Long): String? {
    if (requiredBytes <= 0L) return null
    val probe = if (target.exists()) target else (target.parentFile ?: target)
    val free = probe.usableSpace.takeIf { it > 0L } ?: return null
    val needed = (requiredBytes * 11) / 10
    if (free >= needed) return null
    return l10n("Not enough free space: about ${formatBytes(needed)} needed ", "Spazio libero insufficiente: servono circa ${formatBytes(needed)} ") +
        l10n("but only ${formatBytes(free)} free. Free up space and try again.", "ma sono liberi solo ${formatBytes(free)}. Libera spazio e riprova.")
}

/** Free-space check for the content root (the native install volume), shared by both VMs. */
fun storageShortfall(requiredBytes: Long): String? =
    storageShortfallOn(ContentPaths.contentRoot(), requiredBytes)

private fun formatBytes(b: Long): String {
    if (b < 1024) return "$b B"
    val u = arrayOf("KB", "MB", "GB", "TB")
    var v = b.toDouble()
    var i = -1
    do { v /= 1024.0; i++ } while (v >= 1024.0 && i < u.lastIndex)
    return "%.1f %s".format(v, u[i])
}

/** Busy / ConfirmOverwrite / Done / Failed dialogs for the shared install flow. */
@Composable
fun ContentInstallDialogs(
    state: ContentInstallState,
    onDismiss: () -> Unit,
    onConfirmOverwrite: (srcPath: String, displayName: String) -> Unit,
) {
    when (val s = state) {
        is ContentInstallState.Busy -> AlertDialog(
            onDismissRequest = {},
            title = { Text(s.message) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (s.progress >= 0f) {
                        LinearProgressIndicator(
                            progress = { s.progress },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        Text(l10n("${(s.progress * 100).toInt()}%  ·  this may take a while.", "${(s.progress * 100).toInt()}%  ·  potrebbe richiedere un po' di tempo."))
                    } else {
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                        Text(l10n("This may take a while.", "Potrebbe richiedere un po' di tempo."))
                    }
                }
            },
            confirmButton = {},
        )
        is ContentInstallState.ConfirmOverwrite -> AlertDialog(
            onDismissRequest = onDismiss,
            title = { Text(l10n("Already installed", "Già installato")) },
            text = { Text(l10n("“${s.displayName}” is already installed. Overwrite it?", "“${s.displayName}” è già installato. Sovrascriverlo?")) },
            confirmButton = {
                TextButton(onClick = { onConfirmOverwrite(s.srcPath, s.displayName) }) {
                    Text(l10n("Overwrite", "Sovrascrivi"))
                }
            },
            dismissButton = { TextButton(onClick = onDismiss) { Text(l10n("Cancel", "Annulla")) } },
        )
        is ContentInstallState.Done -> AlertDialog(
            onDismissRequest = onDismiss,
            title = { Text(l10n("Done", "Fatto")) },
            text = { Text(s.message) },
            confirmButton = { TextButton(onClick = onDismiss) { Text("OK") } },
        )
        is ContentInstallState.Failed -> AlertDialog(
            onDismissRequest = onDismiss,
            title = { Text(l10n("Couldn't complete", "Impossibile completare")) },
            text = { Text(s.message) },
            confirmButton = { TextButton(onClick = onDismiss) { Text("OK") } },
        )
        ContentInstallState.Idle -> {}
    }
}
