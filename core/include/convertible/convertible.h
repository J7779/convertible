/*
 * Convertible - offline, privacy-first universal file converter.
 *
 * This is the complete public interface of the core library and the only
 * header applications need. It is plain C99, callable from C, C++, Swift,
 * Kotlin/JNI, C#, Dart, and from JavaScript through WebAssembly.
 *
 * Conventions
 *   - All strings are NUL-terminated UTF-8. File paths are UTF-8 too, on
 *     every platform including Windows.
 *   - Every function tolerates NULL arguments and never throws or aborts.
 *   - Functions returning `char*` hand ownership to the caller: release the
 *     string with cvt_string_free(). Functions returning `const char*` return
 *     storage owned by the library or by the cvt_result they were asked about.
 *   - Every function may be called from any thread, concurrently. A single
 *     cvt_result must not be used from several threads at once.
 *   - The library is self-contained: it never touches the network, collects
 *     no telemetry, starts no other programs, and reads and writes only the
 *     buffers and paths it is given.
 *
 * Copyright (c) Convertible contributors. MIT licence; see LICENSE.
 */
#ifndef CONVERTIBLE_CONVERTIBLE_H
#define CONVERTIBLE_CONVERTIBLE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(CVT_SHARED) && defined(CVT_BUILDING_CORE)
#    define CVT_API __declspec(dllexport)
#  elif defined(CVT_SHARED)
#    define CVT_API __declspec(dllimport)
#  else
#    define CVT_API
#  endif
#elif defined(__EMSCRIPTEN__)
#  define CVT_API __attribute__((used, visibility("default")))
#elif defined(__GNUC__) || defined(__clang__)
#  define CVT_API __attribute__((visibility("default")))
#else
#  define CVT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Library                                                                    */
/* ------------------------------------------------------------------------ */

/** Library version as a static string, e.g. "0.1.0". Never NULL. */
CVT_API const char* cvt_version(void);

/** Releases any non-const `char*` returned by this library. NULL is ignored. */
CVT_API void cvt_string_free(char* s);

/* ------------------------------------------------------------------------ */
/* Catalogue                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * Every format the library knows, most popular first, as a JSON array:
 *
 *   [{"id":"png","name":"PNG image","description":"...","category":"image",
 *     "extensions":["png"],"mime":"image/png","readable":true,"writable":true,
 *     "popularity":95,"output_only":false}, ...]
 *
 *   id          canonical lower-case identifier; pass it to the other calls
 *   category    one of: image audio video document ebook spreadsheet
 *               presentation data archive font subtitle model3d geo pim code
 *               encoding vector other
 *   extensions  without dots; the first one is the default for output files
 *   readable    the format can be decoded into structured content
 *               (any file, readable or not, still converts to the universal
 *               encoding targets such as base64 or checksums)
 *   writable    the format can be produced
 *   popularity  0..100, how common the format is; the list is sorted by
 *               popularity (highest first), ties by id, so the order is stable
 *   output_only the format is only ever produced from other content (a
 *               barcode from text, minified CSS): it is never detected,
 *               never a source, and only appears in target lists
 *
 * Every entry has a non-empty id, name, category, extensions and mime; ids
 * are unique.
 *
 * Caller frees with cvt_string_free(). Returns "[]" only on allocation failure.
 */
CVT_API char* cvt_formats_json(void);

/**
 * Identifies the format of a file from its content and, optionally, its name.
 *
 * `data`/`size` may be just the beginning of the file (64 KiB is plenty for
 * most formats) but the whole file gives the best result. `filename` may be
 * NULL; only its extension is used. Returns the format id (caller frees with
 * cvt_string_free()) or NULL when the format is not recognised.
 */
CVT_API char* cvt_detect(const uint8_t* data, size_t size, const char* filename);

