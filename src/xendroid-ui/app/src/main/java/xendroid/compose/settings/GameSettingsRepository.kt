package xendroid.compose.settings

import xendroid.compose.l10n

import android.util.Log
import java.io.File

/** Legacy pure rebuild-plan helper retained for callers and unit tests. */
sealed interface RebuildPlan {
    data object Delete : RebuildPlan
    data class Write(val keys: Set<String>) : RebuildPlan
}

fun rebuildPlan(overrides: Map<String, String>): RebuildPlan =
    if (overrides.isEmpty()) RebuildPlan.Delete else RebuildPlan.Write(overrides.keys.toSet())

/**
 * Sparse per-game overrides over the global configuration. Flush removes and
 * re-emits schema-known overrides, retaining unknown backend settings from disk.
 * Un-overriding a known setting removes that key so the global value is inherited.
 */
class GameSettingsRepository(private val store: ConfigStore, private val titleId: String) {

    /** key -> override raw String (only the keys the user explicitly set for THIS game). */
    private val overrides = mutableMapOf<String, String>()
    /** key -> effective global value (the inherited value), captured once at open. */
    private var globalValues: Map<String, String?> = emptyMap()
    private var opened = false

    val isCustomDriverSupported: Boolean
        get() = runCatching { File("/dev/kgsl-3d0").exists() }.getOrDefault(false)

    @Synchronized
    fun open() {
        if (opened) return
        // Known overrides are editable; unknown entries are preserved at flush.
        runCatching {
            val h = store.openGameConfig(titleId)
            try {
                SettingsSchema.allSettings.forEach { s ->
                    h.getString(s.section, s.name)?.let { overrides[s.key] = it }
                }
            } finally {
                h.closeDiscard()  // read-only pass; skip the pointless re-serialize + write
            }
        }.onFailure { Log.w("GameSettingsRepo", "per-game open failed ($titleId)", it) }
        // 2) snapshot the global effective values (the INHERITED display). Must not throw -- if it
        //    did, globalValues would stay empty and every row would show the schema default instead
        //    of the real global value.
        runCatching {
            val snap = store.openLiveSnapshot()
            globalValues = SettingsSchema.allSettings.associate { s ->
                s.key to snap.getString(s.section, s.name)
            }
            snap.closeDiscard()
        }.onFailure { Log.w("GameSettingsRepo", "global snapshot failed", it) }
        opened = true
    }

    @Synchronized
    fun ensureOpen() { if (!opened) open() }

    fun isOverridden(s: Setting): Boolean = overrides.containsKey(s.key)

    /** Raw override value for [s] if overridden, else null. Lets the per-game observable carry the
     *  value so a value-only edit changes the snapshot (containsKey still means "overridden"). */
    fun rawOverride(s: Setting): String? = overrides[s.key]

    private fun inheritedRaw(s: Setting): String =
        globalValues[s.key] ?: schemaDefaultString(s)

    // ---- typed reads: override value if present, else inherited ----
    fun boolOf(s: Setting.Bool): Boolean =
        ConfigValueShape.parseBool(overrides[s.key] ?: inheritedRaw(s), s.default)
    fun intOf(s: Setting.IntRange): Int =
        ConfigValueShape.parseInt(overrides[s.key] ?: inheritedRaw(s), s.default)
    fun listValueOf(s: Setting.ListChoice): String = overrides[s.key] ?: inheritedRaw(s)
    fun driverPathOf(s: Setting.Action): String = overrides[s.key] ?: ""
    fun inheritedLabel(s: Setting): String = labelFor(s, inheritedRaw(s))

    // ---- writes: mutate the in-memory map only; persisted on flush ----
    fun setBool(s: Setting.Bool, v: Boolean) { overrides[s.key] = ConfigValueShape.bool(v) }
    fun setInt(s: Setting.IntRange, v: Int) {
        overrides[s.key] = ConfigValueShape.int(v.coerceIn(s.min, s.max))
    }
    fun setListValue(s: Setting.ListChoice, value: String) { overrides[s.key] = value }
    fun setDriverPath(s: Setting.Action, value: String) { overrides[s.key] = value }

    /** Turn override ON: seed from the current inherited value. OFF: drop the key. */
    fun setOverride(s: Setting, on: Boolean) {
        if (on) {
            if (!overrides.containsKey(s.key)) overrides[s.key] = inheritedRaw(s)
        } else {
            overrides.remove(s.key)
        }
    }

    /** One-tap preset: set every key that the schema knows (unknown keys are ignored). */
    fun applyPreset(values: Map<String, String>) {
        values.forEach { (key, raw) ->
            if (SettingsSchema.byKey.containsKey(key)) overrides[key] = raw
        }
    }

    /** Replace known overrides while retaining unrelated backend settings. */
    @Synchronized
    fun flush() {
        if (!opened) return
        val file = store.perGameConfigFile(titleId)
        // Preserve keys outside the frontend schema, including new backend cvars.
        // Re-read at flush so externally added unknown keys are retained too.
        val h = ConfigHandle.openString(if (file.exists()) file.readText() else "")
        val serialized = try {
            SettingsSchema.allSettings.forEach { s ->
                h.remove(s.section, s.name)
                overrides[s.key]?.let { raw -> putTyped(h, s, raw) }
            }
            h.closeString()
        } finally {
            h.closeDiscard()
        }
        file.parentFile?.mkdirs()
        val pending = File(file.parentFile, file.name + ".pending")
        pending.writeText(serialized)
        check(pending.renameTo(file)) { "Could not replace per-game config: $titleId" }
    }

    private fun putTyped(h: ConfigHandle, s: Setting, raw: String) = when (s) {
        is Setting.Bool       -> h.putBool(s.section, s.name, ConfigValueShape.parseBool(raw, s.default))
        is Setting.IntRange   -> h.putInt(s.section, s.name, ConfigValueShape.parseInt(raw, s.default))
        is Setting.ListChoice -> h.putString(s.section, s.name, raw)
        is Setting.Action     -> h.putString(s.section, s.name, raw)
    }

    private fun schemaDefaultString(s: Setting): String = when (s) {
        is Setting.Bool       -> if (s.default) "true" else "false"
        is Setting.IntRange   -> s.default.toString()
        is Setting.ListChoice -> s.default
        is Setting.Action     -> s.default
    }

    private fun labelFor(s: Setting, raw: String): String = when (s) {
        is Setting.ListChoice -> s.options.firstOrNull { it.value == raw }?.label
            ?: raw.ifEmpty { l10n("(default)", "(predefinito)") }
        else -> raw
    }
}
