// Convertible core - format detection.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "util/bytes.hpp"

namespace cvt {

/// One candidate produced by detection.
struct Detection {
  const FormatInfo* format = nullptr;
  /// 1..100. Scale: a magic number scores 60 (1 byte) to 95 (8+ bytes); an
  /// extension match on a format without any content test scores 78; a
  /// `sniff` result is used as returned; content and extension agreeing adds
  /// 5; "looks like text" scores 30; an extension whose format's content
  /// tests all failed scores 25.
  int confidence = 0;
};

/// Lower-cased extension candidates of a file name, longest first:
/// "Backup.TAR.GZ" -> {"tar.gz", "gz"}; no extension -> {}.
[[nodiscard]] std::vector<std::string> filename_extensions(std::string_view filename);

/// All plausible formats for the data, best first (ties: more popular, then
/// by id). `filename` may be empty; `data` may be empty (extension-only).
///
/// Per format the confidence is the best of: a signature match (longer
/// patterns score higher), the format's `sniff` function, and an extension
/// match; agreement between content and extension adds a bonus. As a last
/// resort, data that looks like text is reported as "txt".
[[nodiscard]] std::vector<Detection> detect_candidates(const Registry& registry, ByteView data,
                                                       std::string_view filename);

/// The single best candidate, or nullopt when nothing fits.
[[nodiscard]] std::optional<Detection> detect_format(const Registry& registry, ByteView data,
                                                     std::string_view filename);

}  // namespace cvt
