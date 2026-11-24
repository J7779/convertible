package app.convertible.ui.components

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathMeasure
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import app.convertible.brand.CubicBezier
import app.convertible.brand.Layer
import app.convertible.brand.MotionPaint
import app.convertible.brand.MotionSpec
import app.convertible.brand.Storyboard
import app.convertible.ui.theme.AppTheme
import app.convertible.ui.theme.Palette

/** Paths and measuring state for drawing the mark from the storyboard geometry (one per composable). */
class MarkGeometry(val spec: MotionSpec) {
    val centerline: Path = PathParser().parsePathString(spec.centerline).toPath()
    private val measure = PathMeasure().apply { setPath(centerline, false) }
    val length: Float = measure.length
    private val segment = Path()

    /** The part of the centerline between two fractions of its length. */
    fun trimmed(start: Float, end: Float): Path? {
        val s = start.coerceIn(0f, 1f)
        val e = end.coerceIn(0f, 1f)
        if (e - s <= 0.0005f) return null
        segment.reset()
        measure.getSegment(s * length, e * length, segment, true)
        return segment
    }
}

private fun paintBrush(paint: MotionPaint?, colors: Palette): Brush = when (paint) {
    is MotionPaint.Linear -> Brush.linearGradient(
        colorStops = paint.stops.map { it.first to colors.markColor(it.second) }.toTypedArray(),
        start = Offset(paint.x1, paint.y1),
        end = Offset(paint.x2, paint.y2),
    )
    is MotionPaint.Solid -> SolidColor(colors.markColor(paint.color))
    null -> SolidColor(colors.accent)
}

/** Scales the 1024-unit storyboard canvas uniformly into this draw scope, centred. */
private inline fun DrawScope.inCanvasUnits(block: DrawScope.() -> Unit) {
    val s = minOf(size.width, size.height) / 1024f
    val dx = (size.width - 1024f * s) / 2f
    val dy = (size.height - 1024f * s) / 2f
    withTransform({
        translate(dx, dy)
        scale(s, s, Offset.Zero)
    }) { block() }
}

/** Draws one storyboard layer with its group's transform and opacity. [override] replaces property values. */
private fun DrawScope.drawLayer(
    board: Storyboard,
    layer: Layer,
    timeMs: Double,
    geo: MarkGeometry,
    colors: Palette,
    override: (String) -> Double? = { null },
) {
    fun v(l: Layer, p: String) = (if (l === layer) override(p) else null) ?: board.value(l, p, timeMs)
    val parent = board.parentOf(layer)
    val alpha = (v(layer, "opacity") * (parent?.let { v(it, "opacity") } ?: 1.0)).toFloat().coerceIn(0f, 1f)
    if (alpha <= 0.001f) return
    val scale = v(layer, "scale").toFloat()
    if (scale <= 0.0001f) return
    val brush = paintBrush(layer.paint?.let { geo.spec.paints[it] }, colors)
    withTransform({
        if (parent != null) {
            val pivot = parent.pivot?.let { Offset(it.first, it.second) } ?: Offset(512f, 512f)
            val ps = v(parent, "scale").toFloat()
            rotate(v(parent, "rotationDeg").toFloat(), pivot)
            scale(ps, ps, pivot)
        }
        layer.pivot?.let { scale(scale, scale, Offset(it.first, it.second)) }
    }) {
        when (layer.type) {
            "stroke" -> geo.trimmed(v(layer, "trimStart").toFloat(), v(layer, "trimEnd").toFloat())?.let { path ->
                drawPath(
                    path = path,
                    brush = brush,
                    alpha = alpha,
                    style = Stroke(width = geo.spec.strokeWidth, cap = StrokeCap.Butt, join = StrokeJoin.Miter, miter = 4f),
                )
            }
            "shape" -> when (layer.geometry) {
                "seed" -> {
                    val (cx, cy, r) = geo.spec.seed
                    drawCircle(brush = brush, radius = r, center = Offset(cx, cy), alpha = alpha)
                }
                "endSquare" -> {
                    val (x, y, w, h) = geo.spec.endSquare
                    drawRect(brush = brush, topLeft = Offset(x, y), size = Size(w, h), alpha = alpha)
                }
            }
        }
    }
}

/** Draws every layer of a storyboard at [timeMs]. */
fun DrawScope.drawStoryboard(board: Storyboard, timeMs: Double, geo: MarkGeometry, colors: Palette) = inCanvasUnits {
    for (layer in board.drawables) drawLayer(board, layer, timeMs, geo, colors)
}

