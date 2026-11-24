package app.convertible.brand

import app.convertible.json.Json
import app.convertible.json.asArray
import app.convertible.json.asObject
import app.convertible.json.bool
import app.convertible.json.double
import app.convertible.json.string
import app.convertible.json.strings
import kotlin.math.abs

/** CSS-style cubic-bezier easing (control points P1 = (x1, y1), P2 = (x2, y2)); y may overshoot. */
class CubicBezier(private val x1: Double, private val y1: Double, private val x2: Double, private val y2: Double) {
    private fun bx(t: Double) = ((1 - 3 * x2 + 3 * x1) * t + (3 * x2 - 6 * x1)) * t * t + 3 * x1 * t
    private fun by(t: Double) = ((1 - 3 * y2 + 3 * y1) * t + (3 * y2 - 6 * y1)) * t * t + 3 * y1 * t
    private fun dx(t: Double) = 3 * (1 - 3 * x2 + 3 * x1) * t * t + 2 * (3 * x2 - 6 * x1) * t + 3 * x1

    fun transform(fraction: Double): Double {
        if (fraction <= 0.0) return 0.0
        if (fraction >= 1.0) return 1.0
        // Newton-Raphson, falling back to bisection.
        var t = fraction
        repeat(8) {
            val err = bx(t) - fraction
            if (abs(err) < 1e-7) return by(t)
            val d = dx(t)
            if (abs(d) < 1e-6) return@repeat
            t -= err / d
        }
        var lo = 0.0
        var hi = 1.0
        t = fraction
        repeat(40) {
            val x = bx(t)
            if (abs(x - fraction) < 1e-7) return by(t)
            if (x < fraction) lo = t else hi = t
            t = (lo + hi) / 2
        }
        return by(t)
    }

    companion object {
        fun of(points: List<Double>): CubicBezier = CubicBezier(points[0], points[1], points[2], points[3])
    }
}

data class Keyframe(val t: Double, val value: Double, val easing: String?)

/** A keyframe track; see `conventions` in animation.json. */
class Track(val property: String, val keyframes: List<Keyframe>) {
    fun valueAt(timeMs: Double, easings: Map<String, CubicBezier>): Double {
        val first = keyframes.first()
        if (timeMs <= first.t) return first.value
        val last = keyframes.last()
        if (timeMs >= last.t) return last.value
        for (i in 0 until keyframes.size - 1) {
            val a = keyframes[i]
            val b = keyframes[i + 1]
            if (timeMs >= a.t && timeMs < b.t) {
                val easing = a.easing ?: return a.value // no easing: hold until the next keyframe
                val curve = easings[easing] ?: return a.value
                val span = b.t - a.t
                val p = if (span <= 0) 1.0 else (timeMs - a.t) / span
                return a.value + (b.value - a.value) * curve.transform(p)
            }
        }
        return last.value
    }
}

sealed interface MotionPaint {
    data class Solid(val color: Long) : MotionPaint
    data class Linear(val x1: Float, val y1: Float, val x2: Float, val y2: Float, val stops: List<Pair<Float, Long>>) : MotionPaint
}

data class Layer(
    val id: String,
    val type: String,
    val geometry: String?,
    val paint: String?,
    val pivot: Pair<Float, Float>?,
    val children: List<String>,
    val base: Map<String, Double>,
    val tracks: Map<String, Track>,
)

/** One timeline (intro or spinner). */
class Storyboard(val durationMs: Int, val loop: Boolean, val layers: List<Layer>, private val easings: Map<String, CubicBezier>) {
    private val parents: Map<String, Layer> = buildMap {
        for (l in layers) if (l.type == "group") for (c in l.children) put(c, l)
    }

    fun parentOf(layer: Layer): Layer? = parents[layer.id]

    /** Value of [property] of [layer] at [timeMs] (track, else base, else the property's neutral value). */
    fun value(layer: Layer, property: String, timeMs: Double): Double =
        layer.tracks[property]?.valueAt(timeMs, easings) ?: layer.base[property] ?: neutral(property)

    /** Drawable (non-group) layers back to front. */
    val drawables: List<Layer> get() = layers.filter { it.type != "group" }

