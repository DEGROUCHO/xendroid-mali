package xendroid.compose.ui.library

import xendroid.compose.data.Game
import xendroid.compose.data.GameFormat
import org.junit.Assert.assertTrue
import org.junit.Test
import xendroid.compose.ui.library.LibraryUiState

class LibraryUiStateTest {
    @Test fun loaded_with_games_is_not_empty() {
        val s = LibraryUiState.Loaded(listOf(Game("u", "G", GameFormat.ISO)))
        assertTrue(s.games.isNotEmpty())
    }
    @Test fun loaded_empty_is_distinct_from_no_folder() {
        assertTrue(LibraryUiState.Loaded(emptyList()) is LibraryUiState.Loaded)
        assertTrue(LibraryUiState.NoFolder is LibraryUiState)
    }
}

class LibraryFilterTest {
    private fun g(n: String) = xendroid.compose.data.Game("/x/$n.iso", n, xendroid.compose.data.GameFormat.ISO)
    private val games = listOf(g("Call of Duty - Black Ops"), g("Dante's Inferno"), g("Call of Duty - Modern Warfare 2"))

    @org.junit.Test
    fun blankQuery_keepsEverythingInOrder() {
        org.junit.Assert.assertEquals(games, filterGames(games, "  "))
    }

    @org.junit.Test
    fun everyWordMustMatch_caseInsensitive() {
        org.junit.Assert.assertEquals(listOf(games[2]), filterGames(games, "duty WARFARE"))
        org.junit.Assert.assertEquals(listOf(games[0], games[2]), filterGames(games, "call"))
        org.junit.Assert.assertEquals(emptyList<xendroid.compose.data.Game>(), filterGames(games, "halo"))
    }
}