/** The static logo (= the intro's last frame). */
@Composable
fun StaticMark(modifier: Modifier = Modifier, size: Dp = 32.dp) {
    val spec = AppTheme.storyboard
    val colors = AppTheme.colors
    val geo = remember(spec) { MarkGeometry(spec) }
    Canvas(modifier.size(size)) {
        drawStoryboard(spec.intro, spec.intro.durationMs.toDouble(), geo, colors)
    }
}

/**
 * Determinate progress drawn with the mark: a faint track of the whole mark, the stroke trimmed to [fraction],
 * and the seed once anything has happened. At 1 it is the finished logo; [done] plays the square landing ripple
 * once. [fraction] null shows the looping spinner instead.
 */
@Composable
fun MarkProgress(fraction: Float?, modifier: Modifier = Modifier, size: Dp = 40.dp, done: Boolean = false) {
    if (fraction == null && !done) {
        MarkSpinner(modifier, size)
        return
    }
    val spec = AppTheme.storyboard
    val colors = AppTheme.colors
    val motion = AppTheme.motion
    val geo = remember(spec) { MarkGeometry(spec) }
    val target = if (done) 1f else (fraction ?: 0f)
    val p by animateFloatAsState(target, tween(motion.base, easing = motion.standard), label = "progress")
    val ripple = remember { Animatable(0f) }
    LaunchedEffect(done) {
        if (done && !motion.reduced) {
            ripple.snapTo(0f)
            ripple.animateTo(1f, tween(480, easing = LinearEasing))
        }
    }
    val spinner = spec.spinner
    val trackStroke = spinner.layer("trackStroke")
    val trackSeed = spinner.layer("trackSeed")
    val runner = spinner.layer("runner")
    val seed = spinner.layer("seed")
    val rippleSquare = spec.intro.layer("rippleSquare")
    Canvas(
        modifier
            .size(size)
            .semantics { progressBarRangeInfo = ProgressBarRangeInfo(target, 0f..1f) },
    ) {
        inCanvasUnits {
            // The track layers inherit the faint opacity of their "track" group.
            trackStroke?.let { drawLayer(spinner, it, 0.0, geo, colors) }
            trackSeed?.let { drawLayer(spinner, it, 0.0, geo, colors) }
            if (rippleSquare != null && ripple.value in 0.001f..0.999f) {
                // The intro's landing ripple (1180..1720 ms) compressed into 480 ms.
                val t = 1180.0 + ripple.value * 540.0
                drawLayer(spec.intro, rippleSquare, t, geo, colors) { prop -> if (prop == "rotationDeg") 0.0 else null }
            }
            runner?.let {
                drawLayer(spinner, it, 0.0, geo, colors) { prop ->
                    when (prop) {
                        "trimStart" -> 0.0
                        "trimEnd" -> p.toDouble()
                        else -> null
                    }
                }
            }
            seed?.let { drawLayer(spinner, it, 0.0, geo, colors) { prop -> if (prop == "scale") (if (p > 0f) 1.0 else 0.0) else null } }
        }
    }
}

/** The looping "converting" indicator; with reduced motion a slow opacity pulse of the finished mark. */
@Composable
fun MarkSpinner(modifier: Modifier = Modifier, size: Dp = 40.dp) {
    val spec = AppTheme.storyboard
    val colors = AppTheme.colors
    val reduced = AppTheme.motion.reduced
    val geo = remember(spec) { MarkGeometry(spec) }
    val board = spec.spinner
    val transition = rememberInfiniteTransition(label = "spinner")
    val time by transition.animateFloat(
        initialValue = 0f,
        targetValue = board.durationMs.toFloat(),
        animationSpec = infiniteRepeatable(tween(board.durationMs, easing = LinearEasing)),
        label = "spinnerTime",
    )
    val inOut = remember(spec) { spec.easings["inOut"] ?: CubicBezier(0.65, 0.0, 0.35, 1.0) }
    val pulse by transition.animateFloat(
        initialValue = 1f,
        targetValue = 0.45f,
        animationSpec = infiniteRepeatable(tween(800, easing = { inOut.transform(it.toDouble()).toFloat() }), RepeatMode.Reverse),
        label = "spinnerPulse",
    )
    Canvas(modifier.size(size).semantics { progressBarRangeInfo = ProgressBarRangeInfo.Indeterminate }) {
        if (reduced) {
            inCanvasUnits {
                val track = board.layer("track")
                for (layer in board.drawables) {
                    val isTrack = track?.children?.contains(layer.id) == true
                    drawLayer(board, layer, 0.0, geo, colors) { prop ->
                        when {
                            isTrack -> null
                            prop == "trimStart" -> 0.0
                            prop == "trimEnd" -> 1.0
                            prop == "scale" -> 1.0
                            prop == "opacity" -> pulse.toDouble()
                            else -> null
                        }
                    }
                }
            }
        } else {
            drawStoryboard(board, time.toDouble(), geo, colors)
        }
    }
}
