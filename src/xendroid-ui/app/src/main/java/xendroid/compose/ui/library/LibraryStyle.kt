package xendroid.compose.ui.library

import android.content.Context

/** The library's face, kept with its other view choices: the NXE dashboard with the game
 *  covers (the default for everyone), or the classic grid. */
object LibraryStyle {
    private const val PREFS = "library_ui"
    private const val KEY = "dashboard_view"
    // The dashboard became the face of the library for everyone: an earlier grid choice is
    // reset once, then the player's choice holds again.
    private const val RESET_KEY = "dashboard_default_2"

    fun isDashboard(context: Context): Boolean {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (!prefs.getBoolean(RESET_KEY, false)) {
            prefs.edit().putBoolean(KEY, true).putBoolean(RESET_KEY, true).apply()
            return true
        }
        return prefs.getBoolean(KEY, true)
    }

    fun setDashboard(context: Context, on: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(KEY, on).apply()
    }
}
