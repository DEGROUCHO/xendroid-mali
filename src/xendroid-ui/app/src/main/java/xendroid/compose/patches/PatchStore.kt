package xendroid.compose.patches

import java.io.File

/**
 * Reads effective patch state and writes per-patch toggles. Effective state = the on-disk file
 * in [patchesDir] if present, else the bundled asset (which ships all-disabled). A toggle copies
 * the asset to disk on first edit, then flips one `is_enabled` line; the emulator reads
 * [patchesDir] on the next launch.
 */
class PatchStore(
    private val assets: PatchAssets,
    private val patchesDir: File,
) {
    /**
     * Files whose 8-hex filename prefix matches [titleId] (case-insensitive). A title may have several.
     * Bundled assets plus files only on disk (imported by the user).
     */
    fun patchesForTitle(titleId: String): List<PatchFile> =
        (assets.list() + diskFiles())
            .distinct()
            .filter { it.length >= 8 && it.substring(0, 8).equals(titleId, ignoreCase = true) }
            .sorted()
            .mapNotNull { name -> PatchTomlParser.parse(name, effectiveText(name)) }

    /**
     * Saves a user-picked .patch.toml into [patchesDir] under the name the emulator scans for
     * (`<TITLEID> - <name>.patch.toml`, see patch_db.h). Returns the parsed file, or null when the
     * text is not a patch file (no title_id). A file with the same name replaces the old one.
     */
    fun import(pickedName: String, text: String): PatchFile? {
        val probe = PatchTomlParser.parse(pickedName, text) ?: return null
        if (!Regex("[A-Fa-f0-9]{8}").matches(probe.titleId) || probe.entries.isEmpty()) return null
        val base = pickedName.substringAfterLast('/').removeSuffix(".toml").removeSuffix(".patch")
            .replace(Regex("[\\\\/:*?\"<>|]"), "_").trim()
        val name = if (base.length >= 8 && base.substring(0, 8).equals(probe.titleId, ignoreCase = true)) {
            "$base.patch.toml"
        } else {
            "${probe.titleId.uppercase()} - ${base.ifEmpty { probe.titleName.ifEmpty { "Imported" } }}.patch.toml"
        }
        patchesDir.mkdirs()
        File(patchesDir, name).writeText(text)
        return PatchTomlParser.parse(name, text)
    }

    private fun diskFiles(): List<String> =
        patchesDir.list()?.filter { it.endsWith(".patch.toml") }.orEmpty()

    /** Toggle a single `[[patch]]` entry by its 0-based ordinal. */
    fun setEnabled(fileName: String, patchIndex: Int, enabled: Boolean) {
        patchesDir.mkdirs()
        val onDisk = File(patchesDir, fileName)
        val current = if (onDisk.exists()) onDisk.readText() else assets.read(fileName)
        onDisk.writeText(PatchTomlEditor.setEnabled(current, patchIndex, enabled))
    }

    private fun effectiveText(fileName: String): String {
        val onDisk = File(patchesDir, fileName)
        return if (onDisk.exists()) onDisk.readText() else assets.read(fileName)
    }
}
