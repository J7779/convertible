// Convertible core - Blob IR: opaque bytes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>

#include "util/bytes.hpp"

namespace cvt::ir {

/// A file treated as an uninterpreted sequence of bytes.
///
/// Every source format is implicitly readable as a Blob (the engine supplies
/// that reader, at high cost), so targets that accept a Blob - Base64, hex
/// dumps, checksums, "wrap into an archive" bridges - exist for any input.
/// A format may also register an explicit Blob reader that yields its
/// *decoded* payload (for example `base64` -> the original bytes).
struct Blob {
  /// The content.
  Bytes bytes;
  /// File name the bytes came from, without directories; may be empty.
  std::string filename;
  /// Format id of the source file when known (e.g. "png"); may be empty.
  /// Filled in by the engine's implicit reader.
  std::string format_id;
  /// MIME type of the source file when known; may be empty.
  std::string mime;
};

}  // namespace cvt::ir
