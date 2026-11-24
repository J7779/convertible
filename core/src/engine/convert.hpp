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

/// Apply the reserved `"limits"` object of `options` (if any) on top of
/// `base`. Unknown keys and non-numeric values are InvalidArgument errors.
[[nodiscard]] Result<Limits> effective_limits(const Limits& base, const Options& options);

}  // namespace cvt
