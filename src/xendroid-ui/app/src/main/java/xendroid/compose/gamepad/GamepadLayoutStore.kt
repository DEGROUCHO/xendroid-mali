package xendroid.compose.gamepad

import android.content.Context
import android.util.Log
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.emitAll
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json

// Legacy location (read once to migrate). A DataStore keeps a per-process in-memory copy, so
// the library process never saw what the :emu process wrote and would overwrite it.
private val Context.gamepadStore by preferencesDataStore(name = "xendroid_gamepad")

/**
 * The touch-controls config, as one JSON file in filesDir. Both processes write it (the
 * library's editor and the in-game menu/editor), so an instance re-reads the file when the
 * other process changed it ([refresh]) and writes go through a temp file + rename; the last
 * writer wins as a whole.
 */
class GamepadLayoutStore(private val appContext: Context) {
    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }
    private val legacyKey = stringPreferencesKey("gamepad_config_v1")
    private val file = File(appContext.filesDir, "gamepad_config.json")
    private val state = MutableStateFlow<GamepadConfigDto?>(null)
    private val writeLock = Mutex()
    // File modification time behind [state]; a different value means the other process saved.
    @Volatile private var loadedStamp = Long.MIN_VALUE
    // A save not yet written: a refresh must not replace it with the older file.
    @Volatile private var unsaved: GamepadConfigDto? = null

    private fun stamp(): Long =
        if (file.isFile) file.lastModified() * 31 + file.length() else 0L

    val config: Flow<GamepadConfigDto> = flow {
        if (state.value == null) refresh()
        emitAll(state.filterNotNull())
    }

    /** Re-reads the file if the other process has saved since this instance last read or
     *  wrote it, so a long-lived instance does not show or write back a stale layout. */
    suspend fun refresh() {
        withContext(Dispatchers.IO) {
            writeLock.withLock {
                val stamp = stamp()
                if (unsaved != null || (state.value != null && stamp == loadedStamp))
                    return@withLock
                state.value = load()
                loadedStamp = stamp
            }
        }
    }

    private fun decode(text: String): GamepadConfigDto? =
        runCatching { json.decodeFromString<GamepadConfigDto>(text) }.getOrNull()

    private suspend fun load(): GamepadConfigDto {
        if (file.isFile) {
            runCatching { file.readText() }.getOrNull()?.let(::decode)?.let { return it }
        }
        // First run of this build: carry over the layout saved by the old DataStore.
        val legacy = runCatching { appContext.gamepadStore.data.first()[legacyKey] }
            .getOrNull()?.let(::decode)
        return legacy ?: GamepadConfigDto()
    }

    /** The new value is visible to collectors at once; the file follows. Writes are serialized
     *  and always store the newest value, so quick successive saves (a slider drag) cannot
     *  land out of order. */
    suspend fun save(cfg: GamepadConfigDto) {
        unsaved = cfg
        state.value = cfg
        withContext(Dispatchers.IO) {
            writeLock.withLock {
                val latest = unsaved ?: return@withLock
                runCatching {
                    val text = json.encodeToString(GamepadConfigDto.serializer(), latest)
                    val tmp = File(file.parentFile, file.name + ".tmp")
                    tmp.writeText(text)
                    if (!tmp.renameTo(file)) { file.writeText(text); tmp.delete() }
                    loadedStamp = stamp()
                }.onFailure { Log.w("GamepadStore", "saving touch controls failed", it) }
                if (unsaved === latest) unsaved = null
            }
        }
    }
}
