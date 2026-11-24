// JNI binding of the Convertible C ABI for the Android app.
//
// Deliberately thin: every function maps to one cvt_* call. Kotlin owns all
// policy (threading, cancellation, error presentation). Rules followed here:
//   - strings cross the boundary as UTF-16 (NewString / GetStringRegion) and
//     are converted to and from standard UTF-8 in utf.cpp, never through the
//     modified-UTF-8 *StringUTF* functions;
//   - large inputs arrive either as a path (cvt_convert_file memory-maps it)
//     or as a direct ByteBuffer, so file contents never pass through the Java
//     heap; converted bytes are exposed as a read-only direct ByteBuffer that
//     aliases the cvt_result until it is freed;
//   - the progress callback calls back into Kotlin on the converting thread,
//     reuses its stage string while the stage is unchanged and is throttled to
//     about 60 calls per second; a `true` answer (or a Kotlin exception, which
//     stays pending and is rethrown when the native call returns) cancels;
//   - every local reference created in a loop is released, natives are
//     registered in JNI_OnLoad so only that symbol is exported.
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <jni.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <convertible/convertible.h>

#include "utf.hpp"

namespace {

constexpr const char* kBridgeClass = "app/convertible/core/NativeBridge";
constexpr const char* kSinkClass = "app/convertible/core/NativeProgressSink";

jmethodID g_on_progress = nullptr;  // NativeProgressSink.onProgress(DLjava/lang/String;)Z

// ----------------------------------------------------------------------------- helpers

void throw_java(JNIEnv* env, const char* class_name, const char* message) {
  if (env->ExceptionCheck()) return;  // keep the first exception
  jclass cls = env->FindClass(class_name);
  if (cls != nullptr) {
    env->ThrowNew(cls, message);  // message is ASCII, so modified UTF-8 is fine here
    env->DeleteLocalRef(cls);
  }
}

// UTF-8 (standard) -> java.lang.String. Returns nullptr with an exception pending on failure.
jstring to_jstring(JNIEnv* env, std::string_view utf8) {
  const std::u16string utf16 = cvt_jni::utf8_to_utf16(utf8);
  return env->NewString(reinterpret_cast<const jchar*>(utf16.data()), static_cast<jsize>(utf16.size()));
}

// Takes ownership of a char* returned by the core.
jstring to_jstring_owned(JNIEnv* env, char* owned) {
  if (owned == nullptr) return nullptr;
  jstring result = to_jstring(env, owned);
  cvt_string_free(owned);
  return result;
}

// java.lang.String -> UTF-8. nullopt for a null reference. Strings containing
// U+0000 cannot be represented in the C ABI (NUL-terminated); they raise
// IllegalArgumentException and set `ok` to false rather than being truncated.
std::optional<std::string> from_jstring(JNIEnv* env, jstring value, bool& ok) {
  if (value == nullptr) return std::nullopt;
  const jsize length = env->GetStringLength(value);
  std::u16string utf16(static_cast<std::size_t>(length), u'\0');
  if (length > 0) env->GetStringRegion(value, 0, length, reinterpret_cast<jchar*>(utf16.data()));
  if (env->ExceptionCheck()) {
    ok = false;
    return std::nullopt;
  }
  if (utf16.find(u'\0') != std::u16string::npos) {
    throw_java(env, "java/lang/IllegalArgumentException", "string argument contains U+0000");
    ok = false;
    return std::nullopt;
  }
  return cvt_jni::utf16_to_utf8(utf16);
}

const char* c_str_or_null(const std::optional<std::string>& value) {
  return value ? value->c_str() : nullptr;
}

cvt_result* as_result(jlong handle) {
  return reinterpret_cast<cvt_result*>(static_cast<intptr_t>(handle));
}

jlong as_handle(cvt_result* result) {
  return static_cast<jlong>(reinterpret_cast<intptr_t>(result));
}

// Direct buffer view: address + length, validated against the capacity.
struct BufferView {
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool ok = false;
};

BufferView direct_buffer(JNIEnv* env, jobject buffer, jlong length) {
  BufferView view;
  if (length < 0) {
    throw_java(env, "java/lang/IllegalArgumentException", "negative length");
    return view;
  }
  if (length == 0) {
    view.ok = true;
    return view;
  }
  if (buffer == nullptr) {
    throw_java(env, "java/lang/IllegalArgumentException", "buffer is null");
    return view;
  }
  void* address = env->GetDirectBufferAddress(buffer);
  const jlong capacity = env->GetDirectBufferCapacity(buffer);
  if (address == nullptr || capacity < 0) {
    throw_java(env, "java/lang/IllegalArgumentException", "buffer must be a direct ByteBuffer");
    return view;
  }
  if (length > capacity) {
    throw_java(env, "java/lang/IndexOutOfBoundsException", "length exceeds the buffer capacity");
    return view;
  }
  view.data = static_cast<const uint8_t*>(address);
  view.size = static_cast<size_t>(length);
  view.ok = true;
  return view;
}

// ----------------------------------------------------------------------------- progress

struct ProgressContext {
  JNIEnv* env = nullptr;
  jobject sink = nullptr;
  bool stop = false;            // cancelled, or a Java exception is pending
  std::string stage;            // last stage passed to Kotlin
  jstring stage_ref = nullptr;  // local ref for `stage`, reused while unchanged
  double last_fraction = -1.0;
  std::chrono::steady_clock::time_point last_call{};

