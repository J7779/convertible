package app.convertible.e2e.support

import android.content.Intent
import android.net.Uri
import androidx.compose.ui.semantics.SemanticsNode
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.SemanticsNodeInteraction
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.junit4.ComposeTestRule
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollToNode
import androidx.compose.ui.test.performTextInput
import androidx.lifecycle.ViewModelProvider
import androidx.test.core.app.ActivityScenario
import app.convertible.MainActivity
import app.convertible.e2e.fixtures.Fixtures
import app.convertible.e2e.fixtures.ShareFixtureProvider
import app.convertible.state.ItemStatus
import app.convertible.state.QueueItem
import app.convertible.ui.ConverterViewModel
import org.junit.Rule
import org.junit.rules.ExternalResource
import org.junit.rules.RuleChain
import java.io.File

/**
 * Base class of the end-to-end tests: the real app (MainActivity, real ViewModel, real libconvertible_jni.so)
 * driven through its UI with Compose UI Test, and system UI (file picker, save dialog, share sheet) with
 * UI Automator. Every test runs in a fresh, cleared app process (Android Test Orchestrator, clearPackageData).
 */
abstract class E2eTest {
    val compose: ComposeTestRule = createEmptyComposeRule()
    private val appRule = AppRule()

    @get:Rule
    val rules: RuleChain = RuleChain
        .outerRule(DeviceStateRule())
        .around(FixtureResetRule())
        .around(appRule)
        .around(compose)
        .around(FailureScreenshotRule())

    protected val scenario: ActivityScenario<MainActivity> get() = appRule.scenario ?: error("app not launched")

    val vm: ConverterViewModel
        get() {
            var model: ConverterViewModel? = null
            scenario.onActivity { model = ViewModelProvider(it)[ConverterViewModel::class.java] }
            return model!!
        }

    protected val items: List<QueueItem> get() = vm.state.value.items

    /** Launches the app (cold start in this process) and, unless asked not to, waits for the intro to end. */
    protected fun launch(intent: Intent = mainIntent(), skipIntro: Boolean = true): ActivityScenario<MainActivity> {
        val s = ActivityScenario.launch<MainActivity>(intent)
        appRule.scenario = s
        if (skipIntro) {
            waitForTag("home", LONG)
            skipIntro()
        }
        return s
    }

    protected fun skipIntro() {
        if (exists("intro")) runCatching { compose.onNodeWithTag("intro").performClick() }
        compose.waitUntil(LONG) { !exists("intro") }
    }

    protected fun mainIntent(): Intent = Intent(Device.app, MainActivity::class.java)
        .setAction(Intent.ACTION_MAIN)
        .addCategory(Intent.CATEGORY_LAUNCHER)

