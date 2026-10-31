package xendroid.compose

import android.app.Activity
import android.content.Context
import android.content.Intent
import java.util.Locale

/**
 * UI language. Every user-visible string is written as l10n(english, italian); the choice is read
 * once per process (MainActivity / EmulatorHostActivity onCreate), and changing it restarts the app
 * so strings cached in objects (setting descriptions, schema labels) follow it too.
 */
object UiLanguage {
    const val SYSTEM = "system"
    const val ENGLISH = "en"
    const val ITALIAN = "it"
    private const val PREFS = "ui_language"
    private const val KEY = "language"

    @Volatile
    var italian: Boolean = Locale.getDefault().language == ITALIAN
        private set

    val locale: Locale get() = if (italian) Locale.ITALIAN else Locale.ENGLISH

    fun init(context: Context) {
        italian = when (get(context)) {
            ITALIAN -> true
            ENGLISH -> false
            else -> Locale.getDefault().language == ITALIAN
        }
    }

    fun get(context: Context): String =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, SYSTEM) ?: SYSTEM

    /** Saves [value] and relaunches the app so every screen is rebuilt in the new language. */
    fun setAndRestart(activity: Activity, value: String) {
        activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY, value).commit()
        val launch = activity.packageManager.getLaunchIntentForPackage(activity.packageName) ?: return
        activity.startActivity(launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK))
        activity.finishAffinity()
        Runtime.getRuntime().exit(0)
    }
}

fun l10n(en: String, it: String): String = if (UiLanguage.italian) it else en
