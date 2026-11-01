package xendroid.compose.updater

import xendroid.compose.l10n

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.google.gson.annotations.SerializedName
import retrofit2.Retrofit
import retrofit2.converter.gson.GsonConverterFactory
import retrofit2.http.GET
import xendroid.compose.BuildConfig
import okhttp3.OkHttpClient
import okhttp3.logging.HttpLoggingInterceptor


data class GithubRelease(
    @SerializedName("name")
    val name: String? = null,

    @SerializedName("tag_name")
    val tagName: String,

    @SerializedName("body")
    val changelog: String? = null,

    @SerializedName("html_url")
    val releaseUrl: String
)

sealed class UpdateResult {
    data class Available(
        val release: GithubRelease
    ) : UpdateResult()

    data class Latest(
        val commitHash: String
    ) : UpdateResult()

     data class Cooldown(
        val remainingMillis: Long
    ) : UpdateResult()
}

interface GithubApi {

    // This fork's own releases: upstream builds target Adreno and would replace the Mali work.
    @GET("repos/DEGROUCHO/xendroid-mali/releases/latest")
    suspend fun latestRelease(): GithubRelease
}

private const val PREFS_NAME = "updater"
private const val KEY_LAST_CHECK = "last_update_check"
private const val UPDATE_INTERVAL = 5 * 60 * 1000L

fun getRemainingCooldown(context: Context): Long {
    val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    val lastCheck = prefs.getLong(KEY_LAST_CHECK, 0L)
    val now = System.currentTimeMillis()

    return (UPDATE_INTERVAL - (now - lastCheck)).coerceAtLeast(0L)
}

fun shouldCheckForUpdates(context: Context): Boolean {
    // Custom performance build: never auto-prompt; an upstream update would
    // replace it. Manual check from the library menu still works.
    return false
}

fun saveLastCheck(context: Context) {
    context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        .edit()
        .putLong(KEY_LAST_CHECK, System.currentTimeMillis())
        .apply()
}

private val retrofit = Retrofit.Builder()
    .baseUrl("https://api.github.com/")
    .client(
        OkHttpClient.Builder()
            .addInterceptor(
                HttpLoggingInterceptor().apply {
                    level = HttpLoggingInterceptor.Level.BODY
                }
            )
            .build()
    )
    .addConverterFactory(GsonConverterFactory.create())
    .build()


private val api = retrofit.create(GithubApi::class.java)


suspend fun checkForUpdates(): UpdateResult {

    Log.d("Updater", "Starting update check")

    val release = api.latestRelease()

    Log.d("Updater", "Release tag: ${release.tagName}")

    // Release builds are named after their tag (q636.1); the build type may add a suffix.
    val currentHash = BuildConfig.VERSION_NAME.removeSuffix("-debug")

    Log.d("Updater", "Current: $currentHash")

    val latestHash = release.tagName.trim()

    Log.d("Updater", "Latest: $latestHash")

    return if (currentHash != latestHash) {
        UpdateResult.Available(release)
    } else {
        UpdateResult.Latest(currentHash)
    }
}

fun cleanChangelog(text: String): String {
    return text
        .replace(Regex("(?s)\\*\\*Full Changelog\\*\\*:.*"), "")
        .replace(Regex("https?://\\S+"), "")
        .replace("## What's Changed", "")
        .replace(Regex("\\* "), "• ")
        .replace(Regex("\n{3,}"), "\n\n")
        .trim()
}

@Composable
fun UpdateDialog(
    release: GithubRelease,
    onDismiss: () -> Unit
) {
    val context = LocalContext.current

    AlertDialog(
        onDismissRequest = onDismiss,

        title = {
            Text(l10n("Update available", "Aggiornamento disponibile"))
        },

        text = {
           Column(
            modifier = Modifier
            .heightIn(max = 400.dp)
            .verticalScroll(rememberScrollState())
            ) {
                Text(
                    l10n("A new update is available for XenDroid.", "È disponibile un nuovo aggiornamento per XenDroid.")
                )

                Spacer(modifier = Modifier.height(16.dp))

                Text(
                    l10n("Version: ${release.tagName}", "Versione: ${release.tagName}")
                )

                Spacer(modifier = Modifier.height(16.dp))

                Text(
                    l10n("What's new:", "Novità:")
                )

                Spacer(modifier = Modifier.height(8.dp))

               Text(
                cleanChangelog(
                    release.changelog ?: l10n("No change description available.", "Nessuna descrizione delle modifiche disponibile.")
                )
            )

                Spacer(modifier = Modifier.height(16.dp))

                Text(
                    l10n("Do you want to download it now?", "Vuoi scaricarlo adesso?")
                )

                Spacer(modifier = Modifier.height(16.dp))

                Text(
                    l10n("Otherwise you will be asked again in 5 minutes (avoids API request limit problems).", "Altrimenti ti verrà chiesto di nuovo tra 5 minuti (evita problemi di limite delle richieste API).")
                )
            }
        },

        confirmButton = {
            TextButton(
                onClick = {
                    val intent = Intent(
                        Intent.ACTION_VIEW,
                        Uri.parse(release.releaseUrl)
                    )

                    context.startActivity(intent)
                    onDismiss()
                }
            ) {
                Text(l10n("Yes", "Sì"))
            }
        },

        dismissButton = {
            TextButton(
                onClick = onDismiss
            ) {
                Text("No")
            }
        }
    )
}

@Composable
fun LatestVersionDialog(
    commitHash: String,
    onDismiss: () -> Unit
) {
    AlertDialog(
        onDismissRequest = onDismiss,

        title = {
            Text(l10n("No update available", "Nessun aggiornamento disponibile"))
        },

        text = {
            Text(
                l10n("You have the latest version: $commitHash", "Hai l'ultima versione: $commitHash")
            )
        },

        confirmButton = {
            TextButton(
                onClick = onDismiss
            ) {
                Text("OK")
            }
        }
    )
}

@Composable
fun CooldownDialog(
    remainingMillis: Long,
    onDismiss: () -> Unit
) {
    val minutes = remainingMillis / 1000 / 60
    val seconds = (remainingMillis / 1000) % 60

    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Text(l10n("Update check paused", "Controllo in pausa"))
        },
        text = {
            Text(
                l10n("Update checks are paused.\n\n", "Il controllo degli aggiornamenti è in pausa.\n\n") +
                l10n("You can check again in ${minutes}m ${seconds}s.", "Puoi controllare di nuovo tra ${minutes}m ${seconds}s.")
            )
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text("OK")
            }
        }
    )
}