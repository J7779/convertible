package app.convertible.e2e

import androidx.activity.BackEventCompat
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.state.ToggleableState
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsSelected
import androidx.compose.ui.test.hasAnyAncestor
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performScrollToNode
import androidx.compose.ui.test.performTextClearance
import androidx.compose.ui.test.performTextInput
import androidx.lifecycle.Lifecycle
import androidx.test.ext.junit.runners.AndroidJUnit4
import app.convertible.R
import app.convertible.e2e.support.Device
import app.convertible.e2e.support.E2eTest
import app.convertible.e2e.support.Screenshots
import app.convertible.model.Preset
import app.convertible.state.ThemeMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** Formats browser, settings (persisted across process death), and back / predictive-back navigation. */
@RunWith(AndroidJUnit4::class)
class ScreensE2eTest : E2eTest() {
    private val prefsFile: File get() = File(Device.app.dataDir, "shared_prefs/settings.xml")

    @Test
    fun formatsBrowserSearchesTheLiveCatalogue() {
        launch()
        compose.onNodeWithTag("nav_formats").performClick()
        waitForTag("formats_screen")
        compose.waitUntil(LONG) { vm.state.value.catalogue.formats.isNotEmpty() }
        val formats = vm.state.value.catalogue.formats
        // The least popular format is far down the list: search finds it by display name.
        val rare = formats.last()
        compose.onNodeWithTag("formats_search").performTextInput(rare.name)
        waitForTag("format_${rare.id}")
        compose.onNodeWithTag("format_${rare.id}").assertIsDisplayed()
            .assert(hasContentDescription(rare.name, substring = true))
        // Every format of the catalogue is reachable by its id.
        val sample = formats.filterIndexed { i, _ -> i % maxOf(1, formats.size / 6) == 0 }
        for (f in sample) {
            compose.onNodeWithTag("formats_search").performTextClearance()
            compose.onNodeWithTag("formats_search").performTextInput(f.id)
            waitForTag("format_${f.id}")
        }
        compose.onNodeWithTag("formats_search").performTextClearance()
        compose.onNodeWithTag("formats_search").performTextInput("zzqx-no-such-format")
        compose.waitUntil(MEDIUM) { compose.onAllNodes(hasText("No format matches", substring = true)).fetchSemanticsNodes().isNotEmpty() }
        compose.onNodeWithTag("formats_search").performTextClearance()

        // Category filter: only formats of that category remain.
        val category = vm.state.value.catalogue.categories.first { c -> formats.count { it.category == c } >= 2 }
        val inside = formats.first { it.category == category }
        val outside = formats.first { it.category != category }
        val chipLabel = Device.app.getString(
            Device.app.resources.getIdentifier("category_$category", "string", Device.app.packageName).takeIf { it != 0 } ?: R.string.category_other,
        )
        compose.onNode(hasText(chipLabel) and SemanticsMatcher.keyIsDefined(SemanticsProperties.Selected)).performScrollTo().performClick()
        compose.onNodeWithTag("formats_list").performScrollToNode(hasTestTag("format_${inside.id}"))
        assertTrue(exists("format_${inside.id}"))
        compose.waitUntil(MEDIUM) {
            runCatching { compose.onNodeWithTag("formats_list").performScrollToNode(hasTestTag("format_${outside.id}")) }.isFailure
        }

        pressBackUntilGone("formats_screen")
        waitForTag("home")
    }

