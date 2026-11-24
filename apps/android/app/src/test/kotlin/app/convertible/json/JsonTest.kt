package app.convertible.json

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class JsonTest {
    @Test
    fun parsesNestedValues() {
        val v = Json.parse("""{"a":[1,2.5,-3e2,true,false,null],"b":{"c":"d"},"e":9007199254740993}""").asObject()
        assertEquals(listOf(1L, 2.5, -300.0, true, false, null), v["a"])
        assertEquals("d", v["b"].asObject()["c"])
        assertEquals(9007199254740993L, v["e"])
    }

    @Test
    fun keepsKeyOrder() {
        val v = Json.parse("""{"z":1,"a":2,"m":3}""").asObject()
        assertEquals(listOf("z", "a", "m"), v.keys.toList())
    }

    @Test
    fun decodesEscapesAndSurrogatePairs() {
        val s = Json.parse("\"tab\\t nl\\n quote\\\" slash\\/ \\u00e9 \\ud83d\\ude00\"")
        assertEquals("tab\t nl\n quote\" slash/ \u00e9 \uD83D\uDE00", s)
    }

    @Test
    fun rejectsMalformedInput() {
        val bad = listOf("", "{", "[1,]", "{\"a\" 1}", "01", "1.", "\"\u0001\"", "tru", "[1] x", "\"\\x\"", "{\"a\":1,}")
        for (text in bad) {
            val failed = runCatching { Json.parse(text) }.exceptionOrNull()
            assertTrue("should reject <$text>", failed is Json.ParseException)
        }
    }

    @Test
    fun writesAndRoundTrips() {
        val value = linkedMapOf("quality" to 70L, "preset" to "small", "flag" to false, "scale" to 0.5, "text" to "a\"b\\c\n\u0001")
        val json = Json.write(value)
        assertEquals("""{"quality":70,"preset":"small","flag":false,"scale":0.5,"text":"a\"b\\c\n\u0001"}""", json)
        assertEquals(value, Json.parse(json))
    }

    @Test
    fun writesWholeDoublesAsIntegers() {
        assertEquals("[3,1.25]", Json.write(listOf(3.0, 1.25)))
    }

    @Test
    fun typedAccessorsFallBack() {
        val o = Json.parse("""{"s":"x","n":4,"b":true,"l":["p",1,"q"]}""").asObject()
        assertEquals("x", o.string("s"))
        assertEquals("dflt", o.string("missing", "dflt"))
        assertEquals(4L, o.long("n"))
        assertEquals(true, o.bool("b"))
        assertEquals(listOf("p", "q"), o.strings("l"))
        assertNull(o.stringOrNull("n"))
        assertTrue(Json.parse("[]").asObject().isEmpty())
    }
}
