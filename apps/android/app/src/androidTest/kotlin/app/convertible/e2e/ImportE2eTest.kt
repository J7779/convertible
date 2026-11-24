package app.convertible.e2e

import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import androidx.compose.ui.test.hasAnyAncestor
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import app.convertible.MainActivity
import app.convertible.e2e.fixtures.Fixtures
import app.convertible.e2e.fixtures.ShareFixtureProvider
import app.convertible.e2e.support.Device
import app.convertible.e2e.support.E2eTest
import app.convertible.e2e.support.SystemUi
import app.convertible.state.ItemStatus
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Files reach the app through the system file picker (UI Automator drives DocumentsUI against a test storage root),
 * "Share -> Convertible" (ACTION_SEND / SEND_MULTIPLE) and "Open with" (ACTION_VIEW). Each import is checked byte for
 * byte against the fixture and shown as a queue card with the detected format's badge.
 */
@RunWith(AndroidJUnit4::class)
class ImportE2eTest : E2eTest() {
    private fun assertImported(id: Long, name: String) {
        val item = item(id)
        assertEquals(name, item.name)
        assertEquals(ItemStatus.Ready, item.status)
        assertArrayEquals("imported bytes of $name", Fixtures.bytes(name), File(item.inputPath!!).readBytes())
        assertEquals(Fixtures.size(name), item.size)
        // Card: name and the format badge of the detected source format (label from the live catalogue).
        val label = (vm.state.value.catalogue[item.sourceId]?.label ?: item.sourceId!!.uppercase())
        queueNode("card_$id")
        compose.onNode(hasText(name) and hasAnyAncestor(hasTestTag("card_$id")), useUnmergedTree = true).assertExists()
        compose.onNode(hasText(label) and hasAnyAncestor(hasTestTag("card_$id")), useUnmergedTree = true).assertExists()
        assertTrue("targets offered for $name", item.targets.isNotEmpty())
    }

    @Test
    fun systemPickerImportsSeveralFilesWithFormatBadges() {
        launch()
        compose.onNodeWithTag("choose_files").performClick()
        val picked = arrayOf(Fixtures.NOTE, Fixtures.ENCODED, Fixtures.DATA)
        SystemUi.pickFixtures(*picked)
        val ids = awaitImported(picked.size)
        assertEquals(picked.size, ids.size)
        // DocumentsUI returns the selection in its own order; match by name.
        picked.forEach { name -> assertImported(items.single { it.name == name }.id, name) }
        assertEquals("txt", items.single { it.name == Fixtures.NOTE }.sourceId)
        assertEquals("base64", items.single { it.name == Fixtures.ENCODED }.sourceId)
        // Distinct formats: no "Convert all to…" group.
        assertTrue(vm.state.value.groups.isEmpty())
    }

    @Test
    fun systemPickerSingleFileAndCancel() {
        launch()
        compose.onNodeWithTag("choose_files").performClick()
        SystemUi.awaitDocumentsUi()
        SystemUi.cancelDocumentsUi()
        compose.waitForIdle()
        assertTrue("cancelled picker adds nothing", items.isEmpty())
        compose.onNodeWithTag("choose_files").performClick()
        SystemUi.pickFixtures(Fixtures.NOTE_3)
        val (id) = awaitImported(1)
        assertImported(id, Fixtures.NOTE_3)
    }

    @Test
    fun shareToConvertibleImportsOneFile() {
        val (id) = launchWith(Fixtures.NOTE)
        assertImported(id, Fixtures.NOTE)
    }

    @Test
    fun shareMultipleKeepsTheOrderAndGroupsSameFormats() {
        val names = arrayOf(Fixtures.NOTE, Fixtures.NOTE_2, Fixtures.NOTE_3, Fixtures.DATA)
        val ids = launchWith(*names)
        assertEquals(names.toList(), items.map { it.name })
        ids.zip(names).forEach { (id, name) -> assertImported(id, name) }
        // Three plain-text files form one group with a "Convert all to…" control.
        val group = vm.state.value.groups.single()
        assertEquals("txt", group.sourceId)
        assertEquals(ids.take(3), group.itemIds)
        waitForTag("group_txt")
    }

    @Test
    fun openWithImportsTheViewedFile() {
        val intent = Intent(Intent.ACTION_VIEW)
            .setClass(Device.app, MainActivity::class.java)
            .setDataAndType(ShareFixtureProvider.uri(Fixtures.ENCODED), "application/octet-stream")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        launch(intent)
        val (id) = awaitImported(1)
        assertImported(id, Fixtures.ENCODED)
    }

    @Test
    fun sharingWhileRunningAddsToTheSameQueue() {
        launchWith(Fixtures.NOTE)
        // A second share while the app is open arrives through onNewIntent (singleTop) into the same queue.
        Device.app.startActivity(sendIntent(Fixtures.NOTE_2).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP))
        val ids = awaitImported(2)
        assertEquals(listOf(Fixtures.NOTE, Fixtures.NOTE_2), items.map { it.name })
        assertImported(ids[1], Fixtures.NOTE_2)
    }

    @Test
    fun unsafeUrisAreIgnored() {
        // file:// URIs (could point at the app's private files) and the app's own FileProvider are refused.
        val own = File(Device.app.filesDir, "secret.txt").apply { writeText("private") }
        val intent = Intent(Intent.ACTION_SEND_MULTIPLE)
            .setClass(Device.app, MainActivity::class.java)
            .setType("*/*")
            .putParcelableArrayListExtra(
                Intent.EXTRA_STREAM,
                arrayListOf(Uri.fromFile(own), Uri.parse("content://${Device.app.packageName}.files/outputs/x.b64"), ShareFixtureProvider.uri(Fixtures.NOTE)),
            )
        launch(intent)
        awaitImported(1)
        assertEquals(listOf(Fixtures.NOTE), items.map { it.name })
    }

    @Test
    fun theSystemOffersConvertibleForShareAndOpenWith() {
        val pm = Device.app.packageManager
        fun handles(intent: Intent) = pm.queryIntentActivities(intent, PackageManager.MATCH_DEFAULT_ONLY)
            .any { it.activityInfo.packageName == Device.app.packageName }
        assertTrue(handles(Intent(Intent.ACTION_SEND).setType("application/pdf")))
        assertTrue(handles(Intent(Intent.ACTION_SEND).setType("image/png")))
        assertTrue(handles(Intent(Intent.ACTION_SEND_MULTIPLE).setType("*/*")))
        assertTrue(handles(Intent(Intent.ACTION_VIEW).setDataAndType(Uri.parse("content://x/y"), "text/plain")))
    }
}
