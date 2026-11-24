package app.convertible.json

/**
 * A small strict JSON reader and writer (RFC 8259), so that catalogue, schema and token parsing is plain Kotlin
 * that runs unchanged in JVM unit tests (android.jar's org.json is only a stub there).
 *
 * Values map to: object -> [Map] (insertion ordered), array -> [List], string -> [String], number -> [Long] when it
 * is an integer that fits, otherwise [Double], true/false -> [Boolean], null -> null.
 */
object Json {
    class ParseException(message: String, val offset: Int) : IllegalArgumentException("$message at offset $offset")

    fun parse(text: String): Any? {
        val reader = Reader(text)
        reader.skipWhitespace()
        val value = reader.readValue(0)
        reader.skipWhitespace()
        if (!reader.atEnd()) throw ParseException("trailing characters", reader.pos)
        return value
    }

    /** Serialises maps, lists, strings, numbers, booleans and null. */
    fun write(value: Any?): String = StringBuilder().also { writeTo(it, value) }.toString()

    fun quote(text: String): String = StringBuilder().also { writeString(it, text) }.toString()

    private fun writeTo(out: StringBuilder, value: Any?) {
        when (value) {
            null -> out.append("null")
            is String -> writeString(out, value)
            is Boolean -> out.append(if (value) "true" else "false")
            is Int, is Long, is Short, is Byte -> out.append(value.toString())
            is Float -> writeDouble(out, value.toDouble())
            is Double -> writeDouble(out, value)
            is Map<*, *> -> {
                out.append('{')
                var first = true
                for ((k, v) in value) {
                    if (!first) out.append(',')
                    first = false
                    writeString(out, k.toString())
                    out.append(':')
                    writeTo(out, v)
                }
                out.append('}')
            }
            is Iterable<*> -> {
                out.append('[')
                var first = true
                for (v in value) {
                    if (!first) out.append(',')
                    first = false
                    writeTo(out, v)
                }
                out.append(']')
            }
            else -> writeString(out, value.toString())
        }
    }

    private fun writeDouble(out: StringBuilder, value: Double) {
        require(value.isFinite()) { "JSON cannot represent $value" }
        if (value == Math.rint(value) && kotlin.math.abs(value) < 1e15) out.append(value.toLong()) else out.append(value)
    }

    private fun writeString(out: StringBuilder, text: String) {
        out.append('"')
        for (c in text) {
            when (c) {
                '"' -> out.append("\\\"")
                '\\' -> out.append("\\\\")
                '\n' -> out.append("\\n")
                '\r' -> out.append("\\r")
                '\t' -> out.append("\\t")
                '\b' -> out.append("\\b")
                '\u000C' -> out.append("\\f")
                else -> if (c < ' ' || c == ' ' || c == ' ') {
                    out.append("\\u").append(Integer.toHexString(c.code).padStart(4, '0'))
                } else {
                    out.append(c)
                }
            }
        }
        out.append('"')
    }

    private class Reader(private val s: String) {
        var pos = 0

        fun atEnd() = pos >= s.length

        fun skipWhitespace() {
            while (pos < s.length) {
                val c = s[pos]
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') pos++ else break
            }
        }

        fun readValue(depth: Int): Any? {
            if (depth > MAX_DEPTH) throw ParseException("nesting too deep", pos)
            if (atEnd()) throw ParseException("unexpected end", pos)
            return when (val c = s[pos]) {
                '{' -> readObject(depth)
                '[' -> readArray(depth)
                '"' -> readString()
                't' -> literal("true", true)
                'f' -> literal("false", false)
                'n' -> literal("null", null)
                else -> if (c == '-' || c in '0'..'9') readNumber() else throw ParseException("unexpected '$c'", pos)
            }
        }

        private fun literal(word: String, value: Any?): Any? {
            if (!s.startsWith(word, pos)) throw ParseException("invalid literal", pos)
            pos += word.length
            return value
        }