    /** "Share -> Convertible" from another app. */
    protected fun sendIntent(vararg names: String): Intent {
        val uris = names.map { ShareFixtureProvider.uri(it) }
        val intent = if (uris.size == 1) {
            Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_STREAM, uris[0])
        } else {
            Intent(Intent.ACTION_SEND_MULTIPLE).putParcelableArrayListExtra(Intent.EXTRA_STREAM, ArrayList<Uri>(uris))
        }
        return intent.setClass(Device.app, MainActivity::class.java)
            .setType(if (uris.size == 1) Fixtures.mime(names[0]) else "*/*")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }

    /** Launches with shared files and waits until all of them are imported and identified. */
    protected fun launchWith(vararg names: String): List<Long> {
        launch(sendIntent(*names))
        return awaitImported(names.size)
    }

    protected fun awaitImported(count: Int, timeout: Long = LONG): List<Long> {
        compose.waitUntil(timeout) { items.size >= count && items.all { it.status !is ItemStatus.Importing } }
        val ids = items.map { it.id }
        waitForTag("queue")
        return ids
    }

    fun item(id: Long): QueueItem = vm.store.item(id) ?: error("no item $id")

    protected fun exists(tag: String): Boolean = compose.onAllNodes(hasTestTag(tag)).fetchSemanticsNodes().isNotEmpty()

    protected fun waitForTag(tag: String, timeout: Long = MEDIUM) = compose.waitUntil(timeout) { exists(tag) }

    /** System Back (closing the keyboard first if it is open) until [tag] has gone. */
    protected fun pressBackUntilGone(tag: String) {
        repeat(3) {
            if (!exists(tag)) return
            Device.ui.pressBack()
            runCatching { compose.waitUntil(3_000) { !exists(tag) } }
        }
        waitForGone(tag)
    }

    protected fun existsUnmerged(tag: String): Boolean =
        compose.onAllNodes(hasTestTag(tag), useUnmergedTree = true).fetchSemanticsNodes().isNotEmpty()

    protected fun waitForGone(tag: String, timeout: Long = MEDIUM) = compose.waitUntil(timeout) { !exists(tag) }

    /** A node inside the (lazy) queue, scrolled into view first. */
    protected fun queueNode(tag: String): SemanticsNodeInteraction {
        // Lazy list: off-screen cards are not composed, so scroll first (retrying while state settles).
        compose.waitUntil(MEDIUM) {
            exists(tag) || runCatching { compose.onNodeWithTag("queue").performScrollToNode(hasTestTag(tag)) }.isSuccess
        }
        compose.onNodeWithTag("queue").performScrollToNode(hasTestTag(tag))
        return compose.onNodeWithTag(tag)
    }

    protected fun clickInQueue(tag: String) {
        queueNode(tag).performClick()
    }

    /** Opens the target picker of one file, searches and picks [targetId]. */
    protected fun chooseTarget(id: Long, targetId: String, search: String = targetId) {
        clickInQueue("pick_target_$id")
        waitForTag("target_picker")
        compose.onNodeWithTag("picker_search").performTextInput(search)
        waitForTag("target_$targetId")
        compose.onNodeWithTag("target_$targetId").performClick()
        waitForGone("target_picker")
        compose.waitUntil(MEDIUM) { item(id).target?.id == targetId }
    }

    /** Waits for item [id] to reach status [type]; an unexpected failure fails the test at once with its error. */
    fun <T : ItemStatus> awaitStatusOf(id: Long, type: Class<T>, timeout: Long = LONG): T {
        compose.waitUntil(timeout) {
            val s = vm.store.item(id)?.status
            if (s is ItemStatus.Failed && type != ItemStatus.Failed::class.java) throw AssertionError("conversion failed: ${s.error}")
            type.isInstance(s)
        }
        return type.cast(item(id).status)!!
    }

    inline fun <reified T : ItemStatus> awaitStatus(id: Long, timeout: Long = LONG): T = awaitStatusOf(id, T::class.java, timeout)

    protected fun convertAndWait(id: Long, timeout: Long = LONG): ItemStatus.Done {
        clickInQueue("convert_$id")
        return awaitStatus(id, timeout)
    }

    protected fun outputBytes(done: ItemStatus.Done): ByteArray = File(done.output.path).readBytes()

    /** Matches nodes whose test tag starts with [prefix]. */
    protected fun hasTagPrefix(prefix: String) = SemanticsMatcher("testTag starts with $prefix") {
        it.config.getOrNull(SemanticsProperties.TestTag)?.startsWith(prefix) == true
    }

    protected fun SemanticsNode.tag(): String? = config.getOrNull(SemanticsProperties.TestTag)

    companion object {
        const val MEDIUM = 10_000L
        const val LONG = 30_000L
        const val CONVERSION = 120_000L
    }
}

/** Closes the activity after the test (inside the device-state rule, outside the failure screenshot). */
private class AppRule : ExternalResource() {
    var scenario: ActivityScenario<MainActivity>? = null

    override fun after() {
        runCatching { scenario?.close() }
        scenario = null
    }
}

/** Forgets documents the fixture storage provider received in earlier tests. */
class FixtureResetRule : ExternalResource() {
    override fun before() {
        Device.app.contentResolver.call(ShareFixtureProvider.uri("x"), ShareFixtureProvider.METHOD_RESET, null, null)
    }
}
