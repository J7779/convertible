package app.convertible.model

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CatalogueTest {
    private val catalogue = Catalogue.parse(Fixtures.FORMATS_JSON)

    @Test
    fun parsesEveryFormatInOrder() {
        assertEquals(12, catalogue.formats.size)
        assertEquals("pdf", catalogue.formats.first().id)
        val jpg = catalogue["jpg"]!!
        assertEquals("JPEG image", jpg.name)
        assertEquals(listOf("jpg", "jpeg"), jpg.extensions)
        assertEquals("JPG", jpg.label)
        assertTrue(jpg.readable && jpg.writable)
        assertEquals(95, jpg.popularity)
    }

    @Test
    fun looksUpByExtensionToo() {
        assertEquals("jpg", catalogue["JPEG"]?.id)
        assertEquals("base64", catalogue["b64"]?.id)
        assertNull(catalogue["nope"])
        assertNull(catalogue[null])
    }

    @Test
    fun categoriesFollowCanonicalOrderAndKeepUnknownOnes() {
        assertEquals(listOf("image", "document", "archive", "encoding", "hologram"), catalogue.categories)
    }

    @Test
    fun toleratesBrokenOrPartialJson() {
        assertTrue(Catalogue.parseFormats("not json").isEmpty())
        assertTrue(Catalogue.parseFormats("{}").isEmpty())
        val partial = Catalogue.parseFormats("""[{"id":"x"},{"name":"no id"}]""")
        assertEquals(1, partial.size)
        assertEquals("other", partial[0].category)
        assertEquals("x", partial[0].name)
    }

    @Test
    fun parsesTargets() {
        val targets = Catalogue.parseTargets(Fixtures.PNG_TARGETS_JSON)
        assertEquals(10, targets.size)
        assertTrue(targets[0].compress)
        assertFalse(targets[1].compress)
        assertTrue(targets.first { it.id == "jpg" }.lossy)
    }

    @Test
    fun parsesOptionSchemaWithTypes() {
        val schema = Catalogue.parseOptions(Fixtures.JPG_SCHEMA_JSON)
        assertEquals(listOf("quality", "preset", "strip_metadata", "scale", "comment"), schema.map { it.key })
        val quality = schema[0]
        assertEquals(OptionType.INT, quality.type)
        assertEquals(OptionValue.IntValue(90), quality.default)
        assertEquals(1.0, quality.min!!, 0.0)
        assertEquals(100.0, quality.max!!, 0.0)
        assertFalse(quality.advanced)
        assertEquals(listOf("high", "balanced", "small"), schema[1].choices)
        assertEquals(OptionValue.BoolValue(true), schema[2].default)
        assertTrue(schema[2].advanced)
        assertEquals(OptionValue.FloatValue(1.0), schema[3].default)
        assertEquals(OptionValue.Text(""), schema[4].default)
        assertNull(schema[4].min)
    }

    @Test
    fun unknownOptionTypesBecomeStrings() {
        val schema = Catalogue.parseOptions("""[{"key":"k","type":"colour","default":"#fff"}]""")
        assertEquals(OptionType.STRING, schema.single().type)
        assertEquals(OptionValue.Text("#fff"), schema.single().default)
    }
}