        private fun readObject(depth: Int): Map<String, Any?> {
            pos++ // {
            val map = LinkedHashMap<String, Any?>()
            skipWhitespace()
            if (peek() == '}') {
                pos++
                return map
            }
            while (true) {
                skipWhitespace()
                if (peek() != '"') throw ParseException("expected a key", pos)
                val key = readString()
                skipWhitespace()
                expect(':')
                skipWhitespace()
                map[key] = readValue(depth + 1)
                skipWhitespace()
                when (peek()) {
                    ',' -> pos++
                    '}' -> {
                        pos++
                        return map
                    }
                    else -> throw ParseException("expected ',' or '}'", pos)
                }
            }
        }

        private fun readArray(depth: Int): List<Any?> {
            pos++ // [
            val list = ArrayList<Any?>()
            skipWhitespace()
            if (peek() == ']') {
                pos++
                return list
            }
            while (true) {
                skipWhitespace()
                list.add(readValue(depth + 1))
                skipWhitespace()
                when (peek()) {
                    ',' -> pos++
                    ']' -> {
                        pos++
                        return list
                    }
                    else -> throw ParseException("expected ',' or ']'", pos)
                }
            }
        }

        private fun readString(): String {
            pos++ // opening quote
            val out = StringBuilder()
            while (true) {
                if (atEnd()) throw ParseException("unterminated string", pos)
                val c = s[pos++]
                when {
                    c == '"' -> return out.toString()
                    c == '\\' -> {
                        if (atEnd()) throw ParseException("unterminated escape", pos)
                        when (val e = s[pos++]) {
                            '"' -> out.append('"')
                            '\\' -> out.append('\\')
                            '/' -> out.append('/')
                            'b' -> out.append('\b')
                            'f' -> out.append('\u000C')
                            'n' -> out.append('\n')
                            'r' -> out.append('\r')
                            't' -> out.append('\t')
                            'u' -> {
                                if (pos + 4 > s.length) throw ParseException("short \\u escape", pos)
                                val code = s.substring(pos, pos + 4).toIntOrNull(16)
                                    ?: throw ParseException("invalid \\u escape", pos)
                                out.append(code.toChar())
                                pos += 4
                            }
                            else -> throw ParseException("invalid escape '\\$e'", pos - 1)
                        }
                    }
                    c < ' ' -> throw ParseException("control character in string", pos - 1)
                    else -> out.append(c)
                }
            }
        }

        private fun readNumber(): Any {
            val start = pos
            if (peek() == '-') pos++
            if (peek() == '0') {
                pos++
            } else if (peek() in '1'..'9') {
                while (peek() in '0'..'9') pos++
            } else {
                throw ParseException("invalid number", pos)
            }
            var integral = true
            if (peek() == '.') {
                integral = false
                pos++
                if (peek() !in '0'..'9') throw ParseException("invalid fraction", pos)
                while (peek() in '0'..'9') pos++
            }
            if (peek() == 'e' || peek() == 'E') {
                integral = false
                pos++
                if (peek() == '+' || peek() == '-') pos++
                if (peek() !in '0'..'9') throw ParseException("invalid exponent", pos)
                while (peek() in '0'..'9') pos++
            }
            val text = s.substring(start, pos)
            if (integral) text.toLongOrNull()?.let { return it }
            return text.toDouble()
        }

        private fun peek(): Char = if (pos < s.length) s[pos] else '\u0000'

        private fun expect(c: Char) {
            if (peek() != c) throw ParseException("expected '$c'", pos)
            pos++
        }
    }

    private const val MAX_DEPTH = 256
}

/** Typed accessors for parsed JSON objects. */
@Suppress("UNCHECKED_CAST")
fun Any?.asObject(): Map<String, Any?> = this as? Map<String, Any?> ?: emptyMap()
fun Any?.asArray(): List<Any?> = this as? List<Any?> ?: emptyList()
fun Map<String, Any?>.string(key: String, fallback: String = ""): String = this[key] as? String ?: fallback
fun Map<String, Any?>.stringOrNull(key: String): String? = this[key] as? String
fun Map<String, Any?>.bool(key: String, fallback: Boolean = false): Boolean = this[key] as? Boolean ?: fallback
fun Map<String, Any?>.long(key: String, fallback: Long = 0): Long = (this[key] as? Number)?.toLong() ?: fallback
fun Map<String, Any?>.double(key: String): Double? = (this[key] as? Number)?.toDouble()
fun Map<String, Any?>.strings(key: String): List<String> = this[key].asArray().filterIsInstance<String>()
