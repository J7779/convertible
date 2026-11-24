// Convertible core - table family: legacy single-byte code pages.
//
// dBase files, BIFF5 workbooks, Lotus files and old CSV exports carry text in
// DOS and Windows code pages. The tables (upper halves only; the lower half
// is ASCII) live in codepage_tables.cpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "util/bytes.hpp"

namespace cvt::table {

struct CodePageInfo {
  int code_page = 0;                             ///< Windows code page number (437, 1252, 10000, ...)
  std::string_view name;                         ///< canonical label ("windows-1251", "cp437", ...)
  const std::array<char16_t, 128>* upper = nullptr;  ///< code points of bytes 0x80..0xFF
};

/// Lookup by number or by label (canonical names and common aliases, ASCII
/// case-insensitive); nullptr when unknown.
[[nodiscard]] const CodePageInfo* find_code_page(int code_page) noexcept;
[[nodiscard]] const CodePageInfo* find_code_page(std::string_view label) noexcept;
[[nodiscard]] std::span<const CodePageInfo> code_pages() noexcept;

/// Decode bytes of a single-byte code page into UTF-8. `replaced` counts
/// unassigned bytes (written as U+FFFD).
[[nodiscard]] std::string decode_code_page(ByteView data, const CodePageInfo& page, std::size_t* replaced = nullptr);
/// Encode UTF-8 into a single-byte code page; characters it cannot hold
/// become '?', counted in `replaced`.
[[nodiscard]] std::string encode_code_page(std::string_view utf8, const CodePageInfo& page,
                                           std::size_t* replaced = nullptr);

}  // namespace cvt::table
