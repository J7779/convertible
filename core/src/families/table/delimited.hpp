// Convertible core - table family: delimited text (CSV and relatives).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"

namespace cvt::table {

/// How a delimited file is written.
struct Dialect {
  char delimiter = ',';
  char quote = '"';        ///< 0 = fields are never quoted
  char escape = 0;         ///< e.g. '\\' for MySQL-style "\"" escapes; 0 = doubled quotes only
  bool skip_first_line = false;  ///< an Excel "sep=;" hint line precedes the data
};

/// Guess the dialect of `text` (already decoded to UTF-8) from its first
/// few kilobytes: an Excel "sep=" line wins; otherwise the candidate
/// delimiter (`preferred` first, then , tab ; | : ^ ~) that splits most lines
/// into the same number (at least two) of fields.
[[nodiscard]] Dialect sniff_dialect(std::string_view text, char preferred = ',');

/// Split `text` into records. Tolerant of real-world damage: a quote inside
/// an unquoted field is literal, text after a closing quote is appended, an
/// unterminated quoted field runs to the end of the input (and sets
/// `*unterminated`). Line breaks are LF, CRLF or CR. Calls `on_record` for
/// each record; a non-OK result stops parsing and is returned.
[[nodiscard]] Result<void> parse_delimited(std::string_view text, const Dialect& dialect,
                                           const std::function<Result<void>(std::vector<std::string>&&)>& on_record,
                                           bool* unterminated = nullptr);

}  // namespace cvt::table
