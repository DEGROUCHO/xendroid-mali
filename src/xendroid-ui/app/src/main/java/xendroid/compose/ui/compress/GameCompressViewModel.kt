package xendroid.compose.ui.compress

import xendroid.compose.l10n

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import xendroid.compose.core.EmulatorRuntime
import java.io.File

/**
 * Drives the per-game "Compress to .zar" safe-replace flow. Deleting the original .iso is
 * offered only after a verified .zar has been placed beside it, and happens only on explicit
 * confirmation; every failure / early-return path leaves the .iso intact. The native call
 * blocks, so it runs on [Dispatchers.IO]; the temp .zar lives in cacheDir and is always
 * removed in a finally.
 */
class GameCompressViewModel(
    private val appContext: Context,
) : ViewModel() {

    sealed interface CompressState {
        data object Idle : CompressState
        data class Busy(val message: String, val progress: Float = -1f) : CompressState
        /** The verified .zar is in place and the .iso is STILL there; the user decides. */
        data class ConfirmDelete(
            val isoPath: String,
            val isoName: String,
            val zarName: String,
            val isoBytes: Long,
        ) : CompressState
        data class Done(val message: String) : CompressState
        /** Any failure; the .iso is untouched. */
        data class Failed(val message: String) : CompressState
    }

    private val _state = MutableStateFlow<CompressState>(CompressState.Idle)
    val state: StateFlow<CompressState> = _state.asStateFlow()

    fun dismiss() { _state.value = CompressState.Idle }

    /** Compress the ISO at [launchUri] into a verified .zar beside it, then ASK whether to
     *  delete the .iso — and only then. */
    fun compress(launchUri: String) = viewModelScope.launch {
        _state.value = CompressState.Busy(l10n("Compressing…", "Compressione…"), 0f)
        // Concurrent read is safe: the progress getter reads file-static atomics.
        val poll = launch {
            while (isActive) {
                val p = EmulatorRuntime.emulator?.compressProgress() ?: 0f
                (_state.value as? CompressState.Busy)
                    ?.takeIf { it.message == l10n("Compressing…", "Compressione…") }
                    ?.let { _state.value = it.copy(progress = p.coerceIn(0f, 1f)) }
                delay(200)
            }
        }
        val result = withContext(Dispatchers.IO) { runCompress(launchUri) }
        poll.cancel()
        _state.value = result
    }

    fun deleteIso() = viewModelScope.launch {
        val s = _state.value as? CompressState.ConfirmDelete ?: return@launch
        val deleted = withContext(Dispatchers.IO) { File(s.isoPath).delete() }
        _state.value = CompressState.Done(
            if (deleted) l10n("Replaced the ISO with “${s.zarName}”.", "Sostituito l'ISO con “${s.zarName}”.")
            else l10n("Created “${s.zarName}”, but the ISO could not be deleted. ", "Creato “${s.zarName}”, ma non è stato possibile eliminare l'ISO. ") +
                l10n("You can delete it manually.", "Puoi eliminarlo manualmente."))
    }

    /** Keep both files. Also the dismiss path, so a stray tap outside the dialog can
     *  never delete anything. */
    fun keepIso() {
        val s = _state.value as? CompressState.ConfirmDelete ?: return
        _state.value = CompressState.Done(
            l10n("Created “${s.zarName}”. The original ISO was kept.", "Creato “${s.zarName}”. L'ISO originale è stato mantenuto."))
    }

    private suspend fun runCompress(launchUri: String): CompressState {
        EmulatorRuntime.ensureLoaded()
        val emulator = EmulatorRuntime.emulator
            ?: return CompressState.Failed(l10n("Emulator not loaded. The ISO was not modified.", "Emulatore non caricato. L'ISO non è stato modificato."))

        // Resolve-only prologue — NOTHING is mutated yet.
        val isoFile = File(launchUri)
        if (!isoFile.isFile)
            return CompressState.Failed(l10n("Couldn't open the ISO. The ISO was not modified.", "Impossibile aprire l'ISO. L'ISO non è stato modificato."))
        val parent = isoFile.parentFile?.takeIf { it.canWrite() }
            ?: return CompressState.Failed(l10n("The game folder is not writable. The ISO was not modified.", "La cartella del gioco non è scrivibile. L'ISO non è stato modificato."))

        val isoName = isoFile.name
        val baseName = isoName.removeSuffix(".iso").removeSuffix(".ISO").ifBlank { "game" }
        val zarName = "$baseName.zar"
        if (File(parent, zarName).exists())
            return CompressState.Failed(l10n("“$zarName” already exists in the folder. The ISO was not modified.", "Esiste già “$zarName” nella cartella. L'ISO non è stato modificato."))

        val tempZar = File.createTempFile("compress", ".zar", appContext.cacheDir)
        tempZar.delete()   // native creates the output; it must not pre-exist
        var placed: File? = null
        try {
            // (1) PACK + VERIFY: native packs the ISO's VFS into the temp .zar, then re-opens
            //     and verifies it. 0 == created AND verified.
            val packStatus = emulator.compressIsoToZar(isoFile.absolutePath, tempZar.absolutePath)
            if (packStatus != 0)
                return CompressState.Failed(
                    l10n("Compression failed (0x${packStatus.toUInt().toString(16)}). The ISO was not modified.", "Compressione non riuscita (0x${packStatus.toUInt().toString(16)}). L'ISO non è stato modificato."))

            // (3) PLACE: copy the verified temp .zar beside the .iso, which stays present.
            _state.value = CompressState.Busy(l10n("Replacing…", "Sostituzione…"))
            val dest = File(parent, zarName)
            val copied = runCatching { tempZar.copyTo(dest, overwrite = false) }.isSuccess
            if (!copied) {
                dest.delete(); placed = null
                return CompressState.Failed(l10n("Couldn't write the .zar file. The ISO was not modified.", "Impossibile scrivere il file .zar. L'ISO non è stato modificato."))
            }
            placed = dest
            // (3b) sanity: non-empty and its size matches the temp.
            if (dest.length() <= 0L || dest.length() != tempZar.length()) {
                dest.delete(); placed = null
                return CompressState.Failed(l10n("The written .zar file appears incomplete. The ISO was not modified.", "Il file .zar scritto sembra incompleto. L'ISO non è stato modificato."))
            }

            // (4) ASK before deleting the .iso — and only now. A verified .zar is already in
            //     the folder, so either answer leaves the game playable.
            return CompressState.ConfirmDelete(
                isoPath = isoFile.absolutePath,
                isoName = isoName,
                zarName = zarName,
                isoBytes = isoFile.length(),
            )
        } catch (e: Exception) {
            placed?.delete()   // threw AFTER placing but BEFORE deleting iso -> remove orphan .zar
            return CompressState.Failed(l10n("Compression failed: ${e.message}. The ISO was not modified.", "Compressione non riuscita: ${e.message}. L'ISO non è stato modificato."))
        } finally {
            tempZar.delete()
        }
    }
}
