// Convertible core - encoding family: declarative registration helpers.
//
// Almost every format in this family is one of two shapes, and each shape is
// registered by filling in one struct:
//
//   BinaryEncoding  any file  <-> text      (Blob writer + optional Blob reader)
//                   base64, hex, uuencode, C array, checksums, ...
//   TextTransform   text      <-> text      (PlainText writer + optional reader)
//                   ROT13, HTML entities, Morse, punycode, ...
//
// Adding an encoding is therefore: write `encode` (and `decode` when the
// encoding is reversible) as plain functions over strings/bytes, then
//
//   encoding::add_binary_encoding(registry, {
//       .id = "base32", .name = "Base32 text", .description = "RFC 4648 Base32.",
//       .extensions = {"b32"}, .popularity = 30,
//       .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
//         return base32_encode(blob.bytes, ctx.options.get_bool("padding", true));
//       },
//       .decode = [](std::string_view text, const ReadContext&) { return base32_decode(text); },
//       .encode_options = {OptionSpec::boolean("padding", "Padding", true, "End with '=' padding.")},
//       .expansion = 2,
//   });
//
// The helpers take care of everything else: format and codec registration,
// reading the input as text whatever its Unicode encoding, size limits, and
// carrying the file name along.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"

namespace cvt::encoding {

/// A representation of arbitrary bytes as text.
struct BinaryEncoding {
  std::string id;           ///< canonical format id, e.g. "base32"
  std::string name;         ///< human name, e.g. "Base32 text"
  std::string description;  ///< one sentence
  /// Extensions, first = default for output files. "txt" may only be listed
  /// when `sniff` is provided (several formats share it).
  std::vector<std::string> extensions;
  std::string mime = "text/plain";
  int popularity = 20;
  /// Bytes -> text. Required.
  std::function<Result<std::string>(const ir::Blob& blob, const WriteContext& ctx)> encode;
  /// Text -> bytes. Leave null for one-way encodings. The text has already
  /// been decoded to UTF-8 (BOM removed) by the helper.
  std::function<Result<Bytes>(std::string_view text, const ReadContext& ctx)> decode;
  /// Alternative to `decode` for encodings that also carry a file name or a
  /// media type (set `filename` / `mime` on the returned blob).
  std::function<Result<ir::Blob>(std::string_view text, const ReadContext& ctx)> decode_blob;
  std::vector<OptionSpec> encode_options;
  std::vector<OptionSpec> decode_options;
  /// Upper bound on output characters per input byte (base64: 2 is plenty,
  /// hex with separators: 4, binary digits: 9). Used to refuse inputs whose
  /// encoding would exceed the output limit before any memory is spent.
  unsigned expansion = 4;
  /// Optional content test for detection (see FormatInfo::sniff).
  std::function<int(ByteView data, std::string_view filename)> sniff;
};

/// Register the format and its Blob codec.
void add_binary_encoding(Registry& registry, BinaryEncoding encoding);

/// A reversible (or one-way) rewriting of text.
struct TextTransform {
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> extensions;
  std::string mime = "text/plain";
  int popularity = 15;
  /// UTF-8 text -> transformed text (UTF-8). Required.
  std::function<Result<std::string>(std::string_view text, const WriteContext& ctx)> encode;
  /// Transformed text -> UTF-8 text. Leave null for one-way transforms.
  std::function<Result<std::string>(std::string_view text, const ReadContext& ctx)> decode;
  std::vector<OptionSpec> encode_options;
  std::vector<OptionSpec> decode_options;
  /// True when `encode` loses information (e.g. Morse drops letter case).
  bool lossy = false;
  std::function<int(ByteView data, std::string_view filename)> sniff;
};

/// Register the format and its PlainText codec.
void add_text_transform(Registry& registry, TextTransform transform);

/// The input of a decoder as UTF-8 text: a byte order mark is removed and
/// UTF-16/32 input is transcoded, so decoders only ever see UTF-8.
[[nodiscard]] std::string input_as_text(ByteView input);

}  // namespace cvt::encoding
