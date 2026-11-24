package app.convertible.core

import androidx.annotation.Keep
import java.nio.ByteBuffer

/**
 * Raw JNI entry points of `libconvertible_jni.so` (registered in `JNI_OnLoad`), one per C ABI call.
 * Use [NativeCore] instead: it owns result handles, cancellation and error mapping.
 *
 * Result handles returned by [convertBuffer] / [convertFile] must be released with [resultFree] exactly once;
 * a handle of 0 never occurs without a pending Kotlin exception.
 */
@Keep
internal object NativeBridge {
    init {
        System.loadLibrary("convertible_jni")
    }

    @JvmStatic external fun version(): String
    @JvmStatic external fun formatsJson(): String
    @JvmStatic external fun targetsJson(source: String?): String
    @JvmStatic external fun optionsSchemaJson(source: String?, target: String?): String

    /** [data] must be a direct buffer; the first [length] bytes from its start are examined. */
    @JvmStatic external fun detectBuffer(data: ByteBuffer?, length: Long, filename: String?): String?

    /** Reads at most [maxBytes] from the start of the file natively and identifies its format. */
    @JvmStatic external fun detectFile(path: String, filename: String?, maxBytes: Int): String?

    @JvmStatic external fun convertBuffer(
        data: ByteBuffer?, length: Long, filename: String?, source: String?, target: String?,
        options: String?, sink: NativeProgressSink?,
    ): Long

    @JvmStatic external fun convertFile(
        input: String?, output: String?, source: String?, target: String?,
        options: String?, sink: NativeProgressSink?,
    ): Long

    @JvmStatic external fun resultStatus(handle: Long): Int
    @JvmStatic external fun resultMessage(handle: Long): String
    @JvmStatic external fun resultFormat(handle: Long): String
    @JvmStatic external fun resultExtension(handle: Long): String
    @JvmStatic external fun resultMime(handle: Long): String
    @JvmStatic external fun resultWarningsJson(handle: Long): String
    @JvmStatic external fun resultSize(handle: Long): Long

    /** A direct buffer aliasing the result's bytes; invalid after [resultFree]. Null when there are none. */
    @JvmStatic external fun resultData(handle: Long): ByteBuffer?
    @JvmStatic external fun resultFree(handle: Long)
}

/** Called by the native progress trampoline on the converting thread. Return true to cancel. */
@Keep
fun interface NativeProgressSink {
    fun onProgress(fraction: Double, stage: String): Boolean
}
