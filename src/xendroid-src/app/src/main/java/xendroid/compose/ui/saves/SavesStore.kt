package xendroid.compose.ui.saves

import android.content.Context
import android.os.Environment
import xendroid.compose.Application
import java.io.File

data class SaveEntry(
    val titleId: String,
    val dir: File,
    val displayName: String,
    val modifiedAt: Long,
    val sizeBytes: Long,
)

object SavesStore {

    private const val SAVED_GAME_CONTENT_TYPE = "00000001"
    private const val EXPORT_ROOT_NAME = "XenDroid-Saves"
    private const val MAX_NAME_LENGTH = 80
    private const val FALLBACK_NAME = "save"

    private fun contentRoot(context: Context): File {
        val composeDir = runCatching { Application.get_global_config_file().parentFile }.getOrNull()
            ?: context.getExternalFilesDir("compose")
            ?: return File("")
        return File(composeDir, "content")
    }

    fun list(context: Context, titleId: String): List<SaveEntry> {
        val root = contentRoot(context)
        val xuidDirs = root.listFiles { file -> file.isDirectory } ?: return emptyList()
        val normalizedTitleId = titleId.uppercase()
        val result = ArrayList<SaveEntry>()

        for (xuidDir in xuidDirs) {
            val savedGamesDir = File(
                File(xuidDir, normalizedTitleId),
                SAVED_GAME_CONTENT_TYPE,
            )
            val saveDirs = savedGamesDir.listFiles { file -> file.isDirectory } ?: continue
            for (saveDir in saveDirs) {
                if (saveDir.listFiles().isNullOrEmpty()) continue
                result += SaveEntry(
                    titleId = normalizedTitleId,
                    dir = saveDir,
                    displayName = saveDir.name,
                    modifiedAt = saveDir.lastModified(),
                    sizeBytes = computeSize(saveDir),
                )
            }
        }

        return result.sortedByDescending { it.modifiedAt }
    }

    fun export(context: Context, entry: SaveEntry, gameName: String): File {
        val downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
        val destination = File(
            File(File(downloads, EXPORT_ROOT_NAME), sanitize(gameName)),
            sanitize(entry.displayName),
        )
        copyRecursively(entry.dir, destination)
        return destination
    }

    fun delete(entry: SaveEntry): Boolean {
        entry.dir.deleteRecursively()
        return !entry.dir.exists()
    }

    private fun computeSize(dir: File): Long {
        val children = dir.listFiles() ?: return 0L
        var total = 0L
        for (child in children) {
            total += if (child.isDirectory) computeSize(child) else child.length()
        }
        return total
    }

    private fun copyRecursively(source: File, destination: File) {
        if (source.isDirectory) {
            if (!destination.exists()) destination.mkdirs()
            val children = source.listFiles() ?: return
            for (child in children) {
                copyRecursively(child, File(destination, child.name))
            }
        } else {
            destination.parentFile?.mkdirs()
            source.inputStream().use { input ->
                destination.outputStream().use { output -> input.copyTo(output) }
            }
        }
    }

    private fun sanitize(rawName: String): String {
        val builder = StringBuilder()
        for (ch in rawName) {
            val allowed = ch.isLetterOrDigit() || ch == ' ' || ch == '-' || ch == '_' ||
                ch == '(' || ch == ')' || ch == '[' || ch == ']'
            builder.append(if (allowed) ch else '_')
        }
        var name = builder.toString().trim().trimEnd('.', ' ')
        if (name.length > MAX_NAME_LENGTH) {
            name = name.substring(0, MAX_NAME_LENGTH).trimEnd('.', ' ')
        }
        if (name.isEmpty() || name.all { it == '.' }) name = FALLBACK_NAME
        return name
    }
}