  ~ProgressContext() {
    if (stage_ref != nullptr && env != nullptr) env->DeleteLocalRef(stage_ref);
  }
};

constexpr auto kMinInterval = std::chrono::milliseconds(16);

int progress_trampoline(double fraction, const char* stage, void* user) {
  auto* ctx = static_cast<ProgressContext*>(user);
  if (ctx == nullptr || ctx->sink == nullptr) return 0;
  if (ctx->stop) return 1;
  JNIEnv* env = ctx->env;
  const std::string_view stage_view = stage != nullptr ? std::string_view(stage) : std::string_view();
  const bool stage_changed = ctx->stage_ref == nullptr || stage_view != ctx->stage;
  const auto now = std::chrono::steady_clock::now();
  const bool finished = fraction >= 1.0 && ctx->last_fraction < 1.0;
  if (!stage_changed && !finished && now - ctx->last_call < kMinInterval) return 0;

  if (stage_changed) {
    jstring next = to_jstring(env, stage_view);
    if (next == nullptr) {  // OutOfMemoryError pending
      ctx->stop = true;
      return 1;
    }
    if (ctx->stage_ref != nullptr) env->DeleteLocalRef(ctx->stage_ref);
    ctx->stage_ref = next;
    ctx->stage.assign(stage_view);
  }
  ctx->last_call = now;
  ctx->last_fraction = fraction;
  const jboolean cancel = env->CallBooleanMethod(ctx->sink, g_on_progress, static_cast<jdouble>(fraction), ctx->stage_ref);
  if (env->ExceptionCheck() || cancel == JNI_TRUE) {
    ctx->stop = true;
    return 1;
  }
  return 0;
}

// Returns the result handle, or 0 with a Java exception pending (the result is freed).
jlong finish_conversion(JNIEnv* env, cvt_result* result) {
  if (env->ExceptionCheck()) {
    cvt_result_free(result);
    return 0;
  }
  return as_handle(result);
}

// ----------------------------------------------------------------------------- natives

#define CVT_JNI_GUARD_BEGIN try {
#define CVT_JNI_GUARD_END(fallback)                                                     \
  }                                                                                     \
  catch (const std::bad_alloc&) {                                                       \
    throw_java(env, "java/lang/OutOfMemoryError", "native allocation failed");          \
    return fallback;                                                                    \
  }                                                                                     \
  catch (...) {                                                                         \
    throw_java(env, "java/lang/IllegalStateException", "unexpected native failure");    \
    return fallback;                                                                    \
  }

jstring native_version(JNIEnv* env, jclass) {
  CVT_JNI_GUARD_BEGIN
  return to_jstring(env, cvt_version());
  CVT_JNI_GUARD_END(nullptr)
}

jstring native_formats_json(JNIEnv* env, jclass) {
  CVT_JNI_GUARD_BEGIN
  return to_jstring_owned(env, cvt_formats_json());
  CVT_JNI_GUARD_END(nullptr)
}

jstring native_targets_json(JNIEnv* env, jclass, jstring source) {
  CVT_JNI_GUARD_BEGIN
  bool ok = true;
  const auto src = from_jstring(env, source, ok);
  if (!ok) return nullptr;
  return to_jstring_owned(env, cvt_targets_json(c_str_or_null(src)));
  CVT_JNI_GUARD_END(nullptr)
}

jstring native_options_schema_json(JNIEnv* env, jclass, jstring source, jstring target) {
  CVT_JNI_GUARD_BEGIN
  bool ok = true;
  const auto src = from_jstring(env, source, ok);
  const auto dst = ok ? from_jstring(env, target, ok) : std::nullopt;
  if (!ok) return nullptr;
  return to_jstring_owned(env, cvt_options_schema_json(c_str_or_null(src), c_str_or_null(dst)));
  CVT_JNI_GUARD_END(nullptr)
}

