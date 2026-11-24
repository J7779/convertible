/* create opj_config_private.h for CMake */
/* Pre-generated once for every target of the build: no feature probes run at
   build time. The aligned allocator uses OpenJPEG's own portable fallback,
   file offsets use plain fseek (only memory streams are ever used), and the
   byte order comes from compiler macros. */

#define OPJ_PACKAGE_VERSION "2.5.4"

/* Not used by openjp2*/
/*#cmakedefine HAVE_MEMORY_H @HAVE_MEMORY_H@*/
/*#cmakedefine HAVE_STDLIB_H @HAVE_STDLIB_H@*/
/*#cmakedefine HAVE_STRINGS_H @HAVE_STRINGS_H@*/
/*#cmakedefine HAVE_STRING_H @HAVE_STRING_H@*/
/*#cmakedefine HAVE_SYS_STAT_H @HAVE_SYS_STAT_H@*/
/*#cmakedefine HAVE_SYS_TYPES_H @HAVE_SYS_TYPES_H@ */
/*#cmakedefine HAVE_UNISTD_H @HAVE_UNISTD_H@*/
/*#cmakedefine HAVE_INTTYPES_H @HAVE_INTTYPES_H@ */
/*#cmakedefine HAVE_STDINT_H @HAVE_STDINT_H@ */

/* #undef _LARGEFILE_SOURCE */
/* #undef _LARGE_FILES */
/* #undef _FILE_OFFSET_BITS */
/* #undef OPJ_HAVE_FSEEKO */

/* find whether or not have <malloc.h> */
/* #undef OPJ_HAVE_MALLOC_H */
/* check if function `aligned_alloc` exists */
/* #undef OPJ_HAVE_ALIGNED_ALLOC */
/* check if function `_aligned_malloc` exists */
/* #undef OPJ_HAVE__ALIGNED_MALLOC */
/* check if function `memalign` exists */
/* #undef OPJ_HAVE_MEMALIGN */
/* check if function `posix_memalign` exists */
/* #undef OPJ_HAVE_POSIX_MEMALIGN */

/* Byte order. */
#if defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
# define OPJ_BIG_ENDIAN
#endif
