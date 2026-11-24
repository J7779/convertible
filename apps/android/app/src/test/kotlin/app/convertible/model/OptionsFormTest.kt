package app.convertible.model

import app.convertible.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class OptionsFormTest {
    private val schema = Catalogue.parseOptions(Fixtures.JPG_SCHEMA_JSON)

    @Test
    fun splitsPresetBasicAndAdvanced() {
        assertTrue(OptionsForm.hasPreset(schema))
        assertEquals(listOf("quality"), OptionsForm.basic(schema).map { it.key })
        assertEquals(listOf("strip_metadata", "scale", "comment"), OptionsForm.advanced(schema).map { it.key })
        assertFalse(OptionsForm.hasPreset(schema.filter { it.key != "preset" }))
    }

    @Test
    fun defaultsAreNotSent() {
        val resolved = OptionsForm.resolve(schema, emptyMap(), Preset.BALANCED, stripMetadata = true)
        assertTrue(resolved.isEmpty())
        assertNull(OptionsForm.toJson(resolved))
    }

    @Test
    fun appDefaultsApplyWhenTheUserHasNotChosen() {
        val resolved = OptionsForm.resolve(schema, emptyMap(), Preset.SMALL, stripMetadata = false)
        assertEquals(mapOf("preset" to OptionValue.Text("small"), "strip_metadata" to OptionValue.BoolValue(false)), resolved)
        assertEquals(mapOf("preset" to "small", "strip_metadata" to false), Json.parse(OptionsForm.toJson(resolved)!!))
    }

    @Test
    fun userChoicesWinAndAreCoerced() {
        val values = mapOf(
            "preset" to OptionValue.Text("high"),
            "quality" to OptionValue.IntValue(250), // clamped to 100
            "scale" to OptionValue.IntValue(0), // int for a float option, clamped to the minimum
            "unknown" to OptionValue.Text("dropped"),
        )
        val resolved = OptionsForm.resolve(schema, values, Preset.SMALL, stripMetadata = true)
        assertEquals(OptionValue.Text("high"), resolved["preset"])
        assertEquals(OptionValue.IntValue(100), resolved["quality"])
        assertEquals(OptionValue.FloatValue(0.01), resolved["scale"])
        assertNull(resolved["unknown"])
        // Schema order.
        assertEquals("""{"quality":100,"preset":"high","scale":0.01}""", OptionsForm.toJson(resolved))
    }

    @Test
    fun enumRejectsValuesOutsideTheChoices() {
        val preset = schema.first { it.key == "preset" }
        assertNull(preset.coerce(OptionValue.Text("ultra")))
        assertNull(preset.coerce(OptionValue.IntValue(1)))
        val resolved = OptionsForm.resolve(schema, mapOf("preset" to OptionValue.Text("ultra")), null, stripMetadata = true)
        assertTrue(resolved.isEmpty())
    }

    @Test
    fun schemasWithoutStandardKeysIgnoreAppDefaults() {
        val plain = Catalogue.parseOptions(
            """[{"key":"line_width","label":"Line width","type":"int","default":76,"min":0,"max":1024,"choices":[],"description":"","category":"basic"}]""",
        )
        assertFalse(OptionsForm.hasPreset(plain))
        assertTrue(OptionsForm.resolve(plain, emptyMap(), Preset.SMALL, stripMetadata = false).isEmpty())
        assertEquals(OptionValue.IntValue(76), OptionsForm.valueOf(plain[0], emptyMap()))
        assertEquals(OptionValue.IntValue(0), OptionsForm.valueOf(plain[0], mapOf("line_width" to OptionValue.IntValue(0))))
    }

    @Test
    fun presetIds() {
        assertEquals(Preset.BALANCED, Preset.from("balanced"))
        assertNull(Preset.from("medium"))
    }
}
