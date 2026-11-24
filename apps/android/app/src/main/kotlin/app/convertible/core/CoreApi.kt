package app.convertible.core

/**
 * The Convertible core as seen by the app: a platform-free interface so that state logic can be unit-tested
 * on the JVM with a fake, while [NativeCore] implements it over JNI.
 *
 * All calls are synchronous; call them off the main thread. Convert calls block until done.
 */
interface CoreApi {
    /** Core library version, e.g. "0.1.0". */
    val version: String

    /** `cvt_formats_json`. */
    fun formatsJson(): String

    /** `cvt_targets_json`; "[]" for unknown ids. */
    fun targetsJson(source: String): String

    /** `cvt_options_schema_json`; "[]" when the conversion has no options. */
    fun optionsSchemaJson(source: String, target: String): String

    /** Detects the format of a file on disk from its first bytes and its name. Null when unrecognised. */
    fun detectFile(path: String, filename: String?): String?

    /**
     * Converts the file at [input] and writes the result to [output] (memory-mapped input; nothing passes
     * through the Java heap). [progress] may cancel by returning true.
     */
    fun convertFile(
        input: String,
        output: String,
        source: String?,
        target: String,
        optionsJson: String?,
        progress: ProgressListener? = null,
    ): ConversionOutcome
}

/** Progress from the core: [fraction] in 0..1, never decreasing; [stage] is a short label. Return true to cancel. */
fun interface ProgressListener {
    fun onProgress(fraction: Double, stage: String): Boolean
}

/** Status codes of `enum cvt_status`. */
object CvtStatus {
    const val OK = 0
    const val INVALID_ARGUMENT = 1
    const val UNKNOWN_FORMAT = 2
    const val UNSUPPORTED_CONVERSION = 3
    const val DECODE = 4
    const val ENCODE = 5
    const val IO = 6
    const val CANCELLED = 7
    const val LIMIT = 8
    const val INTERNAL = 9
}

/** Outcome of one conversion. */
sealed interface ConversionOutcome {
    /** [data] holds the bytes for in-memory conversions and is null for file conversions. */
    class Success(
        val format: String,
        val extension: String,
        val mime: String,
        val warnings: List<String>,
        val data: ByteArray? = null,
    ) : ConversionOutcome

    data class Failure(val error: CoreError) : ConversionOutcome
}

/** A failed conversion, one subtype per core status plus [Bridge] for failures on the Kotlin side of JNI. */
sealed class CoreError(val status: Int, open val detail: String) {
    data class InvalidArgument(override val detail: String) : CoreError(CvtStatus.INVALID_ARGUMENT, detail)
    data class UnknownFormat(override val detail: String) : CoreError(CvtStatus.UNKNOWN_FORMAT, detail)
    data class UnsupportedConversion(override val detail: String) : CoreError(CvtStatus.UNSUPPORTED_CONVERSION, detail)
    data class Decode(override val detail: String) : CoreError(CvtStatus.DECODE, detail)
    data class Encode(override val detail: String) : CoreError(CvtStatus.ENCODE, detail)
    data class Io(override val detail: String) : CoreError(CvtStatus.IO, detail)
    data class Cancelled(override val detail: String) : CoreError(CvtStatus.CANCELLED, detail)
    data class Limit(override val detail: String) : CoreError(CvtStatus.LIMIT, detail)
    data class Internal(override val detail: String) : CoreError(CvtStatus.INTERNAL, detail)

    /** The binding itself failed (e.g. out of memory, or an exception thrown by a progress listener). */
    data class Bridge(override val detail: String) : CoreError(-1, detail)

    companion object {
        /** Maps a `cvt_status` code and `cvt_result_message` to the matching subtype. Unknown codes are Internal. */
        fun from(status: Int, message: String): CoreError = when (status) {
            CvtStatus.INVALID_ARGUMENT -> InvalidArgument(message)
            CvtStatus.UNKNOWN_FORMAT -> UnknownFormat(message)
            CvtStatus.UNSUPPORTED_CONVERSION -> UnsupportedConversion(message)
            CvtStatus.DECODE -> Decode(message)
            CvtStatus.ENCODE -> Encode(message)
            CvtStatus.IO -> Io(message)
            CvtStatus.CANCELLED -> Cancelled(message)
            CvtStatus.LIMIT -> Limit(message)
            else -> Internal(message)
        }
    }
}
