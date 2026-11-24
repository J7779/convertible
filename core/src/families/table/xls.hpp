// Convertible core - table family: Excel 97-2003 (BIFF) workbooks (internal).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "engine/registry.hpp"
#include "util/bytes.hpp"

namespace cvt::table {

/// BIFF record ids used by the reader and the writer.
namespace biff {
inline constexpr std::uint16_t formula = 0x0006;
inline constexpr std::uint16_t eof = 0x000A;
inline constexpr std::uint16_t datemode = 0x0022;
inline constexpr std::uint16_t filepass = 0x002F;
inline constexpr std::uint16_t font = 0x0031;
inline constexpr std::uint16_t continue_record = 0x003C;
inline constexpr std::uint16_t window1 = 0x003D;
inline constexpr std::uint16_t pane = 0x0041;
inline constexpr std::uint16_t codepage = 0x0042;
inline constexpr std::uint16_t colinfo = 0x007D;
inline constexpr std::uint16_t boundsheet = 0x0085;
inline constexpr std::uint16_t mulrk = 0x00BD;
inline constexpr std::uint16_t rstring = 0x00D6;
inline constexpr std::uint16_t xf = 0x00E0;
inline constexpr std::uint16_t mergedcells = 0x00E5;
inline constexpr std::uint16_t sst = 0x00FC;
inline constexpr std::uint16_t labelsst = 0x00FD;
inline constexpr std::uint16_t extsst = 0x00FF;
inline constexpr std::uint16_t dimensions = 0x0200;
inline constexpr std::uint16_t number = 0x0203;
inline constexpr std::uint16_t label = 0x0204;
inline constexpr std::uint16_t boolerr = 0x0205;
inline constexpr std::uint16_t string = 0x0207;
inline constexpr std::uint16_t row = 0x0208;
inline constexpr std::uint16_t window2 = 0x023E;
inline constexpr std::uint16_t rk = 0x027E;
inline constexpr std::uint16_t style = 0x0293;
inline constexpr std::uint16_t format = 0x041E;
inline constexpr std::uint16_t bof = 0x0809;
}  // namespace biff

/// Decode an RK number (30-bit integer or truncated double, optionally /100).
[[nodiscard]] double decode_rk(std::uint32_t rk) noexcept;

/// Decompile BIFF8 parsed formula tokens (rgce) to A1 text; nullopt for
/// constructs that need workbook context (names, 3-D and shared references,
/// array constants) or functions outside the built-in table.
[[nodiscard]] std::optional<std::string> decompile_biff8_formula(ByteView tokens, std::uint32_t row, std::uint32_t column);

/// The BIFF8 writer, registered by register_xls.
[[nodiscard]] Result<Bytes> write_xls(const AnyIR& value, WriteContext& ctx);

}  // namespace cvt::table