/**
 * The formats a source format can be converted to, most relevant first
 * (same category, then lossless, then popular), as a JSON array:
 *
 *   [{"id":"jpg","name":"JPEG image","category":"image","extensions":["jpg","jpeg"],
 *     "mime":"image/jpeg","lossy":true,"compress":false}, ...]
 *
 *   lossy      the conversion discards information
 *   compress   true for the entry whose id is the source format itself:
 *              converting a file to its own format re-encodes it (with the
 *              "quality", "preset" and related options), so applications can
 *              offer it as a "Compress" action
 *   role       what the target does, for grouping the list:
 *                "compress"  the source format itself (same as compress:true)
 *                "extract"   the file inside a compressed file or one-entry
 *                            archive, under its own name and format (see
 *                            cvt_result_filename); id "extract"
 *                "archive"   the file packed into an archive or compressed
 *                            (zip, 7z, tar.gz, gz ...)
 *                "encode"    a byte-level rendering of any file (base64, hex,
 *                            checksums ...); always listed last
 *                "convert"   any other conversion
 *              Unknown values may be added later; treat them as "convert".
 *
 * Order: the source's own category first (most popular first, the compress
 * entry included), then real conversions to other categories (fewer
 * intermediate steps first, faithful before lossy, then popularity), then the
 * archive and byte-level targets. Output-only formats (barcodes, minified
 * code; "output_only" in cvt_formats_json) appear only here, never as sources.
 *
 * `source_format_id` accepts a format id or a file extension. Returns "[]"
 * for NULL or unknown ids. Caller frees with cvt_string_free().
 */
CVT_API char* cvt_targets_json(const char* source_format_id);

/**
 * The options a particular conversion understands, as a JSON array ready to
 * be rendered as a form:
 *
 *   [{"key":"quality","label":"Quality","type":"int","default":85,"min":1,
 *     "max":100,"choices":[],"description":"Higher is better and larger.",
 *     "category":"basic"}, ...]
 *
 *   type      "int" | "float" | "bool" | "enum" | "string"
 *   min/max   inclusive bounds for int/float, otherwise null
 *   choices   allowed values for "enum", otherwise empty
 *   category  "basic" (show by default) or "advanced" (show on request)
 *
 * The same option keys mean the same thing for every format: "quality"
 * (1-100, higher is better and larger), "preset" ("high" | "balanced" |
 * "small"), "compression_level", "lossless", "max_width", "max_height",
 * "scale", "strip_metadata", "sample_rate", "channels", "bitrate_kbps",
 * "fps", "width", "height", "video_bitrate_kbps", "audio_bitrate_kbps".
 *
 * Pass the chosen values to cvt_convert() as a flat JSON object,
 * e.g. {"quality":70}. Returns "[]" when the conversion has no options or is
 * not possible. Caller frees with cvt_string_free().
 */
CVT_API char* cvt_options_schema_json(const char* source_format_id, const char* target_format_id);

/* ------------------------------------------------------------------------ */
/* Conversion                                                                 */
/* ------------------------------------------------------------------------ */

/**
 * Progress callback. `fraction` runs from 0 to 1 and never decreases;
 * `stage` is a short English label such as "Reading" (never NULL, valid only
 * during the call); `user` is the pointer given to the convert call.
 * Return nonzero to cancel the conversion. Called on the thread that called
 * the convert function.
 */
typedef int (*cvt_progress_fn)(double fraction, const char* stage, void* user);

/** Outcome of a conversion. Opaque; query it with the cvt_result_* functions. */
typedef struct cvt_result cvt_result;

/** Status codes reported by cvt_result_status(). */
enum cvt_status {
  CVT_OK = 0,
  CVT_ERR_INVALID_ARGUMENT = 1,       /**< NULL/invalid argument or option value */
  CVT_ERR_UNKNOWN_FORMAT = 2,         /**< unknown format id, or detection failed */
  CVT_ERR_UNSUPPORTED_CONVERSION = 3, /**< no route from the source to the target */
  CVT_ERR_DECODE = 4,                 /**< the input is damaged or uses unsupported features */
  CVT_ERR_ENCODE = 5,                 /**< the content cannot be written in the target format */
  CVT_ERR_IO = 6,                     /**< a file could not be read or written */
  CVT_ERR_CANCELLED = 7,              /**< the progress callback asked to stop */
  CVT_ERR_LIMIT = 8,                  /**< a safety limit (size, pixels, memory...) was exceeded */
  CVT_ERR_INTERNAL = 9                /**< unexpected failure inside the library */
};

/**
 * Converts a file held in memory.
 *
 *   data, size        the complete source file (data may be NULL when size is 0)
 *   filename          original file name, used for detection and passed to
 *                     encoders that embed names; may be NULL
 *   source_format_id  format id or extension of the input; NULL or "" = detect
 *   target_format_id  format id or extension to produce; required
 *   options_json      JSON object of option values (see
 *                     cvt_options_schema_json); NULL or "" = defaults.
 *                     The reserved key "limits" may hold an object that
 *                     overrides safety limits for this call, with any of:
 *                     max_input_bytes, max_output_bytes, max_pixels,
 *                     max_archive_entries, max_decompressed_bytes, max_depth.
 *   progress, user    optional progress/cancel callback and its context
 *
 * Never returns NULL: inspect the result with cvt_result_status() and
 * release it with cvt_result_free(). Blocks until done; run it on a worker
 * thread in interactive applications.
 */
