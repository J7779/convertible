// Convertible desktop - UTF-8 helpers and human-readable formatting.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cvd {

/// Decodes one code point starting at `pos` and advances `pos`. Invalid bytes yield U+FFFD.
std::uint32_t utf8_next(std::string_view text, std::size_t& pos);
/// Byte offset of the code point before `pos` (0 at the start).
std::size_t utf8_prev(std::string_view text, std::size_t pos);
/// Appends the UTF-8 encoding of `cp`.
void utf8_append(std::string& out, std::uint32_t cp);
/// Decodes the whole string.
std::vector<std::uint32_t> utf8_decode(std::string_view text);
/// Number of code points.
std::size_t utf8_length(std::string_view text);

/// ASCII lower-casing (format ids and extensions are ASCII).
std::string ascii_lower(std::string_view text);
std::string ascii_upper(std::string_view text);
/// Case-insensitive ASCII substring search.
bool contains_ci(std::string_view haystack, std::string_view needle);
bool starts_with_ci(std::string_view text, std::string_view prefix);

/// "0 bytes", "1 byte", "999 bytes", "1.2 KB", "34 KB", "5.6 MB", "1.2 GB" (decimal units, like the OS file managers).
std::string format_size(std::uint64_t bytes);
/// Saved percentage between before and after, e.g. "−42%" (smaller) or "+8%" (larger); "0%" when equal.
std::string format_size_change(std::uint64_t before, std::uint64_t after);
/// Integer percentage 0..100 of a fraction.
std::string format_percent(double fraction);

/// Splits on a single character.
std::vector<std::string> split(std::string_view text, char sep);
/// Trims ASCII whitespace.
std::string_view trim(std::string_view text);

/// File name part of a path (accepts '/' and '\\').
std::string path_filename(std::string_view path);
/// Name without the last extension ("a.tar.gz" -> "a.tar").
std::string path_stem(std::string_view filename);
/// Last extension without dot, lower-case.
std::string path_extension(std::string_view filename);
/// Directory part of a path without the trailing separator.
std::string path_parent(std::string_view path);

}  // namespace cvd
