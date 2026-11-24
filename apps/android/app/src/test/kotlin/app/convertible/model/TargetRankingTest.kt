package app.convertible.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TargetRankingTest {
    private val catalogue = Catalogue.parse(Fixtures.FORMATS_JSON)
    private val targets = Catalogue.parseTargets(Fixtures.PNG_TARGETS_JSON)

    @Test
    fun sectionsStartWithPopularThenSourceCategoryThenByRelevance() {
        val sections = TargetRanking.sections(targets, "image", catalogue)
        assertNull(sections[0].category)
        // Popularity: pdf 98, jpg 95, zip 90, webp 80, gif 70 (png is the compress entry and excluded).
        assertEquals(listOf("pdf", "jpg", "zip", "webp", "gif"), sections[0].targets.map { it.id })
        assertEquals(listOf("image", "document", "archive", "encoding"), sections.drop(1).map { it.category })
        // Within a category the core's order is kept.
        assertEquals(listOf("webp", "bmp", "gif", "jpg"), sections[1].targets.map { it.id })
    }

    @Test
    fun sourceCategoryComesFirstEvenWhenLessRelevant() {
        val sections = TargetRanking.sections(targets, "encoding", catalogue)
        assertEquals("encoding", sections[1].category)
    }

    @Test
    fun smallListsSkipThePopularSection() {
        val few = targets.take(4)
        val sections = TargetRanking.sections(few, "image", catalogue)
        assertTrue(sections.all { it.category != null })
        assertEquals(listOf("webp", "bmp", "gif"), sections.single().targets.map { it.id })
    }

    @Test
    fun compressEntriesAreNeverListedAsTargets() {
        val all = TargetRanking.sections(targets, "image", catalogue).flatMap { it.targets }
        assertTrue(all.none { it.compress })
        assertTrue(TargetRanking.search(targets, "png").isEmpty())
        assertTrue(TargetRanking.suggestions(targets).none { it.compress })
    }

    @Test
    fun compressChoicesAreSameFormatThenArchives() {
        assertEquals(listOf("png", "zip"), TargetRanking.compressTargets(targets).map { it.id })
    }

    @Test
    fun suggestionsAreTheMostRelevant() {
        assertEquals(listOf("webp", "bmp", "gif"), TargetRanking.suggestions(targets).map { it.id })
    }

    @Test
    fun searchRanksExactThenPrefixThenNameThenSubstring() {
        assertEquals("jpg", TargetRanking.search(targets, "jpeg").first().id) // exact extension
        assertEquals(listOf("base64"), TargetRanking.search(targets, ".b64").map { it.id })
        assertEquals(listOf("hex"), TargetRanking.search(targets, "HEXA").map { it.id }) // name prefix, case-insensitive
        val image = TargetRanking.search(targets, "image").map { it.id }
        assertEquals(listOf("webp", "bmp", "gif", "jpg"), image) // name word prefix, core order
        assertEquals(listOf("checksums"), TargetRanking.search(targets, "sum").map { it.id }) // substring
        assertTrue(TargetRanking.search(targets, "zzz").isEmpty())
        assertEquals(targets.size - 1, TargetRanking.search(targets, "  ").size)
    }

    @Test
    fun formatSearchAlsoMatchesMime() {
        val result = TargetRanking.searchFormats(catalogue.formats, "application/zip")
        assertEquals(listOf("zip"), result.map { it.id })
        assertEquals(catalogue.formats, TargetRanking.searchFormats(catalogue.formats, ""))
    }

    @Test
    fun emptyTargetsGiveNoSections() {
        assertTrue(TargetRanking.sections(emptyList(), "image", catalogue).isEmpty())
    }
}
