package app.convertible.e2e

import android.Manifest
import android.content.pm.PackageManager
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsNode
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.isRoot
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.text.TextLayoutResult
import androidx.test.ext.junit.runners.AndroidJUnit4
import app.convertible.e2e.fixtures.Fixtures
import app.convertible.e2e.support.Device
import app.convertible.e2e.support.E2eTest
import app.convertible.e2e.support.Screenshots
import app.convertible.state.ItemStatus
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Configuration changes and window sizes: rotation and activity recreation in the middle of a conversion, phone /
 * foldable / tablet layouts, light and dark, large fonts, TalkBack-relevant semantics and touch targets, and the
 * merged manifest's permissions.
 */
@RunWith(AndroidJUnit4::class)
class AdaptiveE2eTest : E2eTest() {
    private val density get() = Device.app.resources.displayMetrics.density

    private fun startLongConversion(): Long {
        val (id) = launchWith(Fixtures.LARGE)
        assumeTrue(item(id).targets.any { it.id == "checksums" })
        chooseTarget(id, "checksums", search = "checksum")
        clickInQueue("convert_$id")
        compose.waitUntil(LONG) { item(id).status is ItemStatus.Converting }
        return id
    }

    private fun assertLargeChecksum(id: Long) {
        val done = awaitStatus<ItemStatus.Done>(id, CONVERSION)
        val sha = Regex("""SHA-256: ([0-9a-f]+)""").find(String(outputBytes(done)))!!.groupValues[1]
        assertEquals(Fixtures.sha256(Fixtures.LARGE).joinToString("") { "%02x".format(it) }, sha)
        queueNode("save_$id")
    }

    @Test
    fun rotationDuringAConversionKeepsItRunning() {
        val id = startLongConversion()
        Device.ui.setOrientationLandscape()
        compose.waitUntil(MEDIUM) { Device.app.resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE }
        waitForTag("home")
        assertTrue("conversion survives rotation", item(id).status.isBusy || item(id).status is ItemStatus.Done)
        assertFalse("intro is not replayed", exists("intro"))
        Device.ui.setOrientationNatural()
        assertLargeChecksum(id)
    }

    @Test
    fun activityRecreationDuringAConversionKeepsTheQueue() {
        val id = startLongConversion()
        val before = vm
        scenario.recreate()
        waitForTag("home")
        assertTrue("same ViewModel after recreation", before === vm)
        assertFalse("intro is not replayed", exists("intro"))
        queueNode("card_$id")
        assertLargeChecksum(id)
    }

    private fun bounds(tag: String): Rect = compose.onNodeWithTag(tag).fetchSemanticsNode().boundsInRoot

    private fun rootBounds(): Rect = compose.onAllNodes(isRoot())[0].fetchSemanticsNode().boundsInRoot

    @Test
    fun phoneLayoutIsOneColumnWithABottomSheetPicker() {
        val (id) = launchWith(Fixtures.NOTE)
        // Drop zone sits above the queue cards, in the same column.
        val drop = bounds("drop_zone")
        val card = queueNode("card_$id").fetchSemanticsNode().boundsInRoot
        assertTrue("drop zone above the card", drop.bottom <= card.top)
        clickInQueue("pick_target_$id")
        waitForTag("target_picker")
        val sheet = compose.onNodeWithTag("target_picker").fetchSemanticsNode().boundsInRoot
        val screen = compose.onAllNodes(isRoot()).fetchSemanticsNodes().last().boundsInRoot
        assertTrue("bottom sheet spans the width", sheet.width >= screen.width * 0.95f)
        assertTrue("bottom sheet anchored to the bottom", sheet.bottom >= screen.bottom - 4 * density)
    }

    @Test
    fun tabletLayoutShowsTwoPanesAndACentredPicker() {
        Device.setDisplay(2560, 1600, 240) // ~1707 x 1067 dp
        val (id) = launchWith(Fixtures.NOTE)
        assertTrue("expanded width", rootBounds().width / density >= 840)
        val drop = bounds("drop_zone")
        val queue = bounds("queue")
        assertTrue("drop zone and queue side by side ($drop / $queue)", drop.right <= queue.left)
        assertTrue("panes share the vertical space", drop.top < queue.bottom && queue.top < drop.bottom)
        clickInQueue("pick_target_$id")
        waitForTag("target_picker")
        val sheet = compose.onNodeWithTag("target_picker").fetchSemanticsNode().boundsInRoot
        val screen = compose.onAllNodes(isRoot()).fetchSemanticsNodes().last().boundsInRoot
        assertTrue("centred dialog, not full width", sheet.width / density <= 600 && sheet.left > screen.left && sheet.right < screen.right)
        assertTrue("not attached to the bottom", sheet.bottom < screen.bottom - 8 * density)
    }

