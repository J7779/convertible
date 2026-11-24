package app.convertible.e2e

import android.content.Intent
import android.net.Uri
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.hasAnyAncestor
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.isHeading
import androidx.compose.ui.test.isSelected
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performSemanticsAction
import androidx.compose.ui.test.performTextClearance
import androidx.compose.ui.test.performTextInput
import androidx.test.ext.junit.runners.AndroidJUnit4
import app.convertible.MainActivity
import app.convertible.core.CoreError
import app.convertible.e2e.fixtures.FixtureDocumentsProvider
import app.convertible.e2e.fixtures.Fixtures
import app.convertible.e2e.support.Device
import app.convertible.e2e.support.E2eTest
import app.convertible.e2e.support.SystemUi
import app.convertible.model.SizeFormat
import app.convertible.state.ItemStatus
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.security.MessageDigest
import java.util.Base64
import java.util.Locale
import java.util.zip.CRC32

/**
 * Single-file conversions through the UI and the real core, with the produced bytes verified independently
 * (java.util.Base64, MessageDigest, CRC32, Charsets) and the result saved through the system save dialog and read
 * back from the storage provider.
 */
@RunWith(AndroidJUnit4::class)
class ConversionE2eTest : E2eTest() {
    private fun assumeTarget(id: Long, target: String) =
        assumeTrue("core offers no '$target' target for ${item(id).sourceId}", item(id).targets.any { it.id == target })

    /** A node inside the options panel of item [id], option [key]. */
    private fun optionChild(id: Long, key: String, matcher: SemanticsMatcher) =
        compose.onNode(matcher and hasAnyAncestor(hasTestTag("option_${id}_$key")), useUnmergedTree = true)

    @Test
    fun base64ShowsSizesAndSavesTheRightBytesUnderTheRightName() {
        val (id) = launchWith(Fixtures.NOTE)
        assumeTarget(id, "base64")
        chooseTarget(id, "base64")
        val done = convertAndWait(id)
        val output = done.output

        // Bytes: the Base64 text decodes to the input.
        val text = String(outputBytes(done), Charsets.US_ASCII)
        assertArrayEquals(Fixtures.bytes(Fixtures.NOTE), Base64.getMimeDecoder().decode(text.trim()))
        assertEquals("note.b64", output.name)

        // Result: output name, before -> after sizes and the size change.
        queueNode("save_$id")
        compose.onNode(hasText(output.name) and hasAnyAncestor(hasTestTag("card_$id")), useUnmergedTree = true).assertExists()
        val sizes = "${SizeFormat.format(item(id).size)} → ${SizeFormat.format(output.size)}"
        compose.onNode(hasText(sizes) and hasAnyAncestor(hasTestTag("card_$id")), useUnmergedTree = true).assertExists()
        compose.onNode(hasText("larger", substring = true) and hasAnyAncestor(hasTestTag("card_$id")), useUnmergedTree = true).assertExists()

        // Save through the system dialog: proposed name, stored name (MIME type consistent with the extension) and bytes.
        clickInQueue("save_$id")
        assertEquals(output.name, SystemUi.saveToFixtures())
        compose.waitUntil(MEDIUM) { output.name in SystemUi.savedDocuments() }
        assertEquals(listOf(output.name), SystemUi.savedDocuments())
        val saved = Device.app.contentResolver.openInputStream(FixtureDocumentsProvider.savedDocumentUri(output.name))!!.use { it.readBytes() }
        assertArrayEquals(outputBytes(done), saved)
        compose.waitUntil(MEDIUM) { compose.onAllNodes(hasText("Saved ${output.name}")).fetchSemanticsNodes().isNotEmpty() }
    }

