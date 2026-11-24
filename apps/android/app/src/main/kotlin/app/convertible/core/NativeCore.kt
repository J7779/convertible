package app.convertible.core

import app.convertible.json.Json
import java.nio.ByteBuffer

/**
 * [CoreApi] over JNI. Owns native result handles (always freed, also on exceptions) and turns status codes into
 * [ConversionOutcome]s. Thread-safe like the core itself.
 */
class NativeCore : CoreApi {
    override val version: String by lazy { NativeBridge.version() }

    override fun formatsJson(): String = NativeBridge.formatsJson()

    override fun targetsJson(source: String): String = NativeBridge.targetsJson(source)

    override fun optionsSchemaJson(source: String, target: String): String =
        NativeBridge.optionsSchemaJson(source, target)

    override fun detectFile(path: String, filename: String?): String? =
        NativeBridge.detectFile(path, filename, DETECT_BYTES)

    /** Detects the format of the first [length] bytes of a direct buffer. */
    fun detectBuffer(data: ByteBuffer, length: Int = data.remaining(), filename: String? = null): String? {
        require(data.isDirect) { "detectBuffer needs a direct ByteBuffer" }
        return NativeBridge.detectBuffer(data.slice(), length.toLong(), filename)
    }

    override fun convertFile(
        input: String,
        output: String,
        source: String?,
        target: String,
        optionsJson: String?,
        progress: ProgressListener?,
    ): ConversionOutcome = execute(progress) { sink ->
        NativeBridge.convertFile(input, output, source?.ifEmpty { null }, target, optionsJson, sink)
    }

    /**
     * Converts bytes held in a direct buffer (from its position to its limit) without copying them onto the Java
     * heap. The converted bytes are copied once into [ConversionOutcome.Success.data].
     */
    fun convertBuffer(
        data: ByteBuffer,
        filename: String?,
        source: String?,
        target: String,
        optionsJson: String? = null,
        progress: ProgressListener? = null,
    ): ConversionOutcome {
        require(data.isDirect) { "convertBuffer needs a direct ByteBuffer" }
        val view = data.slice()
        return execute(progress, copyData = true) { sink ->
            NativeBridge.convertBuffer(view, view.remaining().toLong(), filename, source?.ifEmpty { null }, target, optionsJson, sink)
        }
    }

    /** Convenience for small inputs: copies [bytes] into a direct buffer first. */
    fun convertBytes(
        bytes: ByteArray,
        filename: String?,
        source: String?,
        target: String,
        optionsJson: String? = null,
        progress: ProgressListener? = null,
    ): ConversionOutcome {
        val direct = ByteBuffer.allocateDirect(bytes.size).put(bytes)
        direct.flip()
        return convertBuffer(direct, filename, source, target, optionsJson, progress)
    }

    private inline fun execute(
        progress: ProgressListener?,
        copyData: Boolean = false,
        call: (NativeProgressSink?) -> Long,
    ): ConversionOutcome {
        val sink = progress?.let { listener -> NativeProgressSink { f, s -> listener.onProgress(f, s) } }
        val handle = try {
            call(sink)
        } catch (e: OutOfMemoryError) {
            return ConversionOutcome.Failure(CoreError.Bridge("Not enough memory for this conversion."))
        } catch (e: RuntimeException) {
            // Thrown by a progress listener (rethrown after the core stopped) or by argument validation.
            return ConversionOutcome.Failure(CoreError.Bridge(e.message ?: e.javaClass.simpleName))
        }
        try {
            val status = NativeBridge.resultStatus(handle)
            if (status != CvtStatus.OK) {
                return ConversionOutcome.Failure(CoreError.from(status, NativeBridge.resultMessage(handle)))
            }
            val bytes = if (copyData) {
                NativeBridge.resultData(handle)?.asReadOnlyBuffer()?.let { buffer ->
                    ByteArray(buffer.remaining()).also { buffer.get(it) }
                } ?: ByteArray(0)
            } else {
                null
            }
            return ConversionOutcome.Success(
                format = NativeBridge.resultFormat(handle),
                extension = NativeBridge.resultExtension(handle),
                mime = NativeBridge.resultMime(handle),
                warnings = parseWarnings(NativeBridge.resultWarningsJson(handle)),
                data = bytes,
            )
        } finally {
            NativeBridge.resultFree(handle)
        }
    }

    companion object {
        /** How much of a file detection reads. */
        const val DETECT_BYTES = 1 shl 20

        internal fun parseWarnings(json: String): List<String> =
            (runCatching { Json.parse(json) }.getOrNull() as? List<*>)?.filterIsInstance<String>().orEmpty()
    }
}
