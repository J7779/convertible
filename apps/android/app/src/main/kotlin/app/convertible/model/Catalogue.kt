package app.convertible.model

import app.convertible.json.Json
import app.convertible.json.asArray
import app.convertible.json.asObject
import app.convertible.json.bool
import app.convertible.json.double
import app.convertible.json.long
import app.convertible.json.string
import app.convertible.json.strings

/** One entry of `cvt_formats_json`. */
data class FormatInfo(
    val id: String,
    val name: String,
    val description: String,
    val category: String,
    val extensions: List<String>,
    val mime: String,
    val readable: Boolean,
    val writable: Boolean,
    val popularity: Int,
) {
    /** Short upper-case label for badges, e.g. "PNG". */
    val label: String get() = (extensions.firstOrNull() ?: id).uppercase()
}

/** One entry of `cvt_targets_json`. */
data class TargetInfo(
    val id: String,
    val name: String,
    val category: String,
    val extensions: List<String>,
    val mime: String,
    val lossy: Boolean,
    val compress: Boolean,
    /**
     * Optional, additive: what the target does for this source when the core says so (for example "compress",
     * "archive" or "extract"). Null when the core does not send it; the app then falls back to [compress] and the
     * category. Unknown values are kept and ignored.
     */
    val role: String? = null,
) {
    val label: String get() = id.uppercase()
}

/** Category ids in catalogue order (see convertible.h). Unknown future ids sort after these. */
object Categories {
    val ORDER = listOf(
        "image", "audio", "video", "document", "ebook", "spreadsheet", "presentation", "data", "archive",
        "font", "subtitle", "model3d", "geo", "pim", "code", "encoding", "vector", "other",
    )

    fun rank(category: String): Int = ORDER.indexOf(category).let { if (it < 0) ORDER.size else it }
}

/** The format catalogue with lookups by id and extension. */
class Catalogue(val formats: List<FormatInfo>) {
    private val byId = formats.associateBy { it.id }
    private val byExtension: Map<String, FormatInfo> = buildMap {
        for (f in formats) for (e in f.extensions) putIfAbsent(e.lowercase(), f)
    }

    operator fun get(id: String?): FormatInfo? = id?.let { byId[it] ?: byExtension[it.lowercase()] }

    fun popularity(id: String): Int = byId[id]?.popularity ?: 0

    /** Categories present in the catalogue, in [Categories.ORDER]. */
    val categories: List<String> get() = formats.map { it.category }.distinct().sortedBy { Categories.rank(it) }

    companion object {
        val EMPTY = Catalogue(emptyList())

        fun parse(json: String): Catalogue = Catalogue(parseFormats(json))

        fun parseFormats(json: String): List<FormatInfo> = parseArray(json).mapNotNull { item ->
            val o = item.asObject()
            val id = o.string("id")
            if (id.isEmpty()) return@mapNotNull null
            FormatInfo(
                id = id,
                name = o.string("name", id),
                description = o.string("description"),
                category = o.string("category", "other").ifEmpty { "other" },
                extensions = o.strings("extensions"),
                mime = o.string("mime"),
                readable = o.bool("readable"),
                writable = o.bool("writable"),
                popularity = o.long("popularity").toInt(),
            )
        }

        fun parseTargets(json: String): List<TargetInfo> = parseArray(json).mapNotNull { item ->
            val o = item.asObject()
            val id = o.string("id")
            if (id.isEmpty()) return@mapNotNull null
            TargetInfo(
                id = id,
                name = o.string("name", id),
                category = o.string("category", "other").ifEmpty { "other" },
                extensions = o.strings("extensions"),
                mime = o.string("mime"),
                lossy = o.bool("lossy"),
                compress = o.bool("compress"),
                role = o.string("role").ifEmpty { null },
            )
        }

        fun parseOptions(json: String): List<OptionSpec> = parseArray(json).mapNotNull { item ->
            val o = item.asObject()
            val key = o.string("key")
            if (key.isEmpty()) return@mapNotNull null
            val type = OptionType.from(o.string("type"))
            OptionSpec(
                key = key,
                label = o.string("label", key),
                type = type,
                default = OptionValue.of(type, o["default"]),
                min = o.double("min"),
                max = o.double("max"),
                choices = o["choices"].asArray().mapNotNull { (it as? String) ?: (it as? Number)?.toString() },
                description = o.string("description"),
                advanced = o.string("category") == "advanced",
            )
        }

        private fun parseArray(json: String): List<Any?> = runCatching { Json.parse(json) }.getOrNull().asArray()
    }
}