    fun layer(id: String): Layer? = layers.firstOrNull { it.id == id }

    companion object {
        fun neutral(property: String): Double = when (property) {
            "scale", "opacity", "trimEnd" -> 1.0
            else -> 0.0
        }
    }
}

/**
 * The motion storyboard from `assets/brand/animation.json`: geometry, paints, easings and the intro and spinner
 * timelines. Parsed into plain data; drawing happens in the UI layer.
 */
class MotionSpec(
    val easings: Map<String, CubicBezier>,
    val easingPoints: Map<String, List<Double>>,
    val paints: Map<String, MotionPaint>,
    val centerline: String,
    val markOutline: String,
    val strokeWidth: Float,
    val seed: Triple<Float, Float, Float>,
    val endSquare: List<Float>,
    val intro: Storyboard,
    val spinner: Storyboard,
) {
    companion object {
        fun parse(json: String): MotionSpec {
            val root = Json.parse(json).asObject()
            val easingPoints = root["easings"].asObject().mapValues { (_, v) -> v.asArray().map { (it as Number).toDouble() } }
            val easings = easingPoints.mapValues { CubicBezier.of(it.value) }
            val paints = root["paints"].asObject().mapValues { (_, v) ->
                val o = v.asObject()
                when (o.string("type")) {
                    "linearGradient" -> MotionPaint.Linear(
                        x1 = (o.double("x1") ?: 0.0).toFloat(),
                        y1 = (o.double("y1") ?: 0.0).toFloat(),
                        x2 = (o.double("x2") ?: 0.0).toFloat(),
                        y2 = (o.double("y2") ?: 0.0).toFloat(),
                        stops = o["stops"].asArray().map {
                            val s = it.asObject()
                            (s.double("offset") ?: 0.0).toFloat() to parseHexColor(s.string("color"))
                        },
                    )
                    else -> MotionPaint.Solid(parseHexColor(o.string("color", "#000000")))
                }
            }
            val geometry = root["geometry"].asObject()
            val centerline = geometry["centerline"].asObject()
            val seed = geometry["seed"].asObject()
            val square = geometry["endSquare"].asObject()
            fun timeline(key: String): Storyboard {
                val o = root[key].asObject()
                return Storyboard(
                    durationMs = (o.double("durationMs") ?: 0.0).toInt(),
                    loop = o.bool("loop"),
                    layers = o["layers"].asArray().map { parseLayer(it.asObject()) },
                    easings = easings,
                )
            }
            return MotionSpec(
                easings = easings,
                easingPoints = easingPoints,
                paints = paints,
                centerline = centerline.string("d"),
                markOutline = geometry["markOutline"].asObject().string("d"),
                strokeWidth = (centerline.double("strokeWidth") ?: 200.0).toFloat(),
                seed = Triple((seed.double("cx") ?: 0.0).toFloat(), (seed.double("cy") ?: 0.0).toFloat(), (seed.double("r") ?: 0.0).toFloat()),
                endSquare = listOf("x", "y", "width", "height").map { (square.double(it) ?: 0.0).toFloat() },
                intro = timeline("intro"),
                spinner = timeline("spinner"),
            )
        }

        private fun parseLayer(o: Map<String, Any?>): Layer {
            val pivot = o["pivot"].asArray().mapNotNull { (it as? Number)?.toFloat() }
            return Layer(
                id = o.string("id"),
                type = o.string("type"),
                geometry = o["geometry"] as? String,
                paint = (o["stroke"] as? String) ?: (o["fill"] as? String),
                pivot = if (pivot.size == 2) pivot[0] to pivot[1] else null,
                children = o.strings("children"),
                base = o["base"].asObject().mapNotNull { (k, v) -> (v as? Number)?.let { k to it.toDouble() } }.toMap(),
                tracks = o["tracks"].asArray().map { it.asObject() }.associate { t ->
                    val property = t.string("property")
                    property to Track(
                        property,
                        t["keyframes"].asArray().map { k ->
                            val ko = k.asObject()
                            Keyframe(ko.double("t") ?: 0.0, ko.double("value") ?: 0.0, ko["easing"] as? String)
                        },
                    )
                },
            )
        }
    }
}
