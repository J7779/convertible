package app.convertible.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Behaviour with the full catalogue: ~1,500 formats, hundreds of targets, new optional fields. */
class LargeCatalogueTest {
    private fun target(id: String, category: String, ext: String = id, name: String = id.uppercase(), compress: Boolean = false, role: String? = null) =
        TargetInfo(id, name, category, listOf(ext), "application/octet-stream", lossy = false, compress = compress, role = role)

    @Test
    fun `unknown and new optional fields are tolerated`() {
        val json = """[{"id":"tar.gz","name":"Gzipped tar","category":"archive","extensions":["tar.gz","tgz"],"mime":"application/gzip",
            "lossy":false,"compress":false,"role":"archive","future":{"nested":[1,2]},"weight":0.5},
            {"id":"ds_store","name":"Finder metadata","category":"other","extensions":[],"mime":"","lossy":true,"compress":false},
            {"name":"no id: skipped"}]"""
        val targets = Catalogue.parseTargets(json)
        assertEquals(listOf("tar.gz", "ds_store"), targets.map { it.id })
        assertEquals("archive", targets[0].role)
        assertNull(targets[1].role)
        val formats = Catalogue.parseFormats(
            """[{"id":"ds_store","name":"DS","category":"brand-new-category","extensions":["DS_Store"],"readable":true,"writable":false,"popularity":3,"aliases":["x"]}]""",
        )
        assertEquals("brand-new-category", formats.single().category)
        assertEquals(Categories.ORDER.size, Categories.rank("brand-new-category"))
        assertEquals("ds_store", Catalogue(formats)["ds_store"]?.id)
    }

    @Test
    fun `search ranks exact id or extension, then prefixes, then name words`() {
        val targets = listOf(
            target("tar.gz", "archive", ext = "tar.gz", name = "Gzipped tar archive"),
            target("gz", "archive", name = "Gzip"),
            target("targa", "image", ext = "tga", name = "Targa image"),
            target("tar", "archive", name = "Tape archive"),
            target("star", "other", name = "Star data"),
        )
        assertEquals(listOf("tar", "tar.gz", "targa", "star"), TargetRanking.search(targets, "tar").map { it.id })
        assertEquals("tar.gz", TargetRanking.search(targets, ".tar.gz").first().id)
        assertEquals("gz", TargetRanking.search(targets, "gz").first().id)
    }

    @Test
    fun `hundreds of targets organise into category sections without loss`() {
        val categories = Categories.ORDER
        val targets = (0 until 425).map { i -> target("f$i", categories[i % categories.size]) }
        val catalogue = Catalogue(targets.mapIndexed { i, t -> FormatInfo(t.id, t.name, "", t.category, t.extensions, "", true, true, 100 - i % 100) })
        val sections = TargetRanking.sections(targets, "image", catalogue)
        assertEquals("image", sections[1].category)
        val listed = sections.drop(1).flatMap { it.targets }.map { it.id }
        assertEquals(425, listed.size)
        assertEquals(listed.size, listed.toSet().size)
        assertTrue(sections.first().category == null && sections.first().targets.size == TargetRanking.POPULAR)
    }

    @Test
    fun `compress choices follow the core's roles when present`() {
        val legacy = listOf(target("png", "image", compress = true), target("zip", "archive"), target("jpg", "image"))
        assertEquals(listOf("png", "zip"), TargetRanking.compressTargets(legacy).map { it.id })
        val withRoles = listOf(
            target("png", "image", compress = true, role = "compress"),
            target("zip", "archive", role = "archive"),
            target("cpio", "archive", role = "extract"),
            target("jpg", "image", role = "convert"),
        )
        assertEquals(listOf("png", "zip"), TargetRanking.compressTargets(withRoles).map { it.id })
    }

    private fun spec(type: OptionType, min: Double?, max: Double?, choices: List<String> = emptyList()) =
        OptionSpec("k", "K", type, null, min, max, choices, "", false)

    @Test
    fun `wide ranges get a stepper, many choices a menu`() {
        assertEquals(NumberControl.SLIDER, OptionsForm.numberControl(spec(OptionType.INT, 1.0, 100.0)))
        assertEquals(NumberControl.SLIDER, OptionsForm.numberControl(spec(OptionType.INT, 0.0, 1000.0)))
        assertEquals(NumberControl.STEPPER, OptionsForm.numberControl(spec(OptionType.INT, 0.0, 2000.0)))
        assertEquals(NumberControl.STEPPER, OptionsForm.numberControl(spec(OptionType.INT, 0.0, 384000.0)))
        assertEquals(NumberControl.STEPPER, OptionsForm.numberControl(spec(OptionType.INT, null, null)))
        assertEquals(NumberControl.SLIDER, OptionsForm.numberControl(spec(OptionType.FLOAT, 0.01, 10.0)))
        assertEquals(1.0, OptionsForm.step(spec(OptionType.INT, 0.0, 384000.0)), 0.0)
        assertEquals(0.1, OptionsForm.step(spec(OptionType.FLOAT, 0.0, 10.0)), 1e-12)
        assertEquals(ChoiceControl.SEGMENTED, OptionsForm.choiceControl(spec(OptionType.ENUM, null, null, listOf("a", "b", "c"))))
        assertEquals(ChoiceControl.CHIPS, OptionsForm.choiceControl(spec(OptionType.ENUM, null, null, listOf("a", "b", "c", "d"))))
        assertEquals(ChoiceControl.MENU, OptionsForm.choiceControl(spec(OptionType.ENUM, null, null, (1..30).map { "c$it" })))
        assertEquals(ChoiceControl.CHIPS, OptionsForm.choiceControl(spec(OptionType.ENUM, null, null, emptyList())))
    }
}
