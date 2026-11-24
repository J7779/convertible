/* Build configuration of libwebp inside Convertible (not an upstream file).
 *
 * Every target (Clang, GCC, MSVC, Android NDK incl. armeabi-v7a, Emscripten)
 * uses this one file. HAVE_CONFIG_H is defined for the library, which makes
 * src/dsp/cpu.h enable a SIMD path only when the matching WEBP_HAVE_* macro is
 * set here. None is set, so only the plain C implementations are compiled and
 * selected. No threads (WEBP_USE_THREAD is not defined). See
 * CONVERTIBLE_CHANGES.md. */
#ifndef WEBP_WEBP_CONFIG_H_
#define WEBP_WEBP_CONFIG_H_

/* Byte-swap builtins, detected the same way upstream does without a config
 * header; src/utils/endian_inl_utils.h has portable fallbacks. */
#if defined(__has_builtin)
#if __has_builtin(__builtin_bswap16)
#define HAVE_BUILTIN_BSWAP16 1
#endif
#if __has_builtin(__builtin_bswap32)
#define HAVE_BUILTIN_BSWAP32 1
#endif
#if __has_builtin(__builtin_bswap64)
#define HAVE_BUILTIN_BSWAP64 1
#endif
#elif defined(__GNUC__) && !defined(__clang__) && \
    ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
#define HAVE_BUILTIN_BSWAP16 1
#define HAVE_BUILTIN_BSWAP32 1
#define HAVE_BUILTIN_BSWAP64 1
#endif

/* Big-endian hosts (none of the supported targets, kept for completeness). */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define WORDS_BIGENDIAN 1
#endif

#endif /* WEBP_WEBP_CONFIG_H_ */
