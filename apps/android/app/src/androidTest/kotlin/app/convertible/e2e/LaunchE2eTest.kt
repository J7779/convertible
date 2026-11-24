package app.convertible.e2e

import androidx.compose.ui.input.key.Key
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.ExperimentalTestApi
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertHasClickAction
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performKeyInput
import androidx.compose.ui.test.pressKey
import androidx.test.ext.junit.runners.AndroidJUnit4
import app.convertible.e2e.support.Device
import app.convertible.e2e.support.E2eTest
import app.convertible.ui.theme.Brand
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Cold launch: Android 12+ splash (background only) hands over to the animated intro, which ends by itself, on a
 * tap or key, and collapses to a short static mark when the system's animator scale is 0 ("Remove animations").
 * The intro is driven on Compose's virtual clock, so the timing assertions are exact and need no sleeps.
 */
@OptIn(ExperimentalTestApi::class)
@RunWith(AndroidJUnit4::class)
class LaunchE2eTest : E2eTest() {
    private val introMs: Long get() = Brand.load(Device.app).second.intro.durationMs.toLong()

    private fun launchPaused() {
        compose.mainClock.autoAdvance = false
        launch(skipIntro = false)
        waitForTag("intro")
        waitForTag("home")
    }

    @Test
    fun coldLaunchPlaysTheIntroThenShowsAUsableHome() {
        assertTrue("intro storyboard has a duration", introMs >= 500)
        launchPaused()
        compose.onNodeWithTag("intro").assertIsDisplayed()
            .assert(SemanticsMatcher.keyIsDefined(SemanticsActions.OnClick)) // skippable, announced to TalkBack
        // Half-way through, the intro still covers the home screen.
        compose.mainClock.advanceTimeBy(introMs / 2)
        assertTrue(exists("intro"))
        // After its duration (plus the hold and fade-out) it is gone without any input.
        compose.mainClock.advanceTimeBy(introMs / 2 + 1_000)
        compose.mainClock.autoAdvance = true
        waitForGone("intro")
        compose.onNodeWithTag("choose_files").assertIsDisplayed().assertHasClickAction()
        compose.onNodeWithTag("privacy_note").assertIsDisplayed()
        compose.onNodeWithTag("nav_settings").performClick()
        waitForTag("settings_screen")
    }

    @Test
    fun tapSkipsTheIntro() {
        launchPaused()
        compose.mainClock.advanceTimeBy(100)
        compose.onNodeWithTag("intro").performClick()
        compose.mainClock.advanceTimeBy(600)
        assertTrue("skipping must not take the whole intro", 700 < introMs)
        assertFalse("intro still shown after a tap", exists("intro"))
        compose.mainClock.autoAdvance = true
        compose.onNodeWithTag("choose_files").assertIsDisplayed()
    }

    @Test
    fun anyKeySkipsTheIntro() {
        launchPaused()
        compose.mainClock.advanceTimeBy(100)
        compose.onNodeWithTag("intro").performKeyInput { pressKey(Key.Spacebar) }
        compose.mainClock.advanceTimeBy(600)
        assertFalse("intro still shown after a key press", exists("intro"))
        compose.mainClock.autoAdvance = true
    }

    @Test
    fun removeAnimationsShowsAShortStaticIntro() {
        Device.setAnimatorScale(0f)
        launchPaused()
        compose.onNodeWithTag("intro").assertIsDisplayed()
        // The reduced-motion intro is a static mark held briefly: gone well before the animated one would end.
        compose.mainClock.advanceTimeBy(900)
        assertTrue(900 < introMs)
        assertFalse("reduced-motion intro must be short", exists("intro"))
        compose.mainClock.autoAdvance = true
        compose.onNodeWithTag("choose_files").assertIsDisplayed()
    }
}