    @Test
    fun hexAndChecksumReportMatchIndependentImplementations() {
        val (id) = launchWith(Fixtures.DATA)
        val input = Fixtures.bytes(Fixtures.DATA)
        assumeTarget(id, "hex")
        chooseTarget(id, "hex")
        // A boolean option rendered from the schema: upper-case digits.
        queueNode("option_${id}_uppercase").performClick()
        compose.waitUntil(MEDIUM) { item(id).values["uppercase"] != null }
        val hex = String(outputBytes(convertAndWait(id)), Charsets.US_ASCII).filterNot { it.isWhitespace() }
        assertEquals(input.joinToString("") { "%02X".format(it) }, hex)

        // "Convert again" keeps the file; switch it to a checksum report.
        clickInQueue("again_$id")
        awaitStatus<ItemStatus.Ready>(id)
        assumeTarget(id, "checksums")
        chooseTarget(id, "checksums", search = "checksum")
        val report = String(outputBytes(convertAndWait(id)), Charsets.UTF_8)
        val values = Regex("""^([A-Za-z0-9-]+): ([0-9a-fA-F]+)$""", RegexOption.MULTILINE).findAll(report)
            .associate { it.groupValues[1] to it.groupValues[2].lowercase(Locale.ROOT) }
        fun digest(alg: String) = MessageDigest.getInstance(alg).digest(input).joinToString("") { "%02x".format(it) }
        assertEquals(digest("SHA-256"), values["SHA-256"])
        assertEquals(digest("SHA-1"), values["SHA-1"])
        assertEquals(digest("MD5"), values["MD5"])
        assertEquals("%08x".format(CRC32().apply { update(input) }.value), values["CRC-32"])
        assertTrue(report.contains("Size: ${input.size} bytes"))
    }

    @Test
    fun compressReEncodesTextAsUtf16WithOptionsFromTheSchema() {
        val (id) = launchWith(Fixtures.NOTE)
        val compress = item(id).targets.filter { it.compress }
        assumeTrue("core offers no Compress target for text", compress.isNotEmpty())
        clickInQueue("compress_$id")
        waitForTag("compress_picker")
        compose.onNodeWithTag("target_${compress.first().id}").performClick()
        waitForGone("compress_picker")
        compose.waitUntil(MEDIUM) { item(id).target?.compress == true && item(id).schema.isNotEmpty() }
        assumeTrue("text compress target has an encoding option", item(id).schema.any { it.key == "encoding" && "utf-16le" in it.choices })

        queueNode("option_${id}_encoding")
        optionChild(id, "encoding", hasText("utf-16le")).performClick()
        compose.waitUntil(MEDIUM) { item(id).values["encoding"] != null }
        val le = outputBytes(convertAndWait(id))
        assertArrayEquals(Fixtures.NOTE_TEXT.toByteArray(Charsets.UTF_16LE), le)

        // Big-endian with a byte order mark.
        clickInQueue("again_$id")
        awaitStatus<ItemStatus.Ready>(id)
        queueNode("option_${id}_encoding")
        optionChild(id, "encoding", hasText("utf-16be")).performClick()
        queueNode("option_${id}_bom").performClick()
        compose.waitUntil(MEDIUM) { item(id).values.size >= 2 }
        val be = outputBytes(convertAndWait(id))
        assertArrayEquals(byteArrayOf(0xFE.toByte(), 0xFF.toByte()) + Fixtures.NOTE_TEXT.toByteArray(Charsets.UTF_16BE), be)
    }

