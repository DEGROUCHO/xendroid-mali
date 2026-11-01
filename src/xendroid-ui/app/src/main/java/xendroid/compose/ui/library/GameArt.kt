package xendroid.compose.ui.library

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.mutableIntStateOf
import coil.network.HttpException
import coil.request.CachePolicy
import coil.request.ImageRequest
import coil.size.Precision
import java.util.concurrent.ConcurrentHashMap

/**
 * Xbox 360 Marketplace artwork by title id, from the open x360db archive
 * (github.com/xenia-manager/x360db, ~6500 games): box art 219x300 for nearly every game, a
 * 1280x720 background for less than half of them. Coil's disk cache keeps every image after
 * its first download. Where a picture is missing the callers fall back (background -> box art
 * -> the icon embedded in the game), so every game gets the same kind of screen.
 */
internal object GameArt {
    enum class Kind(val file: String, val tag: String) {
        BoxArt("boxart.jpg", "box"),
        /** The few titles whose only box art is a PNG scan. */
        BoxArtPng("boxart.png", "boxpng"),
        Background("background.jpg", "bg"),
    }

    /** Box art width / height. */
    const val BOX_ASPECT = 219f / 300f

    /** Share of a box art's height taken by the white "XBOX 360" header of the case. */
    const val CASE_HEADER = 0.13f

    /** Bumped whenever a missing picture is learnt, so whatever reads it re-picks its source. */
    val missingVersion = mutableIntStateOf(0)

    // Pictures the archive does not have, learnt from its 404s and kept across launches: a game
    // without one goes straight to its fallback instead of asking the network every time.
    private val missing: MutableSet<String> = ConcurrentHashMap.newKeySet()
    @Volatile private var prefs: SharedPreferences? = null

    private fun prefsOf(context: Context): SharedPreferences = prefs ?: synchronized(this) {
        prefs ?: context.applicationContext.getSharedPreferences("game_art", Context.MODE_PRIVATE).also { p ->
            missing.addAll(p.getStringSet("missing", emptySet()).orEmpty())
            prefs = p
        }
    }

    private fun idOf(titleId: String?): String? = titleId?.takeIf { it.length == 8 }?.uppercase()

    private fun keyOf(id: String, kind: Kind) = "${kind.tag}:$id"

    private fun markMissing(context: Context, id: String, kind: Kind) {
        val p = prefsOf(context)
        if (missing.add(keyOf(id, kind))) {
            p.edit().putStringSet("missing", HashSet(missing)).apply()
            missingVersion.intValue++
        }
    }

    fun url(titleId: String?, kind: Kind): String? {
        val id = idOf(titleId) ?: return null
        return "https://raw.githubusercontent.com/xenia-manager/x360db/main/titles/$id/artwork/${kind.file}"
    }

    /** The address to ask for, or null when the game has no title id or the archive lacks it. */
    private fun available(context: Context, titleId: String?, kind: Kind): String? {
        val id = idOf(titleId) ?: return null
        prefsOf(context)
        return if (keyOf(id, kind) in missing) null else url(id, kind)
    }

    private fun ImageRequest.Builder.learnMissing(context: Context, titleId: String?, kind: Kind) = apply {
        val id = idOf(titleId) ?: return@apply
        listener(onError = { _, result ->
            if ((result.throwable as? HttpException)?.response?.code == 404) markMissing(context, id, kind)
        })
    }

    /** Box art: the archive's JPEG, else its PNG. Decoded once at up to 600 px wide (the PNG
     *  scans are larger, the JPEGs smaller and never enlarged), shared by the flow, its
     *  reflection, the backdrop and the prefetch, and faded in over the built cover. */
    fun boxArtRequest(context: Context, titleId: String?): ImageRequest? {
        val kind = when {
            available(context, titleId, Kind.BoxArt) != null -> Kind.BoxArt
            available(context, titleId, Kind.BoxArtPng) != null -> Kind.BoxArtPng
            else -> return null
        }
        return ImageRequest.Builder(context).data(url(titleId, kind))
            .size(600, 822).precision(Precision.INEXACT).crossfade(180)
            .learnMissing(context, titleId, kind).build()
    }

    /** The Marketplace background, small for the blurred light or whole for the picture. */
    fun backgroundRequest(context: Context, titleId: String?, small: Boolean): ImageRequest? {
        val url = available(context, titleId, Kind.Background) ?: return null
        return ImageRequest.Builder(context).data(url)
            .apply { if (small) size(320, 180) }
            .crossfade(250)
            .learnMissing(context, titleId, Kind.Background).build()
    }

    /** Downloads a background into the disk cache only (decoded tiny, not kept in memory), and
     *  learns early when there is none. */
    fun backgroundPrefetch(context: Context, titleId: String?): ImageRequest? {
        val url = available(context, titleId, Kind.Background) ?: return null
        return ImageRequest.Builder(context).data(url).size(64, 36)
            .memoryCachePolicy(CachePolicy.DISABLED)
            .learnMissing(context, titleId, Kind.Background).build()
    }
}
