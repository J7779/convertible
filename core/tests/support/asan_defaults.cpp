// AddressSanitizer defaults for the test executable.
//
// Ubuntu's shared libc++ 18 allocates some objects inside the runtime with
// operator new and releases them with free() (and the other way round), which
// ASan reports as alloc-dealloc mismatches that are not ours. When the tests
// are built with ASan against libc++ on Linux, that one check is off by
// default; ASAN_OPTIONS from the environment still override it.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <version>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CVT_TESTS_HAVE_ASAN 1
#endif
#endif
#if !defined(CVT_TESTS_HAVE_ASAN) && defined(__SANITIZE_ADDRESS__)
#define CVT_TESTS_HAVE_ASAN 1
#endif

#if defined(CVT_TESTS_HAVE_ASAN) && defined(_LIBCPP_VERSION) && defined(__linux__)
extern "C" const char* __asan_default_options();  // NOLINT: name required by the sanitizer runtime
extern "C" const char* __asan_default_options() { return "alloc_dealloc_mismatch=0"; }
#endif
