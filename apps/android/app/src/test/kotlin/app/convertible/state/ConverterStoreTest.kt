package app.convertible.state

import app.convertible.core.ConversionOutcome
import app.convertible.core.CoreApi
import app.convertible.core.CoreError
import app.convertible.core.ProgressListener
import app.convertible.json.Json
import app.convertible.model.Fixtures
import app.convertible.model.OptionValue
import app.convertible.model.Preset
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.nio.file.Files

/** A scripted core: detection by extension, the fixture catalogue, and conversions that write a file. */
private class FakeCore : CoreApi {
    data class Call(val input: String, val output: String, val source: String?, val target: String, val options: String?)

    val calls = mutableListOf<Call>()
    var outcome: (Call) -> ConversionOutcome = { call ->
        File(call.output).writeText("converted:" + File(call.input).readText())
        ConversionOutcome.Success(call.target, "", "text/plain", emptyList())
    }
    var progressSteps = 4
    var onProgress: (Double) -> Unit = {}

    override val version = "9.9.9"
    override fun formatsJson() = Fixtures.FORMATS_JSON
    override fun targetsJson(source: String) = when (source) {
        "png" -> Fixtures.PNG_TARGETS_JSON
        "bin" -> """[{"id":"base64","name":"Base64 text","category":"encoding","extensions":["b64"],"mime":"text/plain","lossy":false,"compress":false}]"""
        else -> "[]"
    }
    override fun optionsSchemaJson(source: String, target: String) = if (target == "jpg") Fixtures.JPG_SCHEMA_JSON else "[]"
    override fun detectFile(path: String, filename: String?) = when (filename?.substringAfterLast('.')) {
        "png" -> "png"
        else -> null
    }

    override fun convertFile(input: String, output: String, source: String?, target: String, optionsJson: String?, progress: ProgressListener?): ConversionOutcome {
        val call = Call(input, output, source, target, optionsJson)
        calls += call
        for (i in 1..progressSteps) {
            val f = i.toDouble() / progressSteps
            onProgress(f)
            if (progress?.onProgress(f, "Encoding") == true) return ConversionOutcome.Failure(CoreError.Cancelled("cancelled"))
        }
        return outcome(call)
    }
}

private class TempWorkspace(val root: File) : Workspace {
    override fun outputPath(itemId: Long, name: String): String = File(File(root, "out/$itemId").apply { mkdirs() }, name).path
    override fun rename(path: String, newName: String): String {
        val to = File(File(path).parentFile, newName)
        return if (File(path).renameTo(to)) to.path else path
    }
    override fun size(path: String) = File(path).length()
    override fun deleteItem(itemId: Long) {
        File(root, "in/$itemId").deleteRecursively()
        File(root, "out/$itemId").deleteRecursively()
    }
    override fun delete(path: String) {
        File(path).delete()
    }

    fun input(itemId: Long, name: String, content: String): ImportedFile {
        val f = File(File(root, "in/$itemId").apply { mkdirs() }, name)
        f.writeText(content)
        return ImportedFile(f.path, name, f.length())
    }
}

@OptIn(ExperimentalCoroutinesApi::class)
class ConverterStoreTest {
    private lateinit var dir: File
    private lateinit var workspace: TempWorkspace
    private val core = FakeCore()
    private var settings = AppSettings()

    @Before
    fun setUp() {
        dir = Files.createTempDirectory("store").toFile()
        workspace = TempWorkspace(dir)
    }

    @After
    fun tearDown() {
        dir.deleteRecursively()
    }

    private fun TestScope.newStore(): ConverterStore {
        val dispatcher = StandardTestDispatcher(testScheduler)
        // The test scope itself (not backgroundScope) so that advanceUntilIdle() runs the store's work.
        return ConverterStore(core, workspace, this, dispatcher, { settings }).also { it.start() }
    }

    private fun ConverterStore.addFile(name: String, content: String = "data"): Long =
        add(name, content.length.toLong()) { id -> workspace.input(id, name, content) }

    @Test
    fun loadsCatalogueAndVersion() = runTest {
        val store = newStore()
        advanceUntilIdle()
        assertEquals(12, store.state.value.catalogue.formats.size)
        assertEquals("9.9.9", store.state.value.coreVersion)
    }

    @Test
    fun importDetectsFormatAndLoadsTargets() = runTest {
        val store = newStore()
        val id = store.addFile("photo.png")
        assertEquals(ItemStatus.Importing, store.item(id)!!.status)
        advanceUntilIdle()
        val item = store.item(id)!!
        assertEquals(ItemStatus.Ready, item.status)
        assertEquals("png", item.sourceId)
        assertEquals("PNG image", item.source?.name)
        assertEquals(10, item.targets.size)
        assertNull(item.target)
        assertFalse(item.canConvert)
    }

