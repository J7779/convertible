package app.convertible.brand

import app.convertible.json.Json
import app.convertible.json.asArray
import app.convertible.json.asObject
import app.convertible.json.double
import app.convertible.json.string

/** ARGB colour parsed from "#RRGGBB" or "#RRGGBBAA" (CSS order: alpha last). */
fun parseHexColor(hex: String): Long {
    val h = hex.trim().removePrefix("#")
    return when (h.length) {
        6 -> 0xFF000000L or h.toLong(16)
        8 -> {
            val rgb = h.substring(0, 6).toLong(16)
            val a = h.substring(6, 8).toLong(16)
            (a shl 24) or rgb
        }
        else -> throw IllegalArgumentException("bad colour '$hex'")
    }
}

data class TypeStyleToken(
    val size: Double,
    val lineHeight: Double,
    val weight: Int,
    val letterSpacingEm: Double,
    val mono: Boolean,
    val uppercase: Boolean,
)

/**
 * Design tokens from `assets/brand/tokens.json` (the source of truth), parsed without any Android dependency.
 * Colours are ARGB longs; sizes are in dp/sp units as written in the file.
 */
class BrandTokens(
    val dark: Map<String, Long>,
    val light: Map<String, Long>,
    val categoryDark: Map<String, Long>,
    val categoryLight: Map<String, Long>,
    val brand: Map<String, Long>,
    val gradient: List<Pair<Float, Long>>,
    val gradientOnLight: List<Pair<Float, Long>>,
    val radius: Map<String, Double>,
    val space: Map<String, Double>,
    val size: Map<String, Double>,
    val durations: Map<String, Int>,
    val easings: Map<String, List<Double>>,
    val type: Map<String, TypeStyleToken>,
) {
    fun color(darkTheme: Boolean, name: String): Long =
        (if (darkTheme) dark else light)[name] ?: throw IllegalArgumentException("no colour token '$name'")

    fun category(darkTheme: Boolean, category: String): Long =
        (if (darkTheme) categoryDark else categoryLight).let { it[category] ?: it["other"] ?: 0xFF888888L }

    companion object {
        fun parse(json: String): BrandTokens {
            val root = Json.parse(json).asObject()
            fun colors(o: Map<String, Any?>): Map<String, Long> =
                o.mapNotNull { (k, v) -> (v as? String)?.takeIf { it.startsWith("#") }?.let { k to parseHexColor(it) } }.toMap()
            fun stops(o: Any?): List<Pair<Float, Long>> = o.asArray().map {
                val s = it.asObject()
                (s.double("offset") ?: 0.0).toFloat() to parseHexColor(s.string("color"))
            }
            fun numbers(o: Map<String, Any?>): Map<String, Double> =
                o.mapNotNull { (k, v) -> (v as? Number)?.let { k to it.toDouble() } }.toMap()

            val color = root["color"].asObject()
            val category = root["category"].asObject()
            val brandObj = root["brand"].asObject()
            val gradient = brandObj["gradient"].asObject()
            val motion = root["motion"].asObject()
            val typography = root["typography"].asObject()
            val scale = typography["scale"].asObject()
            return BrandTokens(
                dark = colors(color["dark"].asObject()),
                light = colors(color["light"].asObject()),
                categoryDark = category.mapNotNull { (k, v) -> v.asObject().string("dark").takeIf { it.isNotEmpty() }?.let { k to parseHexColor(it) } }.toMap(),
                categoryLight = category.mapNotNull { (k, v) -> v.asObject().string("light").takeIf { it.isNotEmpty() }?.let { k to parseHexColor(it) } }.toMap(),
                brand = colors(brandObj),
                gradient = stops(gradient["stops"]),
                gradientOnLight = stops(gradient["stopsOnLight"]),
                radius = numbers(root["radius"].asObject()),
                space = numbers(root["space"].asObject()),
                size = numbers(root["size"].asObject()),
                durations = numbers(motion["duration"].asObject()).mapValues { it.value.toInt() },
                easings = motion["easing"].asObject().mapValues { (_, v) -> v.asArray().map { (it as Number).toDouble() } },
                type = scale.mapValues { (_, v) ->
                    val o = v.asObject()
                    TypeStyleToken(
                        size = o.double("size") ?: 15.0,
                        lineHeight = o.double("lineHeight") ?: 22.0,
                        weight = (o.double("weight") ?: 400.0).toInt(),
                        letterSpacingEm = o.double("letterSpacing") ?: 0.0,
                        mono = o.string("family") == "mono",
                        uppercase = o.string("textTransform") == "uppercase",
                    )
                },
            )
        }
    }
}