jstring native_detect_buffer(JNIEnv* env, jclass, jobject buffer, jlong length, jstring filename) {
  CVT_JNI_GUARD_BEGIN
  const BufferView view = direct_buffer(env, buffer, length);
  if (!view.ok) return nullptr;
  bool ok = true;
  const auto name = from_jstring(env, filename, ok);
  if (!ok) return nullptr;
  return to_jstring_owned(env, cvt_detect(view.data, view.size, c_str_or_null(name)));
  CVT_JNI_GUARD_END(nullptr)
}

// Detection from a file: reads the first `max_bytes` (the core needs only the
// beginning for nearly every format) without involving the Java heap.
jstring native_detect_file(JNIEnv* env, jclass, jstring path, jstring filename, jint max_bytes) {
  CVT_JNI_GUARD_BEGIN
  bool ok = true;
  const auto file_path = from_jstring(env, path, ok);
  const auto name = ok ? from_jstring(env, filename, ok) : std::nullopt;
  if (!ok) return nullptr;
  if (!file_path) {
    throw_java(env, "java/lang/IllegalArgumentException", "path is null");
    return nullptr;
  }
  const int fd = open(file_path->c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    throw_java(env, "java/io/IOException", "cannot open the file for detection");
    return nullptr;
  }
  const size_t limit = max_bytes > 0 ? static_cast<size_t>(max_bytes) : size_t{1} << 20;
  std::vector<uint8_t> head(limit);
  size_t filled = 0;
  while (filled < limit) {
    const ssize_t n = read(fd, head.data() + filled, limit - filled);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    filled += static_cast<size_t>(n);
  }
  close(fd);
  return to_jstring_owned(env, cvt_detect(head.data(), filled, c_str_or_null(name)));
  CVT_JNI_GUARD_END(nullptr)
}

jlong native_convert_buffer(JNIEnv* env, jclass, jobject buffer, jlong length, jstring filename, jstring source,
                            jstring target, jstring options, jobject sink) {
  CVT_JNI_GUARD_BEGIN
  const BufferView view = direct_buffer(env, buffer, length);
  if (!view.ok) return 0;
  bool ok = true;
  const auto name = from_jstring(env, filename, ok);
  const auto src = ok ? from_jstring(env, source, ok) : std::nullopt;
  const auto dst = ok ? from_jstring(env, target, ok) : std::nullopt;
  const auto opts = ok ? from_jstring(env, options, ok) : std::nullopt;
  if (!ok) return 0;
  ProgressContext ctx;
  ctx.env = env;
  ctx.sink = sink;
  cvt_result* result = cvt_convert(view.data, view.size, c_str_or_null(name), c_str_or_null(src), c_str_or_null(dst),
                                   c_str_or_null(opts), sink != nullptr ? progress_trampoline : nullptr, &ctx);
  return finish_conversion(env, result);
  CVT_JNI_GUARD_END(0)
}

jlong native_convert_file(JNIEnv* env, jclass, jstring input, jstring output, jstring source, jstring target,
                          jstring options, jobject sink) {
  CVT_JNI_GUARD_BEGIN
  bool ok = true;
  const auto in = from_jstring(env, input, ok);
  const auto out = ok ? from_jstring(env, output, ok) : std::nullopt;
  const auto src = ok ? from_jstring(env, source, ok) : std::nullopt;
  const auto dst = ok ? from_jstring(env, target, ok) : std::nullopt;
  const auto opts = ok ? from_jstring(env, options, ok) : std::nullopt;
  if (!ok) return 0;
  ProgressContext ctx;
  ctx.env = env;
  ctx.sink = sink;
  cvt_result* result = cvt_convert_file(c_str_or_null(in), c_str_or_null(out), c_str_or_null(src), c_str_or_null(dst),
                                        c_str_or_null(opts), sink != nullptr ? progress_trampoline : nullptr, &ctx);
  return finish_conversion(env, result);
  CVT_JNI_GUARD_END(0)
}

jint native_result_status(JNIEnv*, jclass, jlong handle) {
  return cvt_result_status(as_result(handle));
}

jstring native_result_message(JNIEnv* env, jclass, jlong handle) {
  CVT_JNI_GUARD_BEGIN
  return to_jstring(env, cvt_result_message(as_result(handle)));
  CVT_JNI_GUARD_END(nullptr)
}

