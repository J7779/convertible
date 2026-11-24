package app.convertible.core

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import app.convertible.json.Json
import app.convertible.json.asArray
import app.convertible.json.asObject
import app.convertible.model.Catalogue
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.Base64
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import kotlin.random.Random

/**
 * Real conversions through libconvertible_jni.so and the core, checking the produced bytes. Written against the
 * dynamic catalogue: it relies only on the always-present encoding family (base64, hex, checksums, txt, bin).
 */
@RunWith(AndroidJUnit4::class)
class NativeCoreTest {
    private val core = NativeCore()
    private lateinit var dir: File

    @Before
    fun setUp() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        dir = File(context.cacheDir, "native-test").apply { deleteRecursively(); mkdirs() }
    }

    @After
    fun tearDown() {
        dir.deleteRecursively()
    }

    private fun success(outcome: ConversionOutcome): ConversionOutcome.Success {
        if (outcome is ConversionOutcome.Failure) throw AssertionError("conversion failed: ${outcome.error}")
        return outcome as ConversionOutcome.Success
    }

    private fun failure(outcome: ConversionOutcome): CoreError =
        (outcome as? ConversionOutcome.Failure)?.error ?: throw AssertionError("expected a failure")

    private fun sha256(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }

    private fun sha256(file: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buf = ByteArray(1 shl 16)
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    @Test
    fun versionAndCatalogue() {
        assertTrue(core.version.matches(Regex("""\d+\.\d+\.\d+.*""")))
        val catalogue = Catalogue.parse(core.formatsJson())
        assertTrue(catalogue.formats.size >= 10)
        for (id in listOf("base64", "hex", "checksums", "txt")) assertNotNull(id, catalogue[id])
        // Most popular first.
        val pop = catalogue.formats.map { it.popularity }
        assertEquals(pop.sortedDescending(), pop)
        assertTrue(catalogue.formats.all { it.id.isNotEmpty() && it.category.isNotEmpty() })
    }

    @Test
    fun targetsAndOptionsSchemas() {
        val targets = Catalogue.parseTargets(core.targetsJson("txt"))
        assertTrue(targets.any { it.id == "base64" })
        assertTrue(targets.single { it.id == "txt" }.compress)
        assertEquals("[]", core.targetsJson("definitely-not-a-format"))
        val schema = Catalogue.parseOptions(core.optionsSchemaJson("txt", "base64"))
        val width = schema.single { it.key == "line_width" }
        assertEquals(app.convertible.model.OptionType.INT, width.type)
        assertEquals(app.convertible.model.OptionValue.IntValue(76), width.default)
    }

    @Test
    fun detection() {
        val txt = File(dir, "hello.txt").apply { writeText("hello world\n") }
        assertEquals("txt", core.detectFile(txt.path, txt.name))
        val b64 = File(dir, "data.b64").apply { writeText("aGVsbG8gd29ybGQ=\n") }
        assertEquals("base64", core.detectFile(b64.path, b64.name))
        val direct = ByteBuffer.allocateDirect(5).put("hello".toByteArray()).also { it.flip() }
        assertEquals("txt", core.detectBuffer(direct, filename = "x.txt"))
    }

    @Test
    fun base64RoundTripIsByteExact() {
        val text = "Hello, Convertible! ✓ naïve 😀 \u0000 end".toByteArray(Charsets.UTF_8)
        val encoded = success(core.convertBytes(text, "in.bin", "bin", "base64", """{"line_width":0}"""))
        assertEquals("base64", encoded.format)
        assertEquals(Base64.getEncoder().encodeToString(text), String(encoded.data!!, Charsets.US_ASCII).trim())
        val decoded = success(core.convertBytes(encoded.data!!, "in.b64", "base64", "bin"))
        assertArrayEquals(text, decoded.data)
    }

    @Test
    fun base64WrapsAt76ByDefault() {
        val bytes = Random(7).nextBytes(300)
        val out = String(success(core.convertBytes(bytes, null, "bin", "base64")).data!!, Charsets.US_ASCII)
        val lines = out.trimEnd('\n').split('\n')
        assertTrue(lines.dropLast(1).all { it.length == 76 })
        assertEquals(Base64.getEncoder().encodeToString(bytes), lines.joinToString(""))
    }

    @Test
    fun hexKnownVector() {
        val out = success(core.convertBytes("hello".toByteArray(), "h.txt", "bin", "hex"))
        assertEquals("68656c6c6f", String(out.data!!).trim())
        val back = success(core.convertBytes(out.data!!, "h.hex", "hex", "bin"))
        assertEquals("hello", String(back.data!!))
    }

    @Test
    fun checksumsMatchKnownVectors() {
        val report = String(success(core.convertBytes("abc".toByteArray(), "abc.txt", "bin", "checksums")).data!!)
        assertTrue(report, report.contains("CRC-32: 352441c2"))
        assertTrue(report, report.contains("MD5: 900150983cd24fb0d6963f7d28e17f72"))
        assertTrue(report, report.contains("SHA-1: a9993e364706816aba3e25717850c26c9cd0d89d"))
        assertTrue(report, report.contains("SHA-256: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"))
    }

    @Test
    fun textReEncodingWithBomAndWarnings() {
        val utf16 = success(core.convertBytes("héllo".toByteArray(Charsets.UTF_8), "a.txt", "txt", "txt", """{"encoding":"utf-16le","bom":true}"""))
        assertArrayEquals(byteArrayOf(0xFF.toByte(), 0xFE.toByte()) + "héllo".toByteArray(Charsets.UTF_16LE), utf16.data)
        val latin = success(core.convertBytes("h€llo".toByteArray(Charsets.UTF_8), "a.txt", "txt", "txt", """{"encoding":"latin1"}"""))
        assertEquals("h?llo", String(latin.data!!, Charsets.ISO_8859_1))
        assertEquals(1, latin.warnings.size)
        assertTrue(latin.warnings[0].contains("latin1"))
    }

    @Test
    fun nonAsciiStringsSurviveTheBoundary() {
        // Supplementary characters are where JNI's modified UTF-8 differs from real UTF-8.
        val name = "naïve 😀 файл 𝄞.txt"
        val file = File(dir, name).apply { writeText("abc") }
        val out = File(dir, "out 😀.txt")
        success(core.convertFile(file.path, out.path, null, "checksums", null))
        val report = out.readText()
        assertTrue(report, report.contains("File: $name"))
        val viaBuffer = String(success(core.convertBytes("abc".toByteArray(), name, "bin", "checksums")).data!!)
        assertTrue(viaBuffer.contains("File: $name"))
    }

    @Test
    fun errorStatusesMapToTypedErrors() {
        assertTrue(failure(core.convertBytes("x".toByteArray(), null, "bin", "no-such-format")) is CoreError.UnknownFormat)
        val decode = failure(core.convertBytes("@@@@".toByteArray(), null, "base64", "bin"))
        assertTrue(decode is CoreError.Decode)
        assertTrue(decode.detail.isNotEmpty())
        assertTrue(failure(core.convertBytes("abc".toByteArray(), "a.txt", "txt", "base64", "{not json")) is CoreError.InvalidArgument)
        assertTrue(failure(core.convertBytes("abc".toByteArray(), "a.txt", "txt", "pdf")) is CoreError.UnsupportedConversion)
        val missing = core.convertFile(File(dir, "missing").path, File(dir, "o").path, "bin", "base64", null)
        assertTrue(failure(missing) is CoreError.Io)
        // The safety limits of the core surface as Limit.
        val limited = core.convertBytes(ByteArray(4096), null, "bin", "base64", """{"limits":{"max_input_bytes":1024}}""")
        assertTrue(failure(limited) is CoreError.Limit)
    }

    @Test
    fun badArgumentsNeverCrash() {
        // U+0000 cannot cross the C boundary: rejected instead of silently truncating the path.
        val nul = core.convertFile(File(dir, "a\u0000b").path, File(dir, "o").path, null, "base64", null)
        assertTrue(failure(nul) is CoreError.Bridge)
        val heap = runCatching { core.convertBuffer(ByteBuffer.wrap(ByteArray(4)), null, null, "base64") }
        assertTrue(heap.exceptionOrNull() is IllegalArgumentException)
        // Empty input and null optional arguments are fine.
        assertNotNull(core.convertBytes(ByteArray(0), null, null, "hex"))
        val missing = runCatching { core.detectFile(File(dir, "does-not-exist").path, null) }
        assertTrue(missing.exceptionOrNull() is java.io.IOException)
    }

    @Test
    fun progressIsMonotonicAndOnTheCallingThread() {
        val bytes = Random(1).nextBytes(4 shl 20)
        val seen = mutableListOf<Double>()
        val stages = mutableSetOf<String>()
        val thread = Thread.currentThread()
        var otherThread = false
        success(
            core.convertBytes(bytes, null, "bin", "base64", null) { f, stage ->
                if (Thread.currentThread() !== thread) otherThread = true
                seen += f
                stages += stage
                false
            },
        )
        assertFalse(otherThread)
        assertTrue(seen.isNotEmpty())
        assertEquals(seen.sorted(), seen)
        assertTrue(seen.all { it in 0.0..1.0 })
        assertTrue(stages.all { it.isNotEmpty() })
    }

    @Test
    fun cancelStopsTheConversion() {
        val bytes = Random(2).nextBytes(8 shl 20)
        val immediately = core.convertBytes(bytes, null, "bin", "base64", null) { _, _ -> true }
        assertTrue(failure(immediately) is CoreError.Cancelled)
        var calls = 0
        val midway = core.convertBytes(bytes, null, "bin", "hex", null) { f, _ ->
            calls++
            f > 0.3
        }
        assertTrue(failure(midway) is CoreError.Cancelled)
        assertTrue(calls > 0)
        // File conversions leave no output behind when cancelled.
        val input = File(dir, "big.bin").apply { writeBytes(bytes) }
        val output = File(dir, "big.b64")
        assertTrue(failure(core.convertFile(input.path, output.path, "bin", "base64", null) { _, _ -> true }) is CoreError.Cancelled)
        assertFalse(output.exists())
    }

    @Test
    fun listenerExceptionsBecomeErrors() {
        val outcome = core.convertBytes(Random(3).nextBytes(1 shl 20), null, "bin", "base64", null) { _, _ -> throw IllegalStateException("listener broke") }
        val error = failure(outcome)
        assertTrue(error is CoreError.Bridge)
        assertEquals("listener broke", error.detail)
        // The bridge still works afterwards.
        assertEquals("6869", String(success(core.convertBytes("hi".toByteArray(), null, "bin", "hex")).data!!).trim())
    }

    @Test
    fun largeFileThroughThePathApi() {
        // 64 MiB never touches the Java heap on the core side: memory-mapped input, checksum report output.
        val big = File(dir, "large.bin")
        val random = Random(42)
        val chunk = ByteArray(1 shl 20)
        big.outputStream().use { out -> repeat(64) { random.nextBytes(chunk); out.write(chunk) } }
        val report = File(dir, "large.sum")
        val progress = mutableListOf<Double>()
        val ok = success(core.convertFile(big.path, report.path, null, "checksums", null) { f, _ -> progress += f; false })
        assertEquals("checksums", ok.format)
        assertNull(ok.data)
        val text = report.readText()
        assertTrue(text.contains("Size: ${64L shl 20} bytes"))
        assertTrue(text.contains("SHA-256: ${sha256(big)}"))
        assertTrue(progress.isNotEmpty())
    }

    @Test
    fun largeFileRoundTripThroughBase64Files() {
        val src = File(dir, "src.bin").apply { writeBytes(Random(9).nextBytes(24 shl 20)) }
        val b64 = File(dir, "src.b64")
        val back = File(dir, "back.bin")
        success(core.convertFile(src.path, b64.path, "bin", "base64", null))
        assertTrue(b64.length() > src.length())
        success(core.convertFile(b64.path, back.path, "base64", "bin", null))
        assertEquals(sha256(src), sha256(back))
    }

    @Test
    fun directBufferConversionDoesNotNeedAHeapCopyOfTheInput() {
        val size = 16 shl 20
        val direct = ByteBuffer.allocateDirect(size)
        val random = Random(5)
        val chunk = ByteArray(1 shl 16)
        while (direct.hasRemaining()) { random.nextBytes(chunk); direct.put(chunk, 0, minOf(chunk.size, direct.remaining())) }
        direct.flip()
        val copy = ByteArray(size).also { direct.duplicate().get(it) }
        val report = String(success(core.convertBuffer(direct, "blob.bin", "bin", "checksums")).data!!)
        assertTrue(report.contains("SHA-256: ${sha256(copy)}"))
        assertEquals(0, direct.position()) // the caller's buffer is untouched
    }

    @Test
    fun concurrentConversionsAreIndependent() {
        val pool = Executors.newFixedThreadPool(4)
        try {
            val jobs = (0 until 8).map { i ->
                pool.submit(Callable {
                    val bytes = Random(i).nextBytes(256 * 1024 + i)
                    val enc = success(core.convertBytes(bytes, null, "bin", "base64", null))
                    val dec = success(core.convertBytes(enc.data!!, null, "base64", "bin", null))
                    bytes.contentEquals(dec.data)
                })
            }
            assertTrue(jobs.all { it.get() })
        } finally {
            pool.shutdown()
        }
    }

    @Test
    fun warningsJsonIsAlwaysAnArray() {
        assertEquals(emptyList<String>(), NativeCore.parseWarnings("[]"))
        assertEquals(listOf("a", "b"), NativeCore.parseWarnings("""["a","b"]"""))
        assertEquals(emptyList<String>(), NativeCore.parseWarnings("garbage"))
        assertTrue(Json.parse(core.formatsJson()).asArray().first().asObject().containsKey("popularity"))
    }
}