    @Test
    fun unrecognisedFilesFallBackToUniversalTargets() = runTest {
        val store = newStore()
        val id = store.addFile("mystery.dat")
        advanceUntilIdle()
        val item = store.item(id)!!
        assertEquals(ConverterStore.UNKNOWN_SOURCE, item.sourceId)
        assertEquals(listOf("base64"), item.targets.map { it.id })
    }

    @Test
    fun importFailuresAreReported() = runTest {
        val store = newStore()
        val id = store.add("gone.png", -1) { throw ImportException(ImportProblem.NO_SPACE, "full") }
        advanceUntilIdle()
        val status = store.item(id)!!.status as ItemStatus.ImportFailed
        assertEquals(ImportProblem.NO_SPACE, status.reason)
        val other = store.add("x.png", -1) { throw IllegalStateException("boom") }
        advanceUntilIdle()
        assertEquals(ImportProblem.UNREADABLE, (store.item(other)!!.status as ItemStatus.ImportFailed).reason)
    }

    @Test
    fun choosingATargetLoadsItsSchemaAndKeepsKnownValues() = runTest {
        val store = newStore()
        val id = store.addFile("photo.png")
        advanceUntilIdle()
        store.setTarget(id, "jpg")
        advanceUntilIdle()
        var item = store.item(id)!!
        assertEquals("jpg", item.target?.id)
        assertEquals(5, item.schema.size)
        assertTrue(item.canConvert)
        store.setOption(id, "quality", OptionValue.IntValue(500))
        store.setOption(id, "nonsense", OptionValue.IntValue(1))
        item = store.item(id)!!
        assertEquals(mapOf("quality" to OptionValue.IntValue(100)), item.values)
        store.setOption(id, "quality", null)
        assertTrue(store.item(id)!!.values.isEmpty())
        store.setTarget(id, "not-offered")
        advanceUntilIdle()
        assertEquals("jpg", store.item(id)!!.target?.id)
    }

    @Test
    fun filesOfTheSameFormatFormAGroupAndShareTargets() = runTest {
        val store = newStore()
        val a = store.addFile("a.png")
        val b = store.addFile("b.png")
        store.addFile("c.dat")
        advanceUntilIdle()
        val group = store.state.value.groups.single()
        assertEquals("png", group.sourceId)
        assertEquals(listOf(a, b), group.itemIds)
        assertNull(group.commonTarget)
        store.setTargetForSource("png", "webp")
        advanceUntilIdle()
        assertEquals("webp", store.item(a)!!.target?.id)
        assertEquals("webp", store.item(b)!!.target?.id)
        assertEquals("webp", store.state.value.groups.single().commonTarget?.id)
        // A file added later inherits its siblings' target.
        val d = store.addFile("d.png")
        advanceUntilIdle()
        assertEquals("webp", store.item(d)!!.target?.id)
    }

    @Test
    fun convertsAndReportsTheOutput() = runTest {
        val store = newStore()
        val progress = mutableListOf<Float>()
        val id = store.addFile("photo.png", "12345")
        advanceUntilIdle()
        store.setTarget(id, "webp")
        advanceUntilIdle()
        core.onProgress = { (store.item(id)!!.status as? ItemStatus.Converting)?.let { progress += it.fraction } }
        store.convert(id)
        assertEquals(ItemStatus.Queued, store.item(id)!!.status)
        advanceUntilIdle()
        val done = store.item(id)!!.status as ItemStatus.Done
        assertEquals("photo.webp", done.output.name)
        assertEquals("converted:12345".length.toLong(), done.output.size)
        assertEquals("webp", done.output.format)
        assertTrue(File(done.output.path).exists())
        assertEquals(listOf(0f, 0.25f, 0.5f, 0.75f), progress) // state seen before each callback
        assertEquals("png", core.calls.single().source)
        assertNull(core.calls.single().options)
    }

    @Test
    fun usesTheCoreExtensionForTheOutputName() = runTest {
        val store = newStore()
        core.outcome = { call ->
            File(call.output).writeText("x")
            ConversionOutcome.Success("checksums", "sha", "text/plain", listOf("note one"))
        }
        val id = store.addFile("photo.png")
        advanceUntilIdle()
        store.setTarget(id, "checksums")
        advanceUntilIdle()
        store.convert(id)
        advanceUntilIdle()
        val output = (store.item(id)!!.status as ItemStatus.Done).output
        assertEquals("photo.sha", output.name)
        assertTrue(File(output.path).exists())
        assertEquals(listOf("note one"), output.warnings)
    }