    @Test
    fun unfoldedFoldableUsesTheMediumLayout() {
        Device.setDisplay(1768, 2208, 420) // ~673 x 841 dp, like an unfolded book-style foldable
        val (id) = launchWith(Fixtures.NOTE)
        val width = rootBounds().width / density
        assertTrue("medium width: $width dp", width in 600f..839f)
        // One column (the queue includes the drop zone), but the picker is a centred dialog.
        val drop = bounds("drop_zone")
        val card = queueNode("card_$id").fetchSemanticsNode().boundsInRoot
        assertTrue(drop.bottom <= card.top)
        clickInQueue("pick_target_$id")
        waitForTag("target_picker")
        val sheet = compose.onNodeWithTag("target_picker").fetchSemanticsNode().boundsInRoot
        val screen = compose.onAllNodes(isRoot()).fetchSemanticsNodes().last().boundsInRoot
        assertTrue("centred dialog", sheet.left > screen.left && sheet.bottom < screen.bottom - 8 * density)
    }

    @Test
    fun followsTheSystemLightAndDarkTheme() {
        Device.setNightMode(true)
        launch()
        compose.waitUntil(MEDIUM) { Screenshots.meanLuminance(compose) < 0.35 }
        scenario.close()
        Device.setNightMode(false)
        launch()
        compose.waitUntil(MEDIUM) { Screenshots.meanLuminance(compose) > 0.6 }
    }

    /**
     * Text nodes below [node] that are cut: ellipsised, or drawn outside the control's bounds. (Compose's
     * hasVisualOverflow also reports sub-pixel line-height overflow that is not visible, so it is not used.)
     */
    private fun clippedTexts(node: SemanticsNode): List<String> {
        val out = mutableListOf<String>()
        fun visit(n: SemanticsNode) {
            n.config.getOrNull(SemanticsActions.GetTextLayoutResult)?.action?.let { action ->
                val results = mutableListOf<TextLayoutResult>()
                action(results)
                results.firstOrNull()?.let { r ->
                    val ellipsised = (0 until r.lineCount).any { r.isLineEllipsized(it) }
                    val b = n.boundsInRoot
                    val outside = b.left < node.boundsInRoot.left - 1 || b.right > node.boundsInRoot.right + 1
                    if (ellipsised || outside) out += r.layoutInput.text.text
                }
            }
            n.children.forEach { visit(it) }
        }
        visit(node)
        return out
    }

    private fun assertPrimaryControl(tag: String, scroll: Boolean = false) {
        val interaction = if (scroll) queueNode(tag) else compose.onNodeWithTag(tag)
        val node = compose.onNodeWithTag(tag, useUnmergedTree = true).fetchSemanticsNode()
        interaction.assertExists()
        val root = rootBounds()
        val b = node.boundsInRoot
        assertTrue("$tag fits the window horizontally ($b in $root)", b.left >= root.left - 1 && b.right <= root.right + 1)
        assertEquals("$tag text clipped at large font size", emptyList<String>(), clippedTexts(node))
    }

    @Test
    fun largeFontKeepsPrimaryControlsReadable() {
        Device.setFontScale(1.6f)
        val (id) = launchWith(Fixtures.NOTE)
        assertTrue(Device.app.resources.configuration.fontScale >= 1.6f)
        assertPrimaryControl("nav_formats")
        assertPrimaryControl("nav_settings")
        assertPrimaryControl("choose_files", scroll = true)
        chooseTarget(id, "base64")
        assertPrimaryControl("convert_$id", scroll = true)
        assertPrimaryControl("convert_all")
        clickInQueue("convert_$id")
        awaitStatus<ItemStatus.Done>(id)
        assertPrimaryControl("save_$id", scroll = true)
        assertPrimaryControl("share_$id", scroll = true)
        assertPrimaryControl("save_all")
    }

