// Convertible core - PlainText IR: unstructured Unicode text.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>

namespace cvt::ir {

/// Plain text with no structure beyond lines.
///
/// This is the hub for "the same characters in another byte encoding" and
/// the natural reader kind for source code, logs and other line-oriented
/// formats. Rich formats reach it through a Document -> PlainText bridge.
struct PlainText {
  /// The content as valid UTF-8, without a byte order mark. Line endings are
  /// kept exactly as they were in the source.
  std::string text;
  /// Canonical label of the encoding the text was decoded from ("utf-8",
  /// "utf-16le", "windows-1252", ...); empty when it never had one.
  std::string source_encoding;
  /// Whether the source started with a byte order mark.
  bool had_bom = false;
};

}  // namespace cvt::ir
