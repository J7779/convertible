package app.convertible.model

import app.convertible.core.CoreError
import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt

/** Human-readable sizes, decimal units (1 KB = 1000 bytes) like the Android system UI. */
object SizeFormat {
    private val UNITS = listOf("KB", "MB", "GB", "TB", "PB")

    fun format(bytes: Long, locale: Locale = Locale.getDefault()): String {
        if (bytes < 0) return "—"
        if (bytes < 1000) return "$bytes B"
        var value = bytes.toDouble()
        var unit = -1
        while (value >= 999.95 && unit < UNITS.lastIndex) {
            value /= 1000.0
            unit++
        }
        val text = if (value < 10) String.format(locale, "%.1f", value) else String.format(locale, "%.0f", value)
        return "$text ${UNITS[unit]}"
    }

    /** Signed change from [before] to [after] in whole percent: negative = smaller. Null when [before] is 0. */
    fun changePercent(before: Long, after: Long): Int? {
        if (before <= 0) return null
        return ((after - before) * 100.0 / before).roundToInt()
    }
}

/** How a size change should be described. */
sealed interface SizeChange {
    data class Smaller(val percent: Int) : SizeChange
    data class Larger(val percent: Int) : SizeChange
    data object Same : SizeChange

    companion object {
        fun of(before: Long, after: Long): SizeChange {
            val pct = SizeFormat.changePercent(before, after) ?: return if (after > 0) Larger(100) else Same
            return when {
                // Never claim 100% smaller while something is left.
                pct < 0 -> Smaller(if (after > 0) minOf(abs(pct), 99) else abs(pct))
                pct > 0 -> Larger(pct)
                after < before -> Smaller(0)
                after > before -> Larger(0)
                else -> Same
            }
        }
    }
}

/** Friendly error categories; the UI maps each to a title string. */
enum class ErrorKind {
    INVALID_SETTINGS, UNRECOGNISED, UNSUPPORTED, DAMAGED, CANNOT_WRITE, FILE_ACCESS, CANCELLED, TOO_LARGE, INTERNAL;

    companion object {
        fun of(error: CoreError): ErrorKind = when (error) {
            is CoreError.InvalidArgument -> INVALID_SETTINGS
            is CoreError.UnknownFormat -> UNRECOGNISED
            is CoreError.UnsupportedConversion -> UNSUPPORTED
            is CoreError.Decode -> DAMAGED
            is CoreError.Encode -> CANNOT_WRITE
            is CoreError.Io -> FILE_ACCESS
            is CoreError.Cancelled -> CANCELLED
            is CoreError.Limit -> TOO_LARGE
            is CoreError.Internal, is CoreError.Bridge -> INTERNAL
        }
    }
}

/** Output file naming. */
object FileNames {
    /** Strips directory parts and characters that are unsafe in file names; never returns an empty name. */
    fun sanitize(name: String?): String {
        val base = (name ?: "").substringAfterLast('/').substringAfterLast('\\')
        val cleaned = buildString {
            for (c in base) {
                if (c.code < 0x20 || c == '\u007F' || c in "<>:\"|?*") append('_') else append(c)
            }
        }.trim().trimStart('.')
        val limited = if (cleaned.length > 120) {
            val ext = extension(cleaned)
            cleaned.take(120 - ext.length - 1) + if (ext.isNotEmpty()) ".$ext" else ""
        } else {
            cleaned
        }
        return limited.ifEmpty { "file" }
    }

    fun baseName(name: String): String {
        val dot = name.lastIndexOf('.')
        return if (dot > 0) name.substring(0, dot) else name
    }

    fun extension(name: String): String {
        val dot = name.lastIndexOf('.')
        return if (dot > 0 && dot < name.length - 1) name.substring(dot + 1) else ""
    }

    /** "report.txt" + "b64" -> "report.b64"; a name equal to the input gets a "-converted" suffix. */
    fun outputName(inputName: String, extension: String): String {
        val base = baseName(inputName)
        val ext = extension.ifEmpty { "bin" }
        val candidate = "$base.$ext"
        return if (candidate.equals(inputName, ignoreCase = true)) "$base-converted.$ext" else candidate
    }
}