    private fun clickableNodes(): List<SemanticsNode> =
        compose.onAllNodes(SemanticsMatcher.keyIsDefined(SemanticsActions.OnClick)).fetchSemanticsNodes()
            .filter { it.config.getOrNull(SemanticsProperties.Disabled) == null }
            // Partly scrolled out of view (clipped by the scroll container): checked when fully on screen.
            .filter { it.boundsInRoot.width >= it.size.width - 1 && it.boundsInRoot.height >= it.size.height - 1 && it.size.height > 0 }

    private fun assertTouchTargets(where: String) {
        val min = 48 * density - 1
        val small = clickableNodes().filter { it.touchBoundsInRoot.width < min || it.touchBoundsInRoot.height < min }
            .map { "${it.config.getOrNull(SemanticsProperties.TestTag) ?: it.config.getOrNull(SemanticsProperties.ContentDescription) ?: it.config.getOrNull(SemanticsProperties.Text)} " +
                "${(it.touchBoundsInRoot.width / density).toInt()}x${(it.touchBoundsInRoot.height / density).toInt()}dp" }
        assertEquals("touch targets under 48 dp on $where", emptyList<String>(), small)
    }

    @Test
    fun keyControlsExposeRolesLabelsAndLargeTouchTargets() {
        val (id) = launchWith(Fixtures.NOTE)
        fun node(tag: String) = compose.onNodeWithTag(tag).fetchSemanticsNode().config
        // Icon-only buttons are labelled; buttons announce their role.
        for (tag in listOf("nav_formats", "nav_settings")) {
            assertNotNull("$tag label", node(tag).getOrNull(SemanticsProperties.ContentDescription)?.firstOrNull())
            assertEquals(androidx.compose.ui.semantics.Role.Button, node(tag).getOrNull(SemanticsProperties.Role))
        }
        assertEquals(androidx.compose.ui.semantics.Role.Button, queueNode("choose_files").fetchSemanticsNode().config.getOrNull(SemanticsProperties.Role))
        // A file card announces name, type and size in one stop; the remove button names the file.
        queueNode("card_$id")
        val described = compose.onAllNodes(SemanticsMatcher("describes ${Fixtures.NOTE}") {
            it.config.getOrNull(SemanticsProperties.ContentDescription)?.any { d -> d.startsWith(Fixtures.NOTE + ",") } == true
        }).fetchSemanticsNodes()
        assertTrue("card header description", described.isNotEmpty())
        assertTrue(node("remove_$id").getOrNull(SemanticsProperties.ContentDescription)!!.single().contains(Fixtures.NOTE))
        // The current-format row is a button labelled with the format.
        chooseTarget(id, "base64")
        assertEquals(androidx.compose.ui.semantics.Role.Button, queueNode("pick_target_$id").fetchSemanticsNode().config.getOrNull(SemanticsProperties.Role))
        assertTouchTargets("home")

        // Settings: radio-button segments with selection, a switch with its state.
        compose.onNodeWithTag("nav_settings").performClick()
        waitForTag("settings_screen")
        assertEquals(androidx.compose.ui.semantics.Role.RadioButton, node("theme_0").getOrNull(SemanticsProperties.Role))
        assertEquals(true, node("theme_0").getOrNull(SemanticsProperties.Selected))
        assertEquals(androidx.compose.ui.semantics.Role.Switch, node("strip_metadata").getOrNull(SemanticsProperties.Role))
        assertNotNull(node("strip_metadata").getOrNull(SemanticsProperties.ToggleableState))
        assertTouchTargets("settings")
        compose.onNodeWithTag("open_licences").performScrollTo()
        assertTouchTargets("settings, scrolled")
    }

    @Test
    fun mergedManifestRequestsNoNetworkPermission() {
        val info = Device.app.packageManager.getPackageInfo(Device.app.packageName, PackageManager.GET_PERMISSIONS)
        val requested = info.requestedPermissions.orEmpty().toList()
        assertFalse("INTERNET requested: $requested", Manifest.permission.INTERNET in requested)
        assertFalse("ACCESS_NETWORK_STATE requested: $requested", Manifest.permission.ACCESS_NETWORK_STATE in requested)
        // Only the AndroidX-internal broadcast guard may appear; no dangerous or network permission at all.
        val unexpected = requested.filterNot { it.endsWith(".DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION") }
        assertEquals("unexpected permissions", emptyList<String>(), unexpected)
    }
}
