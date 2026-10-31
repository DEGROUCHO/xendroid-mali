package xendroid.compose.core

import java.io.File

/** The file the native core polls while a game runs (its `live_cvar_file`, by default
 *  `ui-live.toml` next to the global config): flat `name = value` lines of scalar cvars,
 *  applied within about a second and never saved. The core deletes it at every game start,
 *  so only what this session writes is applied. */
object LiveCvars {
    const val FILE_NAME = "ui-live.toml"

    /** One `name = value` line per entry, sorted by name. Only booleans and integers. */
    fun render(values: Map<String, Any>): String =
        values.toSortedMap().entries.joinToString(separator = "") { (name, value) ->
            require(name.matches(Regex("[A-Za-z0-9_]+"))) { "bad cvar name: $name" }
            when (value) {
                is Boolean, is Int -> "$name = $value\n"
                else -> throw IllegalArgumentException("unsupported value for $name")
            }
        }

    /** Replaces the file in one step, so the core never reads half of it. */
    fun write(dir: File, values: Map<String, Any>) {
        val tmp = File(dir, "$FILE_NAME.tmp")
        tmp.writeText(render(values))
        val target = File(dir, FILE_NAME)
        if (!tmp.renameTo(target)) {
            target.writeText(render(values))
            tmp.delete()
        }
    }
}
