package app.convertible.ui

import androidx.compose.ui.test.ExperimentalTestApi
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollToNode
import androidx.compose.ui.test.performTextInput
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import app.convertible.MainActivity
import app.convertible.state.ImportedFile
import app.convertible.state.ItemStatus
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.Base64

@OptIn(ExperimentalTestApi::class)
@RunWith(AndroidJUnit4::class)
class AppUiTest {
    @get:Rule
    val rule = createAndroidComposeRule<MainActivity>()

    @Before
    fun skipIntro() {
        // The intro is skippable by a tap; it also ends by itself after 1.8 s.
        rule.waitUntil(5_000) { rule.onAllNodes(hasTestTag("home")).fetchSemanticsNodes().isNotEmpty() }
        if (rule.onAllNodes(hasTestTag("intro")).fetchSemanticsNodes().isNotEmpty()) {
            runCatching { rule.onNodeWithTag("intro").performClick() }
        }
        rule.waitUntil(5_000) { rule.onAllNodes(hasTestTag("intro")).fetchSemanticsNodes().isEmpty() }
    }

    @Test
    fun homeShowsTheDropZoneAndPrivacyStatement() {
        rule.onNodeWithTag("drop_zone").assertIsDisplayed()
        rule.onNodeWithTag("choose_files").assertIsDisplayed()
        rule.onNodeWithTag("privacy_note").assertIsDisplayed()
        rule.onNodeWithText("Files never leave your device", substring = true).assertIsDisplayed()
    }

    @Test
    fun formatsBrowserSearches() {
        rule.onNodeWithTag("nav_formats").performClick()
        rule.onNodeWithTag("formats_screen").assertIsDisplayed()
        rule.onNodeWithTag("formats_search").performTextInput("base64")
        rule.waitUntil(5_000) { rule.onAllNodes(hasTestTag("format_base64")).fetchSemanticsNodes().isNotEmpty() }
        rule.onNodeWithTag("format_base64").assertIsDisplayed()
    }

    @Test
    fun settingsShowVersions() {
        rule.onNodeWithTag("nav_settings").performClick()
        rule.onNodeWithTag("settings_screen").assertIsDisplayed()
        rule.onNodeWithTag("app_version").assertIsDisplayed()
        rule.waitUntil(5_000) { rule.onAllNodes(hasTestTag("core_version")).fetchSemanticsNodes().isNotEmpty() }
    }

    @Test
    fun convertsAFileEndToEnd() {
        val vm = ViewModelProvider(rule.activity)[ConverterViewModel::class.java]
        val context = rule.activity
        val content = "Convertible UI test ✓"
        // Files normally arrive through the system picker; the test feeds the queue directly.
        val id = vm.store.add("note.txt", content.length.toLong()) { itemId ->
            val f = File(File(context.cacheDir, "work/in/$itemId").apply { mkdirs() }, "note.txt")
            f.writeText(content)
            ImportedFile(f.path, f.name, f.length())
        }
        rule.waitUntil(10_000) { vm.store.item(id)?.status == ItemStatus.Ready }
        rule.waitUntil(5_000) { rule.onAllNodes(hasTestTag("pick_target_$id")).fetchSemanticsNodes().isNotEmpty() }
        rule.onNodeWithTag("queue").performScrollToNode(hasTestTag("pick_target_$id"))
        rule.onNodeWithTag("pick_target_$id").performClick()
        rule.onNodeWithTag("picker_search").performTextInput("base64")
        rule.waitUntil(5_000) { rule.onAllNodes(hasTestTag("target_base64")).fetchSemanticsNodes().isNotEmpty() }
        rule.onNodeWithTag("target_base64").performClick()
        rule.waitUntil(5_000) { rule.onAllNodes(hasTestTag("convert_$id")).fetchSemanticsNodes().isNotEmpty() }
        rule.onNodeWithTag("queue").performScrollToNode(hasTestTag("convert_$id"))
        rule.onNodeWithTag("convert_$id").performClick()
        rule.waitUntil(20_000) { vm.store.item(id)?.status is ItemStatus.Done }
        val output = (vm.store.item(id)!!.status as ItemStatus.Done).output
        assertEquals(Base64.getEncoder().encodeToString(content.toByteArray()), File(output.path).readText().trim())
        rule.onNodeWithTag("queue").performScrollToNode(hasTestTag("save_$id"))
        rule.onNodeWithTag("save_$id").assertIsDisplayed()
    }
}
