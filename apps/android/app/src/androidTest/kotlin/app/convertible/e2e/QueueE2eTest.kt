package app.convertible.e2e

import androidx.compose.ui.test.hasAnyAncestor
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextInput
import androidx.test.ext.junit.runners.AndroidJUnit4
import app.convertible.core.CoreError
import app.convertible.e2e.fixtures.Fixtures
import app.convertible.e2e.fixtures.ShareFixtureProvider
import app.convertible.e2e.support.Device
import app.convertible.e2e.support.E2eTest
import app.convertible.e2e.support.SystemUi
import app.convertible.state.ItemStatus
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.Base64

/** The multi-file queue: "Convert all to…" for a group, "Convert N files", "Save all", cancel and cancel all. */
@RunWith(AndroidJUnit4::class)
class QueueE2eTest : E2eTest() {
    private fun sha256Of(done: ItemStatus.Done): String =
        Regex("""SHA-256: ([0-9a-fA-F]+)""").find(String(outputBytes(done)))?.groupValues?.get(1)?.lowercase()
            ?: throw AssertionError("no SHA-256 in report")

    private fun hex(bytes: ByteArray) = bytes.joinToString("") { "%02x".format(it) }

    /** Sets every file of the group [sourceId] to [target] through "Convert all to…". */
    private fun convertAllTo(sourceId: String, target: String, search: String = target) {
        clickInQueue("group_$sourceId")
        compose.onNode(hasTestTag("group_set_all") and hasAnyAncestor(hasTestTag("group_$sourceId"))).performClick()
        waitForTag("target_picker")
        compose.onNodeWithTag("picker_search").performTextInput(search)
        waitForTag("target_$target")
        compose.onNodeWithTag("target_$target").performClick()
        waitForGone("target_picker")
    }

    @Test
    fun convertAllToThenConvertEverythingAndSaveAll() {
        val names = arrayOf(Fixtures.NOTE, Fixtures.NOTE_2, Fixtures.NOTE_3)
        val ids = launchWith(*names)
        assumeTrue(items.all { item -> item.targets.any { it.id == "base64" } })
        convertAllTo("txt", "base64")
        compose.waitUntil(MEDIUM) { ids.all { item(it).target?.id == "base64" } }
        // The group bar confirms the shared choice.
        compose.waitUntil(MEDIUM) {
            compose.onAllNodes(hasText("BASE64", substring = true) and hasAnyAncestor(hasTestTag("group_txt")), useUnmergedTree = true)
                .fetchSemanticsNodes().isNotEmpty()
        }
        compose.onNodeWithTag("convert_all").performClick()
        ids.forEach { awaitStatus<ItemStatus.Done>(it) }
        ids.zip(names).forEach { (id, name) ->
            val done = item(id).status as ItemStatus.Done
            assertArrayEquals(name, Fixtures.bytes(name), Base64.getMimeDecoder().decode(String(outputBytes(done)).trim()))
        }

        // Save all into a folder picked in the system dialog; read every document back.
        waitForTag("save_all")
        compose.onNodeWithTag("save_all").performClick()
        SystemUi.chooseFixtureFolder()
        val outputs = ids.map { (item(it).status as ItemStatus.Done).output }
        compose.waitUntil(LONG) { SystemUi.savedDocuments().size == outputs.size }
        assertEquals(outputs.map { it.name }.sorted(), SystemUi.savedDocuments())
        outputs.forEach { output ->
            val saved = Device.app.contentResolver.openInputStream(ShareFixtureProvider.savedUri(output.name))!!.use { it.readBytes() }
            assertArrayEquals(output.name, File(output.path).readBytes(), saved)
        }
        compose.waitUntil(MEDIUM) { compose.onAllNodes(hasText("Saved ${outputs.size} files")).fetchSemanticsNodes().isNotEmpty() }
    }

    @Test
    fun cancelDuringALongConversionThenConvertAgain() {
        val (id) = launchWith(Fixtures.LARGE)
        assumeTrue(item(id).targets.any { it.id == "checksums" })
        chooseTarget(id, "checksums", search = "checksum")
        clickInQueue("convert_$id")
        compose.waitUntil(LONG) { item(id).status is ItemStatus.Converting }
        clickInQueue("cancel_$id")
        val failed = awaitStatus<ItemStatus.Failed>(id, CONVERSION)
        assertTrue("expected cancelled, got ${failed.error}", failed.error is CoreError.Cancelled)
        // Cancelled is not an error: neutral title, "Convert" to start again, and no partial output left behind.
        queueNode("retry_$id")
        compose.onNode(hasText(Device.app.getString(app.convertible.R.string.error_cancelled)) and hasAnyAncestor(hasTestTag("card_$id")), useUnmergedTree = true).assertExists()
        compose.onNode(hasText(Device.app.getString(app.convertible.R.string.action_convert)) and hasAnyAncestor(hasTestTag("retry_$id")), useUnmergedTree = true).assertExists()
        val outDir = File(Device.app.cacheDir, "work/out/$id")
        assertTrue("partial output left: ${outDir.list()?.toList()}", outDir.list().isNullOrEmpty())

        clickInQueue("retry_$id")
        val done = awaitStatus<ItemStatus.Done>(id, CONVERSION)
        assertEquals(hex(Fixtures.sha256(Fixtures.LARGE)), sha256Of(done))
    }

    @Test
    fun cancelAllStopsEveryConversionThenConvertAllRestarts() {
        val ids = launchWith(Fixtures.LARGE, Fixtures.LARGE_2)
        assumeTrue(items.all { item -> item.targets.any { it.id == "checksums" } })
        val source = item(ids[0]).sourceId!!
        convertAllTo(source, "checksums", search = "checksum")
        compose.waitUntil(MEDIUM) { ids.all { item(it).target?.id == "checksums" } }
        compose.onNodeWithTag("convert_all").performClick()
        compose.waitUntil(LONG) { ids.all { item(it).status is ItemStatus.Converting } }
        waitForTag("cancel_all")
        compose.onNodeWithTag("cancel_all").performClick()
        ids.forEach { id ->
            val failed = awaitStatus<ItemStatus.Failed>(id, CONVERSION)
            assertTrue(failed.error is CoreError.Cancelled)
        }
        waitForGone("cancel_all")
        waitForTag("convert_all")
        compose.onNodeWithTag("convert_all").performClick()
        val expected = listOf(Fixtures.LARGE, Fixtures.LARGE_2).map { hex(Fixtures.sha256(it)) }
        ids.zip(expected).forEach { (id, sha) -> assertEquals(sha, sha256Of(awaitStatus<ItemStatus.Done>(id, CONVERSION))) }
    }

    @Test
    fun removingAFileDropsItFromTheQueueAndTheWorkspace() {
        val ids = launchWith(Fixtures.NOTE, Fixtures.DATA)
        val input = File(item(ids[0]).inputPath!!)
        assertTrue(input.isFile)
        clickInQueue("remove_${ids[0]}")
        compose.waitUntil(MEDIUM) { items.map { it.id } == listOf(ids[1]) && !input.exists() }
        waitForGone("card_${ids[0]}")
    }
}
