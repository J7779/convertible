package app.convertible.e2e.support

import androidx.test.uiautomator.By
import androidx.test.uiautomator.BySelector
import androidx.test.uiautomator.Direction
import androidx.test.uiautomator.UiObject2
import androidx.test.uiautomator.Until
import app.convertible.e2e.fixtures.FixtureDocumentsProvider
import app.convertible.e2e.fixtures.ShareFixtureProvider
import java.util.regex.Pattern

/**
 * System UI driven with UI Automator: the Storage Access Framework picker / save dialog (DocumentsUI, AOSP or
 * Google build) and the share sheet (IntentResolver or the framework chooser). Selectors use package-agnostic
 * resource-id suffixes and visible text so they work across emulator images.
 */
object SystemUi {
    private const val TIMEOUT = 15_000L
    private val DOCUMENTS_UI: Pattern = Pattern.compile(".*documentsui")
    private val CHOOSER: Pattern = Pattern.compile("com\\.android\\.intentresolver|android")

    private val ui get() = Device.ui

    private fun require(selector: BySelector, what: String, timeout: Long = TIMEOUT): UiObject2 =
        ui.wait(Until.findObject(selector), timeout) ?: throw AssertionError("system UI: $what not found\n${hierarchy()}")

    private fun hierarchy(): String = runCatching {
        val out = java.io.ByteArrayOutputStream()
        ui.dumpWindowHierarchy(out)
        out.toString().lines().filter { "text=\"\"" !in it || "content-desc=\"\" " !in it }.take(80).joinToString("\n")
    }.getOrDefault("")

    fun awaitDocumentsUi() {
        require(By.pkg(DOCUMENTS_UI).depth(0), "file picker")
        ui.waitForIdle()
    }

    /** Opens the roots drawer and selects the fixture storage root. */
    private fun openFixtureRoot() {
        awaitDocumentsUi()
        val title = FixtureDocumentsProvider.ROOT_TITLE
        // Already there (DocumentsUI remembers the last root per calling app)?
        if (ui.hasObject(By.res(DOCUMENTS_UI_ID("breadcrumb_text")).text(title))) return
        require(By.desc("Show roots"), "roots drawer button").click()
        val roots = require(By.res(DOCUMENTS_UI_ID("roots_list")), "roots list")
        val root = roots.findObject(By.text(title))
            ?: roots.scrollUntil(Direction.DOWN, Until.findObject(By.text(title)))
            ?: throw AssertionError("fixture root not listed\n${hierarchy()}")
        root.click()
        require(By.res(DOCUMENTS_UI_ID("breadcrumb_text")).text(title), "fixture root opened")
        ui.waitForIdle()
    }

    private fun findFile(name: String): UiObject2 {
        val list = require(By.res(DOCUMENTS_UI_ID("dir_list")), "file list")
        val found = list.findObject(By.text(name))
            ?: list.scrollUntil(Direction.DOWN, Until.findObject(By.text(name)))
            ?: list.scrollUntil(Direction.UP, Until.findObject(By.text(name)))
            ?: throw AssertionError("file $name not shown\n${hierarchy()}")
        return found
    }

    /** In the open-document picker: picks [names] from the fixture root (several = long-press, tap, "Select"). */
    fun pickFixtures(vararg names: String) {
        openFixtureRoot()
        if (names.size == 1) {
            findFile(names[0]).click()
        } else {
            findFile(names[0]).longClick()
            require(By.textContains("selected"), "selection mode")
            names.drop(1).forEach { findFile(it).click() }
            require(By.textContains("${names.size} selected"), "${names.size} selected")
            require(By.text(Pattern.compile("(?i)select")).clickable(true), "Select button").click()
        }
        ui.wait(Until.gone(By.pkg(DOCUMENTS_UI).depth(0)), TIMEOUT)
    }

    /**
     * In the save dialog: chooses the fixture root, checks the proposed file name and saves. Returns the name the
     * dialog proposed.
     */
    fun saveToFixtures(): String {
        openFixtureRoot()
        val field = require(By.res("android:id/title").clazz("android.widget.EditText"), "file name field")
        val proposed = field.text
        require(By.res("android:id/button1"), "Save button").click()
        ui.wait(Until.gone(By.pkg(DOCUMENTS_UI).depth(0)), TIMEOUT)
        return proposed
    }

    /** In the folder picker: chooses the fixture root, "Use this folder", then allows access. */
    fun chooseFixtureFolder() {
        openFixtureRoot()
        require(By.text(Pattern.compile("(?i)use this folder")), "Use this folder").click()
        require(By.text(Pattern.compile("(?i)allow")).clickable(true), "Allow").click()
        ui.wait(Until.gone(By.pkg(DOCUMENTS_UI).depth(0)), TIMEOUT)
    }

    fun cancelDocumentsUi() {
        ui.pressBack()
        ui.wait(Until.gone(By.pkg(DOCUMENTS_UI).depth(0)), TIMEOUT)
    }

    /** Waits for the share sheet and returns every visible label in it. */
    fun awaitChooserLabels(): List<String> {
        require(By.pkg(CHOOSER).depth(0), "share sheet")
        ui.waitForIdle()
        // Give the target list time to load (the sheet fills in asynchronously).
        ui.wait(Until.hasObject(By.pkg(CHOOSER).res(Pattern.compile(".*:id/(text1|text|chooser_row_text_option|resolver_list|chooser_list_header|title).*"))), 5_000)
        return ui.findObjects(By.pkg(CHOOSER)).flatMap { listOfNotNull(it.text, it.contentDescription) }.filter { it.isNotBlank() }
    }

    fun dismissChooser() {
        ui.pressBack()
        ui.wait(Until.gone(By.pkg(CHOOSER).depth(0)), TIMEOUT)
    }

    /** Names of the documents the fixture storage provider received. */
    fun savedDocuments(): List<String> =
        Device.app.contentResolver.call(ShareFixtureProvider.uri("x"), ShareFixtureProvider.METHOD_LIST, null, null)
            ?.getStringArray(ShareFixtureProvider.KEY_NAMES)?.toList().orEmpty()

    @Suppress("FunctionName")
    private fun DOCUMENTS_UI_ID(id: String): Pattern = Pattern.compile(".*documentsui:id/$id")
}
