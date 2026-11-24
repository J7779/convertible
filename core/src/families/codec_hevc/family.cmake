# codec_hevc family: imported HEVC components (see core/README.md, "Imported code").
#
# Both libraries are built from their portable C sources only (no assembly,
# no SIMD), single-threaded, and reached exclusively through the narrow C
# headers in their api/ directories. Their own headers are added to the
# library object targets only, never to the core's include path, so names
# such as iv.h, ivd.h or kvazaar.h cannot collide with other imports.

# libhevc (Ittiam, Apache-2.0): the decoder.
set(_cvt_libhevc_root "${CVT_CORE_DIR}/src/imported/libhevc")
file(GLOB _cvt_libhevc_sources RELATIVE "${_cvt_libhevc_root}"
  "${_cvt_libhevc_root}/common/*.c"
  "${_cvt_libhevc_root}/decoder/*.c"
  "${_cvt_libhevc_root}/portable/*.c")
list(SORT _cvt_libhevc_sources)
# The single-threaded ithread replacement uses the upstream names; give every
# ithread_* symbol a library-specific name so that another import with its own
# ithread.c (for example libavc) can be linked next to this one.
set(_cvt_libhevc_renames "")
foreach(_fn IN ITEMS get_handle_size get_mutex_lock_size create exit join get_mutex_struct_size
                     mutex_init mutex_destroy mutex_lock mutex_unlock yield sleep msleep usleep
                     get_sem_struct_size sem_init sem_post sem_wait sem_destroy set_affinity
                     get_cond_struct_size cond_init cond_destroy cond_wait cond_signal)
  list(APPEND _cvt_libhevc_renames "ithread_${_fn}=cvt_ihevc_ithread_${_fn}")
endforeach()
cvt_add_imported(libhevc
  SOURCES ${_cvt_libhevc_sources}
  INCLUDE_DIRS api
  PRIVATE_DEFINITIONS ENABLE_MAIN_REXT_PROFILE ${_cvt_libhevc_renames} _DEFAULT_SOURCE
  C_STANDARD 99)
target_include_directories(cvt_imported_libhevc PRIVATE
  "${_cvt_libhevc_root}/portable" "${_cvt_libhevc_root}/common" "${_cvt_libhevc_root}/decoder")

# Kvazaar (Tampere University, BSD-3-Clause): the encoder, 8-bit build.
set(_cvt_kvazaar_root "${CVT_CORE_DIR}/src/imported/kvazaar")
file(GLOB _cvt_kvazaar_sources RELATIVE "${_cvt_kvazaar_root}"
  "${_cvt_kvazaar_root}/src/*.c"
  "${_cvt_kvazaar_root}/src/strategies/*.c"
  "${_cvt_kvazaar_root}/src/strategies/generic/*.c"
  "${_cvt_kvazaar_root}/src/extras/libmd5.c"
  "${_cvt_kvazaar_root}/portable/*.c")
list(SORT _cvt_kvazaar_sources)
cvt_add_imported(kvazaar
  SOURCES ${_cvt_kvazaar_sources}
  INCLUDE_DIRS api
  # strdup() and M_PI are POSIX/X/Open, hidden by strict C99 on glibc and musl.
  PRIVATE_DEFINITIONS KVZ_C_ONLY KVZ_STATIC_LIB KVZ_EXTERNAL_STRATEGY_INIT KVZ_BIT_DEPTH=8 _GNU_SOURCE _DEFAULT_SOURCE
  C_STANDARD 99)
target_include_directories(cvt_imported_kvazaar PRIVATE
  "${_cvt_kvazaar_root}/src" "${_cvt_kvazaar_root}/portable")

# Both libraries left-shift negative values (two's complement on every
# supported compiler, but undefined in C99), and libhevc packs its deblocking
# boundary-strength maps as 32-bit words at 2-byte offsets (fine on x86,
# arm64, wasm32 and armv7 LDR/STR). Keep UBSan's other checks.
if(CVT_SANITIZE AND NOT MSVC)
  target_compile_options(cvt_imported_libhevc PRIVATE -fno-sanitize=shift-base,alignment)
  target_compile_options(cvt_imported_kvazaar PRIVATE -fno-sanitize=shift-base)
endif()
