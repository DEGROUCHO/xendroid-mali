package xendroid.compose.ui.library
import org.junit.Assert.*
import org.junit.Test
import xendroid.compose.data.Game
import xendroid.compose.data.GameFormat

class LibraryOrderTest {
    private val games=listOf(Game("z","Zulu",GameFormat.ISO),Game("b","Beta",GameFormat.ISO),Game("a","alpha",GameFormat.ISO))
    @Test fun newestFirstAndUnplayedTitlesAlphabetically() {
        assertEquals(listOf("b","a","z"),orderLibraryGames(games,true) { if(it.stableId=="b") 20L else 0L }.map { it.stableId })
    }
    @Test fun nameOrderIgnoresRecency() {
        assertEquals(listOf("a","b","z"),orderLibraryGames(games,false) { 99L }.map { it.stableId })
    }
}
