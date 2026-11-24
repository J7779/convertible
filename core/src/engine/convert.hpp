// Convertible core - executing conversions.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/registry.hpp"
#include "util/bytes.hpp"

namespace cvt {

/// One in-memory conversion job.
struct ConvertRequest {
  /// The complete source file. Must stay valid and unchanged for the whole call.
  ByteView input;
  /// Original file name (UTF-8, directories are ignored); may be empty.
  /// Helps detection and is passed on to codecs.
  std::string filename;
  /// Source format id or extension; empty = detect from content and name.
  std::string source_id;
  /// Target format id or extension. Required.
  std::string target_id;
  /// Caller-supplied option values. The reserved key "limits" may hold an
  /// object overriding fields of `limits`.
  Options options;
  /// Progress sink / cancellation.
  Progress progress;
  /// Safety ceilings before any override from `options`.
  Limits limits;
};

/// One file-to-file conversion job (see ConvertRequest for the shared fields).
struct FileConvertRequest {
  std::filesystem::path input;
  std::filesystem::path output;
  std::string source_id;
  std::string target_id;
  Options options;
  Progress progress;
  Limits limits;
};

/// A successful conversion.
struct ConvertOutput {
  /// The encoded target file. Empty for file-to-file conversions.
  Bytes data;
  std::string source_format_id;  ///< canonical id of the (detected) source format
  std::string format_id;         ///< canonical id of the target format
  std::string extension;         ///< default extension of the target, without dot
  std::string mime;              ///< media type of the target
  /// File name the writer gave its output (extract: the archive's one
  /// entry, "notes.txt" from "notes.txt.gz"); empty otherwise. When set,
  /// `format_id`, `extension` and `mime` describe that file, not the target.
  std::string filename;
  std::vector<std::string> warnings;
};

/// Convert in memory. Re-entrant and safe to call from any thread. Never
/// throws: every failure, including exceptions escaping from a codec, comes
/// back as an Error.
[[nodiscard]] Result<ConvertOutput> run_conversion(const Registry& registry, const ConvertRequest& request);

/// Convert a file on disk into another file. The input is memory-mapped
/// (see util::MappedFile), so large files are not copied into RAM by the
/// engine; the encoded output is accumulated in memory and written at the
/// end. The mapping is released before the output is written, so `output`
/// may name the same file as `input`. Nothing is written when the
/// conversion fails.
[[nodiscard]] Result<ConvertOutput> run_file_conversion(const Registry& registry, const FileConvertRequest& request);

/// One named file of a pack request.
struct PackInput {
  ByteView data;
  std::string name;  ///< path inside the archive (UTF-8); sanitised and made unique
};

/// Several files into one archive ("Save all as one archive").
struct PackRequest {
  std::vector<PackInput> inputs;
  std::string target_id;  ///< an archive format with an Archive writer (zip, 7z, tar.gz ...)
  Options options;        ///< the writer's options; the reserved "limits" object as for conversions
  Progress progress;
  Limits limits;
};

/// Build an archive of `target_id` from the inputs. Errors: UnknownFormat,
/// UnsupportedConversion (the format holds no several files), Limit
/// (entries, sizes), InvalidArgument (options), Encode from the writer.
[[nodiscard]] Result<ConvertOutput> run_pack(const Registry& registry, const PackRequest& request);

/// Apply the reserved `"limits"` object of `options` (if any) on top of
/// `base`. Unknown keys and non-numeric values are InvalidArgument errors.
[[nodiscard]] Result<Limits> effective_limits(const Limits& base, const Options& options);

}  // namespace cvt
