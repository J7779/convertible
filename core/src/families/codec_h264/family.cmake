# codec_h264 family: the libavc H.264 decoder and encoder (Apache-2.0),
# compiled from its portable C sources only: no assembly, no SIMD, no threads.
# The `generic/` directories hold the C-only platform glue (see
# core/src/imported/libavc/CONVERTIBLE_CHANGES.md).

set(_cvt_avc_root "${CVT_CORE_DIR}/src/imported/libavc")
file(GLOB _cvt_avc_sources RELATIVE "${_cvt_avc_root}"
  "${_cvt_avc_root}/common/*.c"
  "${_cvt_avc_root}/decoder/*.c"
  "${_cvt_avc_root}/decoder/generic/*.c"
  "${_cvt_avc_root}/encoder/*.c"
  "${_cvt_avc_root}/encoder/generic/*.c")
list(SORT _cvt_avc_sources)

# The single-threaded ithread shim gets a private prefix so that it can never
# clash with another imported Ittiam codec (libhevc, libmpeg2) that ships its
# own ithread_* functions.
set(_cvt_avc_renames "")
foreach(fn IN ITEMS
    ithread_get_handle_size ithread_get_mutex_lock_size ithread_create ithread_join
    ithread_get_mutex_struct_size ithread_mutex_init ithread_mutex_destroy ithread_mutex_lock
    ithread_mutex_unlock ithread_yield ithread_set_name ithread_sleep ithread_msleep ithread_usleep
    ithread_get_sem_struct_size ithread_sem_init ithread_sem_post ithread_sem_wait
    ithread_sem_destroy ithread_set_affinity ithread_get_cond_struct_size ithread_cond_init
    ithread_cond_destroy ithread_cond_wait ithread_cond_signal ithread_get_cond_size
    ithread_cond_broadcast)
  list(APPEND _cvt_avc_renames "${fn}=cvt_avc_${fn}")
endforeach()

cvt_add_imported(libavc
  SOURCES ${_cvt_avc_sources}
  INCLUDE_DIRS common common/generic decoder decoder/generic encoder encoder/generic
  PRIVATE_DEFINITIONS ${_cvt_avc_renames} NDEBUG _DEFAULT_SOURCE)  # strnlen etc. on glibc with -std=c99/c11
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_LIBAVC=1)

# Sanitizer builds: libavc offsets null pointers of planes it never touches
# (for example the third chroma plane of its interleaved internal format) and
# left-shifts negative coefficients (two's complement scaling, defined in
# C++20 and by every supported compiler), and its encoder places 4-byte
# aligned bookkeeping structs in its memory records (fine on every target; on
# 32-bit ones they need no more). UBSan's pointer-overflow, shift-base and
# alignment checks are therefore off for this one library; ASan and every
# other UBSan check stay on.
if(CVT_SANITIZE AND NOT MSVC)
  target_compile_options(cvt_imported_libavc PRIVATE -fno-sanitize=pointer-overflow,shift-base,alignment)
endif()
