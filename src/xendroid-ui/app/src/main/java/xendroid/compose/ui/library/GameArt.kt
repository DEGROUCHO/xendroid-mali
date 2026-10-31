package xendroid.compose.ui.library

/**
 * Xbox 360 Marketplace artwork by title id, from the open x360db archive
 * (github.com/xenia-manager/x360db, ~5900 games): box art 219x300 and background 1280x720.
 * Coil's disk cache keeps every image after its first download. When a title is missing or
 * there is no network, the callers keep showing the icon embedded in the game.
 */
internal object GameArt {
    enum class Kind(val file: String) {
        BoxArt("boxart.jpg"),
        Background("background.jpg"),
    }

    /** Box art width / height. */
    const val BOX_ASPECT = 219f / 300f

    fun url(titleId: String?, kind: Kind): String? {
        val id = titleId?.takeIf { it.length == 8 }?.uppercase() ?: return null
        return "https://raw.githubusercontent.com/xenia-manager/x360db/main/titles/$id/artwork/${kind.file}"
    }
}
