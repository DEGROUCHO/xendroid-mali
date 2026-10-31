package xendroid.compose.data

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.nio.file.Files

class CachedLibraryTest {
    @get:Rule val tmp = TemporaryFolder()
    private fun entry(file: File, name: String = "Game", icon: String? = null) =
        GameMetadataCache.Entry(name, icon, file.length(), file.lastModified(),
            titleId = "12345678", format = GameFormat.ISO, discNumber = 2, discCount = 2)
    private fun game(root: File, name: String): File = File(root, name).apply { writeText("data") }

    @Test fun unchangedGamesAppearSortedWithDiscMetadata() {
        val root = tmp.newFolder(); val z = game(root,"z.iso"); val a = game(root,"a.iso")
        val games = CachedLibrary.games(root,mapOf(z.path to entry(z,"Zulu"),a.path to entry(a,"alpha"))) { true }
        assertEquals(listOf("alpha","Zulu"),games.map { it.name })
        assertEquals("12345678",games.first().titleId); assertEquals(2,games.first().discNumber)
    }
    @Test fun changedAndDeletedGamesAreNotPublished() {
        val root = tmp.newFolder(); val changed = game(root,"changed.iso");val deleted = game(root,"deleted.iso")
        val entries=mapOf(changed.path to entry(changed),deleted.path to entry(deleted))
        changed.appendText("changed");deleted.delete()
        assertTrue(CachedLibrary.games(root,entries) { true }.isEmpty())
    }
    @Test fun otherRootAndSiblingPrefixDoNotLeak() {
        val root=tmp.newFolder("games");val other=tmp.newFolder("games-other");val file=game(other,"game.iso")
        assertTrue(CachedLibrary.games(root,mapOf(file.path to entry(file))) { true }.isEmpty())
    }
    @Test fun missingIconAndUnknownFormatRequireRescan() {
        val root=tmp.newFolder();val file=game(root,"game.iso")
        assertTrue(CachedLibrary.games(root,mapOf(file.path to entry(file,icon="lost.png"))) { false }.isEmpty())
        assertTrue(CachedLibrary.games(root,mapOf(file.path to entry(file).copy(format=null))) { true }.isEmpty())
    }
    @Test fun symlinkEscapingRootIsNotPublished() {
        val root=tmp.newFolder();val outside=game(tmp.newFolder(),"outside.iso")
        val link=File(root,"link.iso");Files.createSymbolicLink(link.toPath(),outside.toPath())
        assertTrue(CachedLibrary.games(root,mapOf(link.path to entry(link))) { true }.isEmpty())
    }
}