    @Test
    fun base64OptionsChangeTheOutput() {
        val (id) = launchWith(Fixtures.DATA)
        val input = Fixtures.bytes(Fixtures.DATA)
        assumeTarget(id, "base64")
        chooseTarget(id, "base64")
        val wrapped = String(outputBytes(convertAndWait(id)), Charsets.US_ASCII)
        val lines = wrapped.trimEnd('\n').split('\n')
        assertTrue("default output is wrapped", lines.size > 1)
        assertEquals(Base64.getEncoder().encodeToString(input), lines.joinToString(""))

        clickInQueue("again_$id")
        awaitStatus<ItemStatus.Ready>(id)
        // Slider (set like TalkBack would) and switch, both generated from the options schema.
        queueNode("option_${id}_line_width")
        optionChild(id, "line_width", SemanticsMatcher.keyIsDefined(SemanticsActions.SetProgress))
            .performSemanticsAction(SemanticsActions.SetProgress) { it(0f) }
        queueNode("option_${id}_padding").performClick()
        compose.waitUntil(MEDIUM) { item(id).values.size >= 2 }
        val single = String(outputBytes(convertAndWait(id)), Charsets.US_ASCII)
        assertFalse("line width 0 writes one line", single.trim().contains('\n'))
        assertEquals(Base64.getEncoder().withoutPadding().encodeToString(input), single.trim())
    }

    @Test
    fun targetPickerSearchesGroupsAndShowsTheSelection() {
        val (id) = launchWith(Fixtures.DATA)
        val targets = item(id).targets
        assumeTrue("needs at least two targets", targets.size >= 2)
        clickInQueue("pick_target_$id")
        waitForTag("target_picker")
        // Grouped: section headings (plus the sheet title) when the search is empty.
        val headings = compose.onAllNodes(isHeading() and hasAnyAncestor(hasTestTag("target_picker")), useUnmergedTree = true).fetchSemanticsNodes()
        assertTrue("picker shows section headings", headings.size >= 2)

        // Search narrows the list to matching targets: by id and by display name, from the live target list.
        val wanted = targets.first { it.id == "hex" || it == targets.last() }
        val matches = app.convertible.model.TargetRanking.search(targets, wanted.name).map { it.id }.toSet()
        val other = targets.first { it.id !in matches }
        compose.onNodeWithTag("picker_search").performTextInput(wanted.name)
        waitForTag("target_${wanted.id}")
        waitForGone("target_${other.id}")
        compose.onNodeWithTag("picker_search").performTextClearance()
        compose.onNodeWithTag("picker_search").performTextInput("zzqx-no-such-format")
        compose.waitUntil(MEDIUM) { compose.onAllNodes(hasText("No format matches", substring = true)).fetchSemanticsNodes().isNotEmpty() }
        compose.onNodeWithTag("picker_search").performTextClearance()
        compose.onNodeWithTag("picker_search").performTextInput(wanted.id)
        waitForTag("target_${wanted.id}")
        compose.onNodeWithTag("target_${wanted.id}").performClick()
        waitForGone("target_picker")
        compose.waitUntil(MEDIUM) { item(id).target?.id == wanted.id }

        // Reopened, the picker marks the current choice; Back closes it without changing anything.
        clickInQueue("pick_target_$id")
        waitForTag("target_picker")
        compose.onNodeWithTag("picker_search").performTextInput(wanted.id)
        waitForTag("target_${wanted.id}")
        compose.onNodeWithTag("target_${wanted.id}").assert(isSelected())
        pressBackUntilGone("target_picker")
        assertEquals(wanted.id, item(id).target?.id)
    }

    @Test
    fun shareOpensTheSystemSheetWithoutListingConvertible() {
        val (id) = launchWith(Fixtures.NOTE)
        assumeTarget(id, "base64")
        chooseTarget(id, "base64")
        val done = convertAndWait(id)

        // The intent handed to the system: a FileProvider URI with a read grant that resolves to the output bytes,
        // and Convertible excluded from its own share targets.
        val chooser = vm.shareIntent(listOf(id))!!
        @Suppress("DEPRECATION")
        val excluded = chooser.getParcelableArrayExtra(Intent.EXTRA_EXCLUDE_COMPONENTS)!!.map { it as android.content.ComponentName }
        assertTrue(excluded.any { it.className == MainActivity::class.java.name })
        @Suppress("DEPRECATION")
        val send = chooser.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)!!
        assertTrue(send.flags and Intent.FLAG_GRANT_READ_URI_PERMISSION != 0)
        @Suppress("DEPRECATION")
        val stream = send.getParcelableExtra<Uri>(Intent.EXTRA_STREAM)!!
        assertArrayEquals(outputBytes(done), Device.app.contentResolver.openInputStream(stream)!!.use { it.readBytes() })

