// Convertible core - document family: single-byte character set tables.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace cvt::document::charsets {

/// Code points of bytes 0x80..0xFF for a lower-case label such as
/// "iso-8859-2" or an alias ("latin2", "cp1251", "koi8-r"); nullptr when unknown.
/// Latin-1 / Windows-1252 / UTF-* are handled by util/text.hpp, not here.
[[nodiscard]] const std::array<std::uint16_t, 128>* find_table(std::string_view normalized_label) noexcept;

}  // namespace cvt::document::charsets
