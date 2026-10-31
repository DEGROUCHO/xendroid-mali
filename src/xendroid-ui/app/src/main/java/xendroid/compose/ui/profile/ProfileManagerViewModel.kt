package xendroid.compose.ui.profile

import xendroid.compose.l10n

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import xendroid.compose.core.ContentPaths
import xendroid.compose.core.EmulatorRuntime
import xendroid.compose.core.Gamertag
import xendroid.compose.core.ProfilePaths
import xendroid.compose.settings.ConfigStore
import java.io.File

class ProfileManagerViewModel(
    private val appContext: Context,
    private val configStore: ConfigStore,
) : ViewModel() {

    data class ProfileEntry(
        val xuid: String,
        val gamertag: String,
        val language: Int,
        val country: Int,
        val hasAvatar: Boolean,
        val isActive: Boolean,
    )

    sealed interface ListState {
        data object Loading : ListState
        data class Loaded(val profiles: List<ProfileEntry>) : ListState
        data class Error(val message: String) : ListState
    }

    private val _listState = MutableStateFlow<ListState>(ListState.Loading)
    val listState: StateFlow<ListState> = _listState.asStateFlow()

    sealed interface OpState {
        data object Idle : OpState
        data class Busy(val message: String) : OpState
        data class Done(val message: String) : OpState
        data class Failed(val message: String) : OpState
    }

    private val _opState = MutableStateFlow<OpState>(OpState.Idle)
    val opState: StateFlow<OpState> = _opState.asStateFlow()

    init { refresh() }

    fun dismiss() { _opState.value = OpState.Idle }

    fun refresh() = viewModelScope.launch {
        _listState.value = ListState.Loading
        _listState.value = withContext(Dispatchers.IO) {
            EmulatorRuntime.ensureLoaded()
            val emu = EmulatorRuntime.emulator
                ?: return@withContext ListState.Error(l10n("Emulator not loaded.", "Emulatore non caricato."))
            val root = ContentPaths.contentRoot().absolutePath
            try {
                val active = activeXuid()
                val profiles = emu.list_profiles(root)?.map {
                    ProfileEntry(
                        xuid = it.xuid,
                        gamertag = it.gamertag ?: "",
                        language = it.language,
                        country = it.country,
                        hasAvatar = it.hasAvatar,
                        isActive = it.xuid.equals(active, ignoreCase = true),
                    )
                }?.sortedBy { it.gamertag.lowercase() }
                    ?: return@withContext ListState.Error(l10n("Couldn't read profiles.", "Impossibile leggere i profili."))
                ListState.Loaded(profiles)
            } catch (t: RuntimeException) {
                ListState.Error(t.message ?: l10n("Couldn't read profiles.", "Impossibile leggere i profili."))
            }
        }
    }

    fun create(gamertag: String, language: Int, country: Int, avatarUri: Uri?) = viewModelScope.launch {
        if (!Gamertag.isValid(gamertag)) {
            _opState.value = OpState.Failed(l10n("Enter a valid gamertag (1-15 characters).", "Inserisci un gamertag valido (1-15 caratteri)."))
            return@launch
        }
        _opState.value = OpState.Busy(l10n("Creating profile…", "Creazione profilo…"))
        val result = withContext(Dispatchers.IO) {
            EmulatorRuntime.ensureLoaded()
            val emu = EmulatorRuntime.emulator ?: return@withContext null
            val xuid = emu.create_profile(
                ContentPaths.contentRoot().absolutePath, gamertag, language, country)
            if (xuid != null && avatarUri != null) writeAvatar(xuid, avatarUri)
            xuid
        }
        if (result != null) {
            _opState.value = OpState.Done(l10n("Created “$gamertag”.", "Creato “$gamertag”."))
            refresh()
        } else {
            _opState.value = OpState.Failed(l10n("Couldn't create profile.", "Impossibile creare il profilo."))
        }
    }

    fun rename(xuid: String, gamertag: String, language: Int, country: Int, avatarUri: Uri?) =
        viewModelScope.launch {
            if (!Gamertag.isValid(gamertag)) {
                _opState.value = OpState.Failed(l10n("Enter a valid gamertag (1-15 characters).", "Inserisci un gamertag valido (1-15 caratteri)."))
                return@launch
            }
            _opState.value = OpState.Busy(l10n("Saving…", "Salvataggio…"))
            val status = withContext(Dispatchers.IO) {
                EmulatorRuntime.ensureLoaded()
                val emu = EmulatorRuntime.emulator ?: return@withContext -1
                val st = emu.rename_profile(
                    ContentPaths.contentRoot().absolutePath, xuid, gamertag, language, country)
                if (st == 0 && avatarUri != null) writeAvatar(xuid, avatarUri)
                st
            }
            if (status == 0) {
                _opState.value = OpState.Done(l10n("Saved “$gamertag”.", "Salvato “$gamertag”."))
                refresh()
            } else {
                _opState.value = OpState.Failed(renameReasonFor(status))
            }
        }

    fun setActive(xuid: String) = viewModelScope.launch {
        withContext(Dispatchers.IO) { writeActiveXuid(xuid.uppercase()) }
        _opState.value = OpState.Done(l10n("Active profile set. It will apply on the next game launch.", "Profilo attivo impostato. Verrà applicato al prossimo avvio del gioco."))
        refresh()
    }

    fun delete(xuid: String) = viewModelScope.launch {
        _opState.value = OpState.Busy(l10n("Removing…", "Rimozione…"))
        val ok = withContext(Dispatchers.IO) {
            val id = xuid.uppercase()
            if (!ProfilePaths.XUID_REGEX.matches(id)) return@withContext false
            val dir = File(ContentPaths.contentRoot(), id)
            val removed = dir.deleteRecursively()
            if (activeXuid().equals(id, ignoreCase = true)) writeActiveXuid("")
            removed
        }
        if (ok) {
            _opState.value = OpState.Done(l10n("Profile removed.", "Profilo rimosso."))
            refresh()
        } else {
            _opState.value = OpState.Failed(l10n("Couldn't remove profile.", "Impossibile rimuovere il profilo."))
        }
    }

    private fun activeXuid(): String {
        val h = configStore.openLiveSnapshot()
        return try {
            h.getString("Profiles", "logged_profile_slot_0_xuid") ?: ""
        } finally {
            h.closeString()
        }
    }

    private fun writeActiveXuid(xuid: String) {
        val h = configStore.openLive()
        try {
            h.putString("Profiles", "logged_profile_slot_0_xuid", xuid)
        } finally {
            h.closeFile()
        }
    }

    private fun writeAvatar(xuid: String, uri: Uri) {
        val src = appContext.contentResolver.openInputStream(uri)?.use {
            BitmapFactory.decodeStream(it)
        } ?: return
        val square = centerCropSquare(src)
        val dir = ProfilePaths.profileDir(xuid).also { it.mkdirs() }
        writePng(square, 64, File(dir, "tile_64.png"))
        writePng(square, 32, File(dir, "tile_32.png"))
        if (square != src) square.recycle()
        src.recycle()
    }

    private fun centerCropSquare(bmp: Bitmap): Bitmap {
        val side = minOf(bmp.width, bmp.height)
        if (side == bmp.width && side == bmp.height) return bmp
        val x = (bmp.width - side) / 2
        val y = (bmp.height - side) / 2
        return Bitmap.createBitmap(bmp, x, y, side, side)
    }

    private fun writePng(square: Bitmap, size: Int, dest: File) {
        val scaled = Bitmap.createScaledBitmap(square, size, size, true)
        dest.outputStream().use { scaled.compress(Bitmap.CompressFormat.PNG, 100, it) }
        if (scaled != square) scaled.recycle()
    }

    private fun renameReasonFor(status: Int): String = when (status) {
        -1 -> l10n("Emulator not loaded.", "Emulatore non caricato.")
        0xC000000D.toInt() -> l10n("Enter a valid gamertag (1-15 characters).", "Inserisci un gamertag valido (1-15 caratteri).")
        0xC0000034.toInt() -> l10n("This profile no longer exists.", "Questo profilo non esiste più.")
        0xC0000022.toInt() -> l10n("Couldn't write profile files.", "Impossibile scrivere i file del profilo.")
        else -> l10n("Save failed (0x${status.toUInt().toString(16)}).", "Salvataggio non riuscito (0x${status.toUInt().toString(16)}).")
    }
}