CVT_API cvt_result* cvt_convert(const uint8_t* data, size_t size, const char* filename,
                                const char* source_format_id, const char* target_format_id,
                                const char* options_json, cvt_progress_fn progress, void* user);

/**
 * Converts a file on disk and writes the result to `output_path` (created or
 * replaced; its directory must exist). Other arguments as for cvt_convert().
 * The input is memory-mapped rather than read into memory, so this is the
 * call to use for large files; the input file must not be modified while the
 * conversion runs.
 * On success the result carries no data (cvt_result_size() is 0) but the
 * format, extension, MIME type and warnings are filled in. On failure no
 * output file is left behind. Never returns NULL.
 */
CVT_API cvt_result* cvt_convert_file(const char* input_path, const char* output_path,
                                     const char* source_format_id, const char* target_format_id,
                                     const char* options_json, cvt_progress_fn progress, void* user);

/** One named file for cvt_pack(). */
typedef struct cvt_pack_input {
  const uint8_t* data; /**< the file's bytes (may be NULL when size is 0) */
  size_t size;
  const char* name;    /**< UTF-8 path inside the archive ("photos/a.jpg"); sanitised, made unique */
} cvt_pack_input;

/**
 * Packs several files into one archive ("Save all as one archive").
 *
 *   inputs, count     the files; names are sanitised (no absolute paths, no
 *                     ".."), duplicates get " (2)", " (3)" ... before the
 *                     extension
 *   target_format_id  an archive format that can hold several files: "zip",
 *                     "7z", "tar", "tar.gz", "tar.xz", "tar.zst", ... (the
 *                     targets of role "archive" except single-stream
 *                     compressors such as "gz")
 *   options_json      the archive writer's options (see
 *                     cvt_options_schema_json("zip", "zip")), e.g.
 *                     {"compression_level":9} or a password for zip/7z
 *   progress, user    as for cvt_convert()
 *
 * Never returns NULL; inspect and free the result as for cvt_convert().
 */
CVT_API cvt_result* cvt_pack(const cvt_pack_input* inputs, size_t count, const char* target_format_id,
                             const char* options_json, cvt_progress_fn progress, void* user);

/** CVT_OK or one of the CVT_ERR_* codes. CVT_ERR_INVALID_ARGUMENT for NULL. */
CVT_API int cvt_result_status(const cvt_result* result);
/** Human-readable error description; "" on success. Never NULL. */
CVT_API const char* cvt_result_message(const cvt_result* result);
/** The converted bytes; NULL when there are none. Valid until cvt_result_free(). */
CVT_API const uint8_t* cvt_result_data(const cvt_result* result);
/** Number of bytes at cvt_result_data(). */
CVT_API size_t cvt_result_size(const cvt_result* result);
/** Id of the produced format; "" on failure. Never NULL. */
CVT_API const char* cvt_result_format(const cvt_result* result);
/** Recommended file extension for the output, without the dot; "" on failure. Never NULL. */
CVT_API const char* cvt_result_extension(const cvt_result* result);
/** MIME type of the output; "" on failure. Never NULL. */
CVT_API const char* cvt_result_mime(const cvt_result* result);
/**
 * File name the conversion gave its output, without directories, or "" when
 * it has none of its own (the usual case: name the output after the input
 * with cvt_result_extension()). Set by the "extract" target, which turns a
 * compressed file or a one-entry archive back into its content
 * ("notes.txt.gz" -> "notes.txt"); cvt_result_format(), _extension() and
 * _mime() then describe that file. Never NULL.
 */
CVT_API const char* cvt_result_filename(const cvt_result* result);
/**
 * Non-fatal notes about the conversion as a JSON array of strings, e.g.
 * ["2 characters could not be represented"]. "[]" when there are none. Never NULL.
 */
CVT_API const char* cvt_result_warnings_json(const cvt_result* result);
/** Releases a result and its data. NULL is ignored. */
CVT_API void cvt_result_free(cvt_result* result);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CONVERTIBLE_CONVERTIBLE_H */