    @Test
    fun settingsApplyImmediatelyAndAreWrittenToDisk() {
        Device.setNightMode(false)
        launch()
        assertTrue("system light: light UI", Screenshots.meanLuminance(compose) > 0.6)
        compose.onNodeWithTag("nav_settings").performClick()
        waitForTag("settings_screen")
        compose.onNodeWithTag("theme_0").assertIsSelected() // System by default
        compose.onNodeWithTag("strip_metadata").assert(SemanticsMatcher.expectValue(SemanticsProperties.ToggleableState, ToggleableState.On))

        compose.onNodeWithTag("theme_1").performClick() // Dark
        compose.onNodeWithTag("default_preset_2").performClick() // Small
        compose.onNodeWithTag("strip_metadata").performScrollTo().performClick()
        compose.waitUntil(MEDIUM) { vm.settings.value.theme == ThemeMode.DARK && !vm.settings.value.stripMetadata }
        assertEquals(Preset.SMALL, vm.settings.value.defaultPreset)
        compose.waitUntil(MEDIUM) { Screenshots.meanLuminance(compose) < 0.35 }

        // What a new process will read: the preferences file on disk.
        compose.waitUntil(MEDIUM) {
            prefsFile.isFile && prefsFile.readText().let {
                ">DARK<" in it && ">${Preset.SMALL.id}<" in it && Regex("""name="strip_metadata" value="false"""").containsMatchIn(it)
            }
        }

        // And the UI keeps them through recreation.
        scenario.recreate()
        waitForTag("settings_screen")
        compose.onNodeWithTag("theme_1").assertIsSelected()
        compose.onNodeWithTag("default_preset_2").assertIsSelected()
        compose.onNodeWithTag("strip_metadata").assert(SemanticsMatcher.expectValue(SemanticsProperties.ToggleableState, ToggleableState.Off))
    }

    @Test
    fun settingsLeftByAnEarlierProcessAreRestoredOnColdStart() {
        // Each test runs in a fresh process; this file is exactly what the previous test saw written on disk.
        prefsFile.parentFile!!.mkdirs()
        prefsFile.writeText(
            """
            <?xml version='1.0' encoding='utf-8' standalone='yes' ?>
            <map>
                <string name="theme">DARK</string>
                <string name="default_preset">${Preset.SMALL.id}</string>
                <boolean name="strip_metadata" value="false" />
            </map>
            """.trimIndent(),
        )
        Device.setNightMode(false)
        launch()
        assertTrue("dark theme from the saved setting", Screenshots.meanLuminance(compose) < 0.35)
        compose.onNodeWithTag("nav_settings").performClick()
        waitForTag("settings_screen")
        compose.onNodeWithTag("theme_1").assertIsSelected()
        compose.onNodeWithTag("default_preset_2").assertIsSelected()
        compose.onNodeWithTag("strip_metadata").performScrollTo()
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.ToggleableState, ToggleableState.Off))
    }

    @Test
    fun backNavigatesTheScreenStackThenLeavesTheApp() {
        launch()
        compose.onNodeWithTag("nav_settings").performClick()
        waitForTag("settings_screen")
        compose.onNodeWithTag("open_licences").performScrollTo().performClick()
        compose.waitUntil(MEDIUM) { !exists("settings_screen") }
        Device.ui.pressBack()
        waitForTag("settings_screen")
        Device.ui.pressBack()
        waitForGone("settings_screen")
        compose.onNodeWithTag("home").assertExists()
        // The on-screen back button of a sub-screen works too.
        compose.onNodeWithTag("nav_formats").performClick()
        waitForTag("formats_screen")
        compose.onNodeWithContentDescriptionOf(R.string.action_back).performClick()
        waitForGone("formats_screen")
        // Back on the home screen leaves the app.
        Device.ui.pressBack()
        compose.waitUntil(MEDIUM) { scenario.state == Lifecycle.State.CREATED || scenario.state == Lifecycle.State.DESTROYED }
    }

    @Test
    fun predictiveBackCanBeCancelledOrCompleted() {
        launch()
        compose.onNodeWithTag("nav_settings").performClick()
        waitForTag("settings_screen")
        fun event(progress: Float) = BackEventCompat(10f, 800f, progress, BackEventCompat.EDGE_LEFT)

        // Gesture started and dragged half-way, then cancelled: the screen springs back and stays.
        scenario.onActivity {
            it.onBackPressedDispatcher.dispatchOnBackStarted(event(0f))
            it.onBackPressedDispatcher.dispatchOnBackProgressed(event(0.5f))
        }
        compose.waitForIdle()
        assertTrue("both screens drawn during the gesture", exists("settings_screen") && exists("home"))
        scenario.onActivity { it.onBackPressedDispatcher.dispatchOnBackCancelled() }
        compose.waitForIdle()
        compose.onNodeWithTag("settings_screen").assertIsDisplayed()

        // Gesture committed: back to home.
        scenario.onActivity {
            it.onBackPressedDispatcher.dispatchOnBackStarted(event(0f))
            it.onBackPressedDispatcher.dispatchOnBackProgressed(event(0.8f))
            it.onBackPressedDispatcher.onBackPressed()
        }
        waitForGone("settings_screen")
        compose.onNodeWithTag("home").assertIsDisplayed()
    }

    private fun androidx.compose.ui.test.junit4.ComposeTestRule.onNodeWithContentDescriptionOf(res: Int) =
        onNode(hasContentDescription(Device.app.getString(res)) and hasAnyAncestor(hasTestTag("formats_screen")))
}
