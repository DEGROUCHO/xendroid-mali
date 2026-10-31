package xendroid.compose.data

import java.io.File

/** Pure filesystem/cache policy; never extracts metadata or changes the scan cache. */
object CachedLibrary {
    fun games(
        root: File,
        entries: Map<String, GameMetadataCache.Entry>,
        iconFileExists: (String) -> Boolean,
    ): List<Game> {
        val canonicalRoot = runCatching { root.canonicalFile }.getOrNull() ?: return emptyList()
        return entries.mapNotNull { (path, entry) ->
            val file = File(path)
            val canonical = runCatching { file.canonicalFile }.getOrNull() ?: return@mapNotNull null
            // Canonical containment excludes stale other-folder entries and symlink escapes.
            if (!canonical.toPath().startsWith(canonicalRoot.toPath()) ||
                !file.isFile || !file.canRead()) return@mapNotNull null
            val format = entry.format ?: return@mapNotNull null
            val hit = GameMetadataCache.decide(entry,
                GameMetadataCache.Signature(file.length(), file.lastModified()),
                iconFileExists) as? GameMetadataCache.Decision.Hit ?: return@mapNotNull null
            Game(path, hit.name, format, hit.iconCacheName, hit.titleId, hit.mediaId,
                hit.discNumber, hit.discCount)
        }.sortedBy { it.name.lowercase() }
    }
}
