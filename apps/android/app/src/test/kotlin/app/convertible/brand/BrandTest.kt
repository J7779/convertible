package app.convertible.brand

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/** Parses the repository's real tokens.json and animation.json (located through a Gradle system property). */
class BrandTest {
    private val brandDir = File(System.getProperty("convertible.brandDir") ?: "../../../assets/brand")
    private val tokens by lazy { BrandTokens.parse(File(brandDir, "tokens.json").readText()) }
    private val motion by lazy { MotionSpec.parse(File(brandDir, "animation.json").readText()) }

    @Test
    fun parsesHexColours() {
        assertEquals(0xFF7C6CFFL, parseHexColor("#7C6CFF"))
        assertEquals(0xCC05060AL, parseHexColor("#05060ACC")) // CSS order: alpha last
    }

    @Test
    fun tokensHaveBothThemesAndEveryCategory() {
        val keys = listOf("background", "surface", "surfaceElevated", "textPrimary", "textSecondary", "accent", "accentFill", "onAccent", "error", "focusRing")
        for (k in keys) {
            assertTrue("dark $k", tokens.dark.containsKey(k))
            assertTrue("light $k", tokens.light.containsKey(k))
        }
        assertEquals(0xFF0D0F16L, tokens.color(true, "background"))
        assertEquals(0xFFF6F6FAL, tokens.color(false, "background"))
        val categories = listOf("image", "audio", "video", "document", "ebook", "spreadsheet", "presentation", "data", "archive",
            "font", "subtitle", "model3d", "geo", "pim", "code", "encoding", "vector", "other")
        for (c in categories) {
            assertTrue(c, tokens.categoryDark.containsKey(c) && tokens.categoryLight.containsKey(c))
        }
        assertEquals(tokens.category(true, "other"), tokens.category(true, "not-a-category"))
    }

    @Test
    fun tokensCarryScalesAndMotion() {
        assertEquals(16.0, tokens.radius["lg"]!!, 0.0)
        assertEquals(24.0, tokens.space["7"]!!, 0.0)
        assertEquals(220, tokens.durations["base"])
        assertEquals(listOf(0.2, 0.0, 0.0, 1.0), tokens.easings["standard"])
        assertEquals(3, tokens.gradient.size)
        val label = tokens.type.getValue("formatLabel")
        assertTrue(label.mono && label.uppercase)
        assertEquals(600, tokens.type.getValue("title1").weight)
    }

    @Test
    fun cubicBezierHitsEndpointsAndOvershoots() {
        val standard = motion.easings.getValue("standard")
        assertEquals(0.0, standard.transform(0.0), 1e-9)
        assertEquals(1.0, standard.transform(1.0), 1e-9)
        var last = 0.0
        for (i in 1..100) {
            val y = standard.transform(i / 100.0)
            assertTrue("monotonic", y >= last - 1e-9)
            last = y
        }
        val overshoot = motion.easings.getValue("overshoot")
        assertTrue((1..99).any { overshoot.transform(it / 100.0) > 1.0 })
        // Linear control points give the identity.
        val linear = CubicBezier(0.0, 0.0, 1.0, 1.0)
        assertEquals(0.37, linear.transform(0.37), 1e-6)
    }

    @Test
    fun introStartsEmptyAndEndsOnTheStaticLogo() {
        val intro = motion.intro
        assertEquals(1800, intro.durationMs)
        val seed = intro.layer("seed")!!
        val trace = intro.layer("trace")!!
        val ghost = intro.layer("ghost")!!
        val stage = intro.layer("stage")!!
        assertEquals(0.0, intro.value(seed, "scale", 0.0), 1e-9)
        assertEquals(0.0, intro.value(trace, "trimEnd", 0.0), 1e-9)
        // trace runs from 260 to 1240 ms
        assertEquals(0.0, intro.value(trace, "trimEnd", 200.0), 1e-9)
        assertTrue(intro.value(trace, "trimEnd", 750.0) in 0.3..0.95)
        val end = intro.durationMs.toDouble()
        assertEquals(1.0, intro.value(seed, "scale", end), 1e-9)
        assertEquals(1.0, intro.value(trace, "trimEnd", end), 1e-9)
        assertEquals(0.0, intro.value(ghost, "opacity", end), 1e-9)
        assertEquals(1.0, intro.value(stage, "scale", end), 1e-9)
        assertEquals(0.0, intro.value(stage, "rotationDeg", end), 1e-9)
        assertEquals(-8.0, intro.value(stage, "rotationDeg", 0.0), 1e-9)
        // Stage is the parent group of the drawable layers.
        assertEquals("stage", intro.parentOf(trace)?.id)
        assertEquals(listOf("rippleRound", "rippleSquare", "ghost", "trace", "seed"), intro.drawables.map { it.id })
    }

    @Test
    fun ghostOpacityHoldsWithoutEasing() {
        val ghost = motion.intro.layer("ghost")!!
        // 0 -> 1000 ms has no easing on the first keyframe: hold 0.26.
        assertEquals(0.26, motion.intro.value(ghost, "opacity", 500.0), 1e-9)
        assertTrue(motion.intro.value(ghost, "opacity", 1150.0) < 0.26)
    }

    @Test
    fun spinnerLoopsAndTrackIsAFaintGroup() {
        val spinner = motion.spinner
        assertTrue(spinner.loop)
        assertEquals(1400, spinner.durationMs)
        val runner = spinner.layer("runner")!!
        assertEquals(1.0, spinner.value(runner, "trimEnd", 700.0), 1e-9)
        assertEquals(0.0, spinner.value(runner, "trimStart", 700.0), 1e-9)
        assertEquals(1.0, spinner.value(runner, "trimStart", 1400.0), 1e-9)
        val track = spinner.layer("track")!!
        assertEquals(0.16, spinner.value(track, "opacity", 0.0), 1e-9)
        assertEquals("track", spinner.parentOf(spinner.layer("trackStroke")!!)?.id)
    }

    @Test
    fun geometryMatchesTheMark() {
        assertEquals(200f, motion.strokeWidth)
        assertEquals(Triple(772f, 260f, 100f), motion.seed)
        assertEquals(listOf(664f, 664f, 200f, 200f), motion.endSquare)
        assertTrue(motion.centerline.startsWith("M772 260"))
        val flux = motion.paints["flux"] as MotionPaint.Linear
        assertEquals(360f, flux.y1)
        assertEquals(664f, flux.y2)
        assertEquals(3, flux.stops.size)
    }
}