        clickInQueue("share_$id")
        val labels = SystemUi.awaitChooserLabels()
        assertTrue("share sheet shown: $labels", labels.isNotEmpty())
        val ownLabels = setOf(Device.app.getString(app.convertible.R.string.app_name), Device.app.getString(app.convertible.R.string.share_target_label))
        assertTrue("Convertible lists itself in its own share sheet: $labels", labels.none { it in ownLabels })
        SystemUi.dismissChooser()
        compose.onNodeWithTag("home").assertExists()
    }

    @Test
    fun invalidOptionShowsAFriendlyErrorAndTryAgainRecovers() {
        val (id) = launchWith(Fixtures.NOTE)
        assumeTarget(id, "data-uri")
        chooseTarget(id, "data-uri")
        assumeTrue(item(id).schema.any { it.key == "mime" })
        queueNode("option_${id}_mime")
        optionChild(id, "mime", hasSetTextAction()).performTextInput("not a media type")
        compose.waitForIdle()
        assertEquals("not a media type", (item(id).values["mime"] as? app.convertible.model.OptionValue.Text)?.value)
        clickInQueue("convert_$id")
        val failed = awaitStatus<ItemStatus.Failed>(id)
        assertTrue("expected invalid settings, got ${failed.error}", failed.error is CoreError.InvalidArgument)

        // Friendly title and the core's detail as a sentence; the action says "Try again".
        queueNode("retry_$id")
        compose.onNodeWithText(Device.app.getString(app.convertible.R.string.error_invalid_settings)).assertIsDisplayed()
        compose.onNode(hasText("Option 'mime' is not a valid media type.", substring = true), useUnmergedTree = true).assertExists()
        compose.onNode(hasText(Device.app.getString(app.convertible.R.string.action_try_again)) and hasAnyAncestor(hasTestTag("retry_$id")), useUnmergedTree = true).assertExists()

        // Fix the option and try again.
        queueNode("option_${id}_mime")
        optionChild(id, "mime", hasSetTextAction()).performTextClearance()
        compose.waitUntil(MEDIUM) { (item(id).values["mime"] as? app.convertible.model.OptionValue.Text)?.value.isNullOrEmpty() }
        clickInQueue("retry_$id")
        val done = awaitStatus<ItemStatus.Done>(id)
        val uri = String(outputBytes(done), Charsets.US_ASCII).trim()
        assertTrue(uri.startsWith("data:"))
        assertArrayEquals(Fixtures.bytes(Fixtures.NOTE), Base64.getDecoder().decode(uri.substringAfter(',')))
    }

    @Test
    fun progressIsShownWhileConverting() {
        val (id) = launchWith(Fixtures.LARGE)
        assumeTarget(id, "checksums")
        chooseTarget(id, "checksums", search = "checksum")
        clickInQueue("convert_$id")
        compose.waitUntil(LONG) { item(id).status is ItemStatus.Converting }
        // The progress ring exposes its value to accessibility services; the cancel action is offered.
        queueNode("cancel_$id")
        compose.waitUntil(MEDIUM) { existsUnmerged("progress_$id") }
        val progress = compose.onNodeWithTag("progress_$id", useUnmergedTree = true).fetchSemanticsNode()
        assertNotNull(progress.config.getOrNull(SemanticsProperties.ProgressBarRangeInfo))
        queueNode("cancel_$id").assertIsDisplayed()
        val done = awaitStatus<ItemStatus.Done>(id, CONVERSION)
        val sha = Regex("""SHA-256: ([0-9a-f]+)""").find(String(outputBytes(done)))!!.groupValues[1]
        assertEquals(Fixtures.sha256(Fixtures.LARGE).joinToString("") { "%02x".format(it) }, sha)
    }
}
