package xendroid.compose.data

import android.content.Context
import android.util.Log
import android.os.SystemClock
import xendroid.compose.core.ContentPaths
import xendroid.compose.core.GameMetadataSource
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File

/** How deep the walk goes below the games folder. A game sits at most a few levels down
 *  (folder / game / disc, or a GOD layout title / 00007000 / container); deeper trees are
 *  other apps' data, and walking them made every scan take tens of seconds. */
private const val MAX_SCAN_DEPTH = 5

/** A folder with more entries than this is never a game's: it is listed (for default.xex)
 *  but not walked. */
private const val MAX_WALKED_DIR_ENTRIES = 2000

/** Discovers games by walking the real-path games dir and every subdirectory beneath it. */
class GameLibraryRepository(
    private val appContext: Context,
    private val prefs: PreferencesStore,
    private val metadata: GameMetadataSource,
    private val iconCache: IconCache,
    private val metadataCache: GameMetadataCache,
) {
    private val tag = "GameLibraryRepo"

    /** Serializes scans so two overlapping refresh()es don't both cold-extract and race on
     *  the cache file; the second runs cheap against the now-warm cache. */
    private val scanMutex = Mutex()
    // Only accessed while scanMutex is held.
    private var cacheHits = 0
    private var cacheMisses = 0
    private var xexExtractions = 0
    private var xexExtractionMs = 0L

    /** An add-on package (DLC, title update, save) found in the games folder. */
    data class AddOn(val path: String, val titleId: String, val contentType: Int, val name: String)

    // Add-ons the last full scan found; replaced as a whole when a scan ends. Only the scan
    // (under scanMutex) writes scanAddOns.
    @Volatile private var foundAddOns: List<AddOn> = emptyList()
    private val scanAddOns = ArrayList<AddOn>()

    /** Add-on packages for [titleId] that the last scan found in the games folder. */
    fun addOnsFor(titleId: String): List<AddOn> =
        foundAddOns.filter { it.titleId.equals(titleId, ignoreCase = true) }

    sealed interface ScanResult {
        data object NoFolder : ScanResult
        data object PermissionLost : ScanResult
        data class Games(val games: List<Game>) : ScanResult
    }

    /** Validates the path is a readable directory FIRST so a later scan can't fail
     *  PermissionLost on an unreadable dir. Returns whether it was saved. */
    suspend fun saveGameDirPath(path: String): Boolean = withContext(Dispatchers.IO) {
        val dir = File(path)
        if (!dir.isDirectory || dir.listFiles() == null) {
            Log.w(tag, "saveGameDirPath rejected (not a readable dir): $path")
            return@withContext false
        }
        prefs.setGameDirPath(path)
        true
    }

    suspend fun currentGameDirPath(): String? = withContext(Dispatchers.IO) {
        prefs.gameDirPath.firstOrNull()
    }

    suspend fun scan(): ScanResult = withContext(Dispatchers.IO) {
        val queuedAt = SystemClock.elapsedRealtime()
        scanMutex.withLock {
            val startedAt = SystemClock.elapsedRealtime()
            cacheHits = 0
            cacheMisses = 0
            xexExtractions = 0
            xexExtractionMs = 0L
            try {
                scanLocked()
            } finally {
                Log.i(tag, "scan_ms=${SystemClock.elapsedRealtime() - startedAt} " +
                    "queue_ms=${startedAt - queuedAt} cache_hits=$cacheHits " +
                    "cache_misses=$cacheMisses xex_extractions=$xexExtractions " +
                    "xex_extraction_ms=$xexExtractionMs")
            }
        }
    }

    /** No JNI or recursive traversal: only reuse unchanged files under the current root. */
    suspend fun cachedGames(): ScanResult? = withContext(Dispatchers.IO) {
        scanMutex.withLock {
            val dir = prefs.gameDirPath.firstOrNull()
                ?: return@withLock ScanResult.NoFolder
            val root = File(dir)
            if (root.listFiles() == null) return@withLock ScanResult.PermissionLost
            metadataCache.load()
            val games = CachedLibrary.games(root, metadataCache.entriesSnapshot()) {
                iconCache.fileFor(it).exists()
            }
            games.takeIf { it.isNotEmpty() }?.let { ScanResult.Games(it) }
        }
    }

    private suspend fun scanLocked(): ScanResult {
        val dir = prefs.gameDirPath.firstOrNull() ?: return ScanResult.NoFolder
        return scanRealPathLocked(dir)
    }

    /** Resolve a game's title id for the per-game config path (boot-free).
     *  MUST run off the main thread: these mmap the file. */
    suspend fun readTitleId(ctx: Context, game: Game): String? = withContext(Dispatchers.IO) {
        game.titleId?.takeIf { it.isNotBlank() && it != "00000000" }?.let { return@withContext it }
        when (game.format) {
            GameFormat.GOD -> metadata.readGodPath(game.launchUri)?.titleId
            GameFormat.STFS -> metadata.readContentHeader(game.launchUri)?.titleId
            GameFormat.ISO, GameFormat.XEX_FOLDER, GameFormat.ZAR ->
                metadata.readTitleIdPath(game.launchUri, game.format)
        }
    }

    private fun signatureOf(file: File): GameMetadataCache.Signature =
        GameMetadataCache.Signature(sizeBytes = file.length(), lastModified = file.lastModified())

    /** Cache lookup for the extracting branches: a fresh Hit (signature matches + icon
     *  File survives), else null so the caller extracts. */
    private fun metadataCacheHit(
        launchUri: String,
        signature: GameMetadataCache.Signature,
    ): GameMetadataCache.Decision.Hit? {
        val decision = GameMetadataCache.decide(
            cached = metadataCache.get(launchUri),
            signature = signature,
            iconFileExists = { iconCache.fileFor(it).exists() },
            // A cached failed extraction is retried, or the game keeps no box art for good.
            requireTitleId = true,
        )
        if (decision is GameMetadataCache.Decision.Hit) cacheHits++ else cacheMisses++
        return decision as? GameMetadataCache.Decision.Hit
    }

    private data class Extracted(
        val name: String,
        val iconCacheName: String?,
        val titleId: String?,
        val mediaId: String?,
        val discNumber: Int = 0,
        val discCount: Int = 0,
    )

    /** Cache wrapper for the always-producing extracting branches (ISO, XEX_FOLDER, ZAR):
     *  reuse a fresh hit, else run [extract], cache it, and build the Game. */
    private inline fun cachedOrExtract(
        launchUri: String,
        signature: GameMetadataCache.Signature,
        format: GameFormat,
        extract: () -> Extracted,
    ): Game {
        metadataCacheHit(launchUri, signature)?.let {
            return Game(launchUri, it.name, format, it.iconCacheName, it.titleId, it.mediaId,
                        it.discNumber, it.discCount)
        }
        val extractionStart = SystemClock.elapsedRealtime()
        xexExtractions++
        val e = try {
            extract()
        } finally {
            val elapsedMs = SystemClock.elapsedRealtime() - extractionStart
            xexExtractionMs += elapsedMs
            Log.i(tag, "xex_metadata format=$format elapsed_ms=$elapsedMs")
        }
        metadataCache.put(launchUri, e.name, e.iconCacheName, signature, e.titleId, e.mediaId,
                          format, e.discNumber, e.discCount)
        return Game(launchUri, e.name, format, e.iconCacheName, e.titleId, e.mediaId,
                    e.discNumber, e.discCount)
    }

    /** Walk the games dir and every subdirectory, classify each entry, sort. A null listing
     *  on the ROOT (grant revoked) -> [ScanResult.PermissionLost]; an unreadable subdirectory
     *  is skipped. */
    private fun scanRealPathLocked(dirPath: String): ScanResult {
        val t0 = SystemClock.elapsedRealtime()
        val root = File(dirPath)
        val children = root.listFiles() ?: return ScanResult.PermissionLost

        // Load the extraction cache once; mutate during classify; persist once after.
        metadataCache.load()
        val t1 = SystemClock.elapsedRealtime()
        val games = ArrayList<Game>()
        val visited = HashSet<String>()
        canonicalOf(root)?.let { visited.add(it) }
        scanAddOns.clear()
        collectGames(children, depth = 1, visited, games)
        foundAddOns = scanAddOns.toList()
        val t2 = SystemClock.elapsedRealtime()
        games.sortBy { it.name.lowercase() }
        metadataCache.retainOnly(games.mapTo(HashSet()) { it.launchUri })
        metadataCache.save()
        Log.i(tag, "scan_stages list_and_load_ms=${t1 - t0} walk_ms=${t2 - t1} " +
            "save_ms=${SystemClock.elapsedRealtime() - t2}")
        return ScanResult.Games(games)
    }

    /** Classify [children], then descend into every subdirectory that is not itself a game.
     *  [visited] holds canonical paths so a symlink loop cannot spin forever. */
    private fun collectGames(
        children: Array<File>,
        depth: Int,
        visited: MutableSet<String>,
        out: MutableList<Game>,
    ) {
        for (child in children) {
            if (!child.isDirectory) {
                val startedAt = SystemClock.elapsedRealtime()
                classifyFile(child)?.let { out.add(it) }
                // Which kind of entry makes a scan slow (no names: the folder is the player's).
                val elapsedMs = SystemClock.elapsedRealtime() - startedAt
                if (elapsedMs > 250) {
                    Log.i(tag, "slow_entry ms=$elapsedMs format=${GameFormat.fromFileName(child.name)} " +
                        "bytes=${child.length()} depth=$depth")
                }
                continue
            }
            if (!isScannableDir(child)) continue
            val listedAt = SystemClock.elapsedRealtime()
            val entries = child.listFiles() ?: continue
            val listMs = SystemClock.elapsedRealtime() - listedAt
            if (listMs > 250) Log.i(tag, "slow_dir list_ms=$listMs entries=${entries.size} depth=$depth")
            // A folder holding default.xex IS the game; its own subtree is game data.
            val xex = entries.firstOrNull {
                it.isFile && it.name.equals("default.xex", ignoreCase = true)
            }
            if (xex != null) {
                out.add(xexFolderGame(child, xex))
                continue
            }
            if (depth >= MAX_SCAN_DEPTH) continue
            if (entries.size > MAX_WALKED_DIR_ENTRIES) {
                Log.i(tag, "not walking a folder of ${entries.size} entries at depth $depth")
                continue
            }
            val canonical = canonicalOf(child) ?: continue
            if (!visited.add(canonical)) continue
            collectGames(entries, depth + 1, visited, out)
        }
    }

    /** Skips hidden dirs and GOD "<container>.data" payload dirs, whose extensionless
     *  Data#### files would each be probed as a container. */
    private fun isScannableDir(dir: File): Boolean =
        !dir.name.startsWith(".") && !dir.name.endsWith(".data", ignoreCase = true)

    private fun canonicalOf(file: File): String? = runCatching { file.canonicalPath }.getOrNull()

    /** XEX folder: launch the default.xex child, fall back to the folder name. Signature is
     *  off default.xex, whose bytes are what extraction reads. */
    private fun xexFolderGame(dir: File, xex: File): Game {
        val xexPath = xex.absolutePath
        return cachedOrExtract(xexPath, signatureOf(xex), GameFormat.XEX_FOLDER) {
            val meta = metadata.readXexMetaPath(xexPath, GameFormat.XEX_FOLDER)
            val displayName = meta?.name?.takeIf { it.isNotEmpty() } ?: dir.name
            val iconName = meta?.iconPng?.let { iconCache.write(xexPath, it) }
            val titleId = meta?.titleId ?: metadata.readTitleIdPath(xexPath, GameFormat.XEX_FOLDER)
            Extracted(displayName, iconName, titleId, meta?.mediaId,
                      meta?.discNumber ?: 0, meta?.discCount ?: 0)
        }
    }

    /** One file -> a Game, or null if ignored/unparseable. */
    private fun classifyFile(child: File): Game? {
        val name = child.name
        return when (GameFormat.fromFileName(name)) {
            GameFormat.ISO, GameFormat.ZAR -> {
                val fmt = GameFormat.fromFileName(name)!!
                val path = child.absolutePath
                cachedOrExtract(path, signatureOf(child), fmt) {
                    val meta = metadata.readXexMetaPath(path, fmt)
                    val displayName = meta?.name?.takeIf { it.isNotEmpty() }
                        ?: fmt.displayNameFor(name)
                    val iconName = meta?.iconPng?.let { iconCache.write(path, it) }
                    // The lighter title-id read still finds the box art when the full one fails.
                    val titleId = meta?.titleId ?: metadata.readTitleIdPath(path, fmt)
                    Extracted(displayName, iconName, titleId, meta?.mediaId,
                          meta?.discNumber ?: 0, meta?.discCount ?: 0)
                }
            }
            GameFormat.GOD -> classifyExtensionless(child, name)
            // Title updates come named like "TU_xxx.0000000000084". Only such names are
            // opened: reading every pdf and photo of a Download folder took 12 s per scan.
            null -> {
                if (looksLikePackageName(name) && hasContentMagic(child)) {
                    metadata.readContentHeader(child.absolutePath)?.let { noteAddOn(child.absolutePath, name, it) }
                }
                null
            }
            GameFormat.STFS, GameFormat.XEX_FOLDER -> null
        }
    }

    /** Players put DLC and title updates next to the game: remember them so launching the
     *  game can offer to install them. */
    private fun noteAddOn(path: String, name: String, header: GameMetadataSource.ContentMeta) {
        val titleId = header.titleId ?: return
        if (!ContentPaths.isAddOnType(header.contentType)) return
        scanAddOns.add(AddOn(path, titleId, header.contentType, header.displayName.ifBlank { name }))
    }

    /** A title update's name ("TU_..."), or an all-hex extension as packages carry. */
    private fun looksLikePackageName(name: String): Boolean {
        if (name.startsWith("TU", ignoreCase = true)) return true
        val ext = name.substringAfterLast('.')
        return ext.isNotEmpty() && ext.all { it.isDigit() || it in 'a'..'f' || it in 'A'..'F' }
    }

    /** CON / LIVE / PIRS: the first four bytes of every Xbox 360 content package. */
    private fun hasContentMagic(file: File): Boolean = runCatching {
        val magic = ByteArray(4)
        file.inputStream().use { if (it.read(magic) != 4) return@runCatching false }
        String(magic, Charsets.US_ASCII) in setOf("CON ", "LIVE", "PIRS")
    }.getOrDefault(false)

    /** An extensionless file: a GOD container, an STFS launchable-game container, or neither.
     *  Distinguishes "not a game" (null) from a cache miss, and carries the format through the
     *  cache so a hit rebuilds the right Game. GOD MUST be probed before STFS (see below). */
    private fun classifyExtensionless(child: File, name: String): Game? {
        val path = child.absolutePath
        metadataCacheHit(path, signatureOf(child))?.let { hit ->
            val fmt = hit.format ?: GameFormat.GOD   // legacy entries predate STFS -> GOD
            return Game(path, hit.name, fmt, hit.iconCacheName, hit.titleId, hit.mediaId,
                        hit.discNumber, hit.discCount)
        }
        // The type gate runs before the GOD probe, not just in the STFS branch below: add-on
        // content (DLC, title updates, profiles, saves) is an STFS container too, so the GOD
        // reader parses it happily and would publish it as a game. An unreadable header is not
        // a rejection - it only means the type is unknown, so those still fall through.
        val header = metadata.readContentHeader(path)
        if (header != null && !ContentPaths.isLaunchableGameType(header.contentType)) {
            noteAddOn(path, name, header)
            return null
        }
        metadata.readGodPath(path)?.let { meta ->
            val displayName = meta.name.ifEmpty { name }
            val iconName = meta.iconPng?.let { iconCache.write(path, it) }
            metadataCache.put(path, displayName, iconName, signatureOf(child), meta.titleId, meta.mediaId,
                              GameFormat.GOD, meta.discNumber, meta.discCount)
            return Game(path, displayName, GameFormat.GOD, iconName, meta.titleId, meta.mediaId,
                        meta.discNumber, meta.discCount)
        }
        // GOD is probed first (above) because a GOD container also parses as a content package.
        if (header == null) return null
        val displayName = header.displayName.ifBlank { name }
        val iconName = header.iconPng?.let { iconCache.write(path, it) }
        metadataCache.put(path, displayName, iconName, signatureOf(child), header.titleId, null, GameFormat.STFS)
        return Game(path, displayName, GameFormat.STFS, iconName, header.titleId, mediaId = null)
    }
}
