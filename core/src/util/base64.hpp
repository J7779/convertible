// Convertible core - Base64 (RFC 4648), standard and URL-safe alphabets.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::util {

struct Base64Options {
  bool url_safe = false;           ///< use '-' and '_' instead of '+' and '/'
  bool padding = true;             ///< append '=' so the length is a multiple of 4
  std::size_t line_width = 0;      ///< insert `newline` every N characters; 0 = single line
  std::string_view newline = "\n"; ///< line separator used when wrapping
};

[[nodiscard]] std::string base64_encode(ByteView data, const Base64Options& options = {});

/// Decodes either alphabet. ASCII whitespace is ignored, padding is optional,
/// and anything after the first '=' must be padding or whitespace.
/// Fails with Status::Decode on any other character or an impossible length.
[[nodiscard]] Result<Bytes> base64_decode(std::string_view text);

}  // namespace cvt::util