jstring native_result_format(JNIEnv* env, jclass, jlong handle) {
  CVT_JNI_GUARD_BEGIN
  return to_jstring(env, cvt_result_format(as_result(handle)));
  CVT_JNI_GUARD_END(nullptr)
}

jstring native_result_extension(JNIEnv* env, jclass, jlong handle) {
  CVT_JNI_GUARD_BEGIN
  return to_jstring(env, cvt_result_extension(as_result(handle)));
  CVT_JNI_GUARD_END(nullptr)
}

jstring native_result_mime(JNIEnv* env, jclass, jlong handle) {
  CVT_JNI_GUARD_BEGIN
  return to_jstring(env, cvt_result_mime(as_result(handle)));
  CVT_JNI_GUARD_END(nullptr)
}

jstring native_result_warnings_json(JNIEnv* env, jclass, jlong handle) {
  CVT_JNI_GUARD_BEGIN
  return to_jstring(env, cvt_result_warnings_json(as_result(handle)));
  CVT_JNI_GUARD_END(nullptr)
}

jlong native_result_size(JNIEnv*, jclass, jlong handle) {
  return static_cast<jlong>(cvt_result_size(as_result(handle)));
}

// Read-only direct view of the converted bytes; valid until resultFree(handle).
jobject native_result_data(JNIEnv* env, jclass, jlong handle) {
  const cvt_result* result = as_result(handle);
  const uint8_t* data = cvt_result_data(result);
  const size_t size = cvt_result_size(result);
  if (data == nullptr || size == 0) return nullptr;
  // NewDirectByteBuffer needs a non-const pointer; the Kotlin side wraps it with asReadOnlyBuffer().
  return env->NewDirectByteBuffer(const_cast<uint8_t*>(data), static_cast<jlong>(size));
}

void native_result_free(JNIEnv*, jclass, jlong handle) {
  cvt_result_free(as_result(handle));
}

const JNINativeMethod kMethods[] = {
    {"version", "()Ljava/lang/String;", reinterpret_cast<void*>(native_version)},
    {"formatsJson", "()Ljava/lang/String;", reinterpret_cast<void*>(native_formats_json)},
    {"targetsJson", "(Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void*>(native_targets_json)},
    {"optionsSchemaJson", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
     reinterpret_cast<void*>(native_options_schema_json)},
    {"detectBuffer", "(Ljava/nio/ByteBuffer;JLjava/lang/String;)Ljava/lang/String;",
     reinterpret_cast<void*>(native_detect_buffer)},
    {"detectFile", "(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;",
     reinterpret_cast<void*>(native_detect_file)},
    {"convertBuffer",
     "(Ljava/nio/ByteBuffer;JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
     "Lapp/convertible/core/NativeProgressSink;)J",
     reinterpret_cast<void*>(native_convert_buffer)},
    {"convertFile",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
     "Lapp/convertible/core/NativeProgressSink;)J",
     reinterpret_cast<void*>(native_convert_file)},
    {"resultStatus", "(J)I", reinterpret_cast<void*>(native_result_status)},
    {"resultMessage", "(J)Ljava/lang/String;", reinterpret_cast<void*>(native_result_message)},
    {"resultFormat", "(J)Ljava/lang/String;", reinterpret_cast<void*>(native_result_format)},
    {"resultExtension", "(J)Ljava/lang/String;", reinterpret_cast<void*>(native_result_extension)},
    {"resultMime", "(J)Ljava/lang/String;", reinterpret_cast<void*>(native_result_mime)},
    {"resultWarningsJson", "(J)Ljava/lang/String;", reinterpret_cast<void*>(native_result_warnings_json)},
    {"resultSize", "(J)J", reinterpret_cast<void*>(native_result_size)},
    {"resultData", "(J)Ljava/nio/ByteBuffer;", reinterpret_cast<void*>(native_result_data)},
    {"resultFree", "(J)V", reinterpret_cast<void*>(native_result_free)},
};

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

  jclass sink = env->FindClass(kSinkClass);
  if (sink == nullptr) return JNI_ERR;
  g_on_progress = env->GetMethodID(sink, "onProgress", "(DLjava/lang/String;)Z");
  env->DeleteLocalRef(sink);
  if (g_on_progress == nullptr) return JNI_ERR;

  jclass bridge = env->FindClass(kBridgeClass);
  if (bridge == nullptr) return JNI_ERR;
  const jint rc = env->RegisterNatives(bridge, kMethods, static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0])));
  env->DeleteLocalRef(bridge);
  return rc == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
