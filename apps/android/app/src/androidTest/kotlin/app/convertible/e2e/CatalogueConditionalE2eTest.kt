package app.convertible.e2e

import android.graphics.BitmapFactory
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import app.convertible.core.NativeCore
import app.convertible.e2e.fixtures.Fixtures
import app.convertible.e2e.support.E2eTest
import app.convertible.model.TargetInfo
import app.convertible.state.ItemStatus
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Media and archive conversions, written against the dynamic catalogue: each test imports a generated fixture
 * (PNG, WAV, ZIP), and runs only if this build of the core recognises it and offers a conversion to another format
 * of the same category. Otherwise it is skipped (JUnit Assume) with the reason, e.g. while only the universal
 * encodings are available for that format.
 */
@RunWith(AndroidJUnit4::class)
class CatalogueConditionalE2eTest : E2eTest() {
    private val core = NativeCore()

    /** Imports [fixture] and returns its id and the same-category targets, or skips the test. */
    private fun importAndFindTargets(fixture: String, category: String): Pair<Long, List<TargetInfo>> {
        val (id) = launchWith(fixture)
        val item = item(id)
        val source = item.sourceId
        val info = vm.state.value.catalogue[source]
        assumeTrue("core does not recognise $fixture as a $category format (detected '$source')", info?.category == category && info.readable)
        val targets = item.targets.filter { it.category == category && it.id != source && !it.compress }
        assumeTrue("core offers no $category target for '$source' yet (only ${item.targets.map { it.id }})", targets.isNotEmpty())
        return id to targets
    }

    private fun convertTo(id: Long, target: TargetInfo): File {
        chooseTarget(id, target.id)
        val done = convertAndWait(id, CONVERSION)
        val out = File(done.output.path)
        assertTrue("empty ${target.id} output", out.length() > 0)
        // The core identifies its own output as the format that was asked for.
        assertEquals(target.id, core.detectFile(out.path, out.name))
        return out
    }

    @Test
    fun imageConvertsToAnotherImageFormat() {
        val (id, targets) = importAndFindTargets(Fixtures.PNG, "image")
        val target = targets.first()
        val out = convertTo(id, target)
        // Where the platform can decode the result, the picture keeps its size.
        BitmapFactory.decodeFile(out.path)?.let {
            assertEquals(48, it.width)
            assertEquals(32, it.height)
        }
    }

    @Test
    fun imageCompressKeepsTheFormat() {
        val (id) = launchWith(Fixtures.PNG)
        val source = item(id).sourceId
        assumeTrue("core does not recognise the PNG fixture as an image", vm.state.value.catalogue[source]?.category == "image")
        val compress = item(id).targets.filter { it.compress }
        assumeTrue("core offers no Compress action for '$source' yet", compress.isNotEmpty())
        clickInQueue("compress_$id")
        waitForTag("compress_picker")
        compose.onNodeWithTag("target_${compress.first().id}").performClick()
        waitForGone("compress_picker")
        clickInQueue("convert_$id")
        val done = awaitStatus<ItemStatus.Done>(id, CONVERSION)
        val out = File(done.output.path)
        assertEquals(compress.first().id, core.detectFile(out.path, out.name))
        assertNotNull("compressed image decodes", BitmapFactory.decodeFile(out.path))
    }

    @Test
    fun audioConvertsToAnotherAudioFormat() {
        val (id, targets) = importAndFindTargets(Fixtures.WAV, "audio")
        // Prefer a lossless target so the content can be compared.
        val target = targets.firstOrNull { !it.lossy } ?: targets.first()
        val out = convertTo(id, target)
        if (target.id == "wav") assertEquals("RIFF", String(out.readBytes().copyOf(4), Charsets.US_ASCII))
    }

    @Test
    fun archiveConvertsToAnotherArchiveFormat() {
        val (id, targets) = importAndFindTargets(Fixtures.ZIP, "archive")
        val out = convertTo(id, targets.first())
        assertTrue(out.length() > 0)
    }
}