    @Test
    fun sendsResolvedOptionsWithAppDefaults() = runTest {
        settings = AppSettings(defaultPreset = Preset.SMALL, stripMetadata = false)
        val store = newStore()
        val id = store.addFile("photo.png")
        advanceUntilIdle()
        store.setTarget(id, "jpg")
        advanceUntilIdle()
        store.setOption(id, "quality", OptionValue.IntValue(42))
        store.convert(id)
        advanceUntilIdle()
        val options = Json.parse(core.calls.single().options!!)
        assertEquals(mapOf("quality" to 42L, "preset" to "small", "strip_metadata" to false), options)
    }

    @Test
    fun failuresKeepTheErrorAndLeaveNoOutput() = runTest {
        val store = newStore()
        core.outcome = { ConversionOutcome.Failure(CoreError.Decode("bad data at offset 3")) }
        val id = store.addFile("photo.png")
        advanceUntilIdle()
        store.setTarget(id, "webp")
        advanceUntilIdle()
        store.convert(id)
        advanceUntilIdle()
        val failed = store.item(id)!!.status as ItemStatus.Failed
        assertEquals(CoreError.Decode("bad data at offset 3"), failed.error)
        assertTrue(store.item(id)!!.canConvert) // try again
        assertFalse(File(dir, "out/$id/photo.webp").exists())
    }

    @Test
    fun cancelStopsARunningConversion() = runTest {
        val store = newStore()
        val id = store.addFile("photo.png")
        advanceUntilIdle()
        store.setTarget(id, "webp")
        advanceUntilIdle()
        core.progressSteps = 10
        // The user taps Cancel while the core is half way.
        core.onProgress = { if (it >= 0.5) store.cancel(id) }
        store.convert(id)
        advanceUntilIdle()
        val failed = store.item(id)!!.status as ItemStatus.Failed
        assertTrue(failed.error is CoreError.Cancelled)
        assertTrue(store.item(id)!!.canConvert)
    }

    @Test
    fun cancelDuringAStageWithoutProgressDiscardsTheResult() = runTest {
        val store = newStore()
        val id = store.addFile("photo.png")
        advanceUntilIdle()
        store.setTarget(id, "webp")
        advanceUntilIdle()
        // The core gives no progress callback after the tap and finishes anyway.
        core.progressSteps = 1
        core.outcome = { call ->
            store.cancel(id)
            File(call.output).writeText("late")
            ConversionOutcome.Success(call.target, "", "image/webp", emptyList())
        }
        store.convert(id)
        advanceUntilIdle()
        val failed = store.item(id)!!.status as ItemStatus.Failed
        assertTrue(failed.error is CoreError.Cancelled)
        assertFalse(File(dir, "out/$id/photo.webp").exists())
    }

    @Test
    fun cancelAllStopsQueuedWork() = runTest {
        val store = newStore()
        val ids = (1..3).map { store.addFile("f$it.png") }
        advanceUntilIdle()
        store.setTargetForSource("png", "webp")
        advanceUntilIdle()
        assertEquals(3, store.state.value.convertibleCount)
        store.convertAll()
        assertEquals(3, store.state.value.activeCount)
        store.cancelAll()
        advanceUntilIdle()
        ids.forEach { assertTrue(((store.item(it)!!.status as ItemStatus.Failed).error) is CoreError.Cancelled) }
        assertTrue(core.calls.isEmpty())
    }

    @Test
    fun convertAgainAndRemove() = runTest {
        val store = newStore()
        val id = store.addFile("photo.png")
        advanceUntilIdle()
        store.setTarget(id, "webp")
        advanceUntilIdle()
        store.convert(id)
        advanceUntilIdle()
        val first = (store.item(id)!!.status as ItemStatus.Done).output
        assertEquals(1, store.state.value.doneItems.size)
        store.reset(id)
        assertEquals(ItemStatus.Ready, store.item(id)!!.status)
        assertFalse(File(first.path).exists())
        assertEquals("webp", store.item(id)!!.target?.id) // same target kept
        store.remove(id)
        advanceUntilIdle()
        assertNull(store.item(id))
        assertFalse(File(dir, "in/$id").exists())
        assertTrue(store.state.value.isEmpty)
    }

    @Test
    fun busyItemsIgnoreEdits() = runTest {
        val store = newStore()
        val id = store.addFile("photo.png")
        advanceUntilIdle()
        store.setTarget(id, "jpg")
        advanceUntilIdle()
        store.convert(id)
        store.setOption(id, "quality", OptionValue.IntValue(10))
        store.convert(id) // already queued: no second job
        advanceUntilIdle()
        assertTrue(store.item(id)!!.values.isEmpty())
        assertEquals(1, core.calls.size)
    }
}
