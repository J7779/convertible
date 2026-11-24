package app.convertible.model

import app.convertible.core.CoreError
import app.convertible.core.CvtStatus
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Locale

class FormattingTest {
    private fun fmt(bytes: Long) = SizeFormat.format(bytes, Locale.US)

    @Test
    fun formatsSizesWithDecimalUnits() {
        assertEquals("0 B", fmt(0))
        assertEquals("999 B", fmt(999))
        assertEquals("1.0 KB", fmt(1000))
        assertEquals("1.5 KB", fmt(1536))
        assertEquals("12 KB", fmt(12_345))
        assertEquals("1.0 MB", fmt(999_999)) // rounds up into the next unit instead of "1000 KB"
        assertEquals("1.2 MB", fmt(1_234_567))
        assertEquals("4.3 GB", fmt(4_300_000_000))
        assertEquals("—", fmt(-1))
    }

    @Test
    fun usesTheLocaleDecimalSeparator() {
        assertEquals("1,5 KB", SizeFormat.format(1536, Locale.GERMANY))
    }

    @Test
    fun describesSizeChanges() {
        assertEquals(SizeChange.Smaller(50), SizeChange.of(1000, 500))
        assertEquals(SizeChange.Smaller(99), SizeChange.of(60_000_000, 240))
        assertEquals(SizeChange.Smaller(100), SizeChange.of(500, 0))
        assertEquals(SizeChange.Larger(33), SizeChange.of(3, 4))
        assertEquals(SizeChange.Same, SizeChange.of(10, 10))
        assertEquals(SizeChange.Smaller(0), SizeChange.of(100_000, 99_999))
        assertEquals(SizeChange.Larger(100), SizeChange.of(0, 10))
        assertEquals(SizeChange.Same, SizeChange.of(0, 0))
        assertNull(SizeFormat.changePercent(0, 5))
        assertEquals(-25, SizeFormat.changePercent(400, 300))
    }

    @Test
    fun mapsEveryStatusToItsError() {
        val expected = mapOf(
            CvtStatus.INVALID_ARGUMENT to CoreError.InvalidArgument::class,
            CvtStatus.UNKNOWN_FORMAT to CoreError.UnknownFormat::class,
            CvtStatus.UNSUPPORTED_CONVERSION to CoreError.UnsupportedConversion::class,
            CvtStatus.DECODE to CoreError.Decode::class,
            CvtStatus.ENCODE to CoreError.Encode::class,
            CvtStatus.IO to CoreError.Io::class,
            CvtStatus.CANCELLED to CoreError.Cancelled::class,
            CvtStatus.LIMIT to CoreError.Limit::class,
            CvtStatus.INTERNAL to CoreError.Internal::class,
        )
        for ((status, type) in expected) {
            val error = CoreError.from(status, "msg $status")
            assertEquals(type, error::class)
            assertEquals(status, error.status)
            assertEquals("msg $status", error.detail)
        }
        assertTrue(CoreError.from(42, "?") is CoreError.Internal)
        assertTrue(CoreError.from(CvtStatus.OK, "") is CoreError.Internal) // OK is never an error
    }

    @Test
    fun mapsErrorsToFriendlyKinds() {
        assertEquals(ErrorKind.DAMAGED, ErrorKind.of(CoreError.Decode("bad")))
        assertEquals(ErrorKind.TOO_LARGE, ErrorKind.of(CoreError.Limit("")))
        assertEquals(ErrorKind.CANCELLED, ErrorKind.of(CoreError.Cancelled("")))
        assertEquals(ErrorKind.UNSUPPORTED, ErrorKind.of(CoreError.UnsupportedConversion("")))
        assertEquals(ErrorKind.INTERNAL, ErrorKind.of(CoreError.Bridge("oom")))
        assertEquals(ErrorKind.FILE_ACCESS, ErrorKind.of(CoreError.Io("")))
        assertEquals(ErrorKind.entries.size, setOf(
            CoreError.InvalidArgument(""), CoreError.UnknownFormat(""), CoreError.UnsupportedConversion(""),
            CoreError.Decode(""), CoreError.Encode(""), CoreError.Io(""), CoreError.Cancelled(""),
            CoreError.Limit(""), CoreError.Internal(""),
        ).map { ErrorKind.of(it) }.toSet().size)
    }

    @Test
    fun sanitisesFileNames() {
        assertEquals("report.txt", FileNames.sanitize("../../etc/report.txt"))
        assertEquals("a_b_.txt", FileNames.sanitize("a:b?.txt"))
        assertEquals("file", FileNames.sanitize("   "))
        assertEquals("file", FileNames.sanitize(null))
        assertEquals("hidden", FileNames.sanitize(".hidden"))
        assertEquals("emoji 😀.txt", FileNames.sanitize("emoji 😀.txt"))
        val long = "x".repeat(300) + ".json"
        val cut = FileNames.sanitize(long)
        assertEquals(120, cut.length)
        assertTrue(cut.endsWith(".json"))
    }

    @Test
    fun namesOutputs() {
        assertEquals("notes.b64", FileNames.outputName("notes.txt", "b64"))
        assertEquals("archive.tar.zip", FileNames.outputName("archive.tar.gz", "zip"))
        assertEquals("photo-converted.png", FileNames.outputName("photo.png", "png"))
        assertEquals("README.hex", FileNames.outputName("README", "hex"))
        assertEquals("x.bin", FileNames.outputName("x.dat", ""))
        assertEquals("txt", FileNames.extension("a.b.txt"))
        assertEquals("", FileNames.extension(".profile"))
    }
}
