// Convertible core - archive family: Zstandard building blocks shared by the decoder and encoder.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "util/bytes.hpp"

namespace cvt::compression::detail::zstd {

inline constexpr std::uint32_t frame_magic = 0xFD2FB528u;
inline constexpr std::size_t block_max = 128 * 1024;

// Literal-length and match-length code tables (RFC 8878 3.1.1.3.2.1.1).
inline constexpr std::array<std::uint32_t, 36> ll_base = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,   9,   10,  11,  12,   13,   14,   15,   16,   18,
    20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
inline constexpr std::array<std::uint8_t, 36> ll_bits = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,  1,
                                                         1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
inline constexpr std::array<std::uint32_t, 53> ml_base = {
    3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,  14,  15,  16,  17,   18,   19,   20,   21,    22,    23,    24,    25,    26,    27,    28,   29,
    30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
inline constexpr std::array<std::uint8_t, 53> ml_bits = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
                                                         2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

// Predefined distributions (RFC 8878 3.1.1.3.2.2).
inline constexpr std::array<std::int16_t, 36> ll_default = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2,
                                                            2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
inline constexpr int ll_default_log = 6;
inline constexpr std::array<std::int16_t, 53> ml_default = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                                            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                                            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
inline constexpr int ml_default_log = 6;
inline constexpr std::array<std::int16_t, 29> of_default = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1,
                                                            1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};
inline constexpr int of_default_log = 5;

/// FSE decoding table entry.
struct FseEntry {
  std::uint16_t symbol = 0;
  std::uint8_t bits = 0;
  std::uint16_t base = 0;  ///< next state = base + read(bits)
};

/// Build a decoding table from normalized counts (-1 = "less than one").
/// Returns false when the counts do not describe a valid table.
bool build_fse_table(const std::int16_t* norm, std::size_t symbols, int log, std::vector<FseEntry>& table);

/// The symbol spread shared by encoder and decoder: position of every
/// table cell's symbol (low-probability symbols at the top).
bool spread_symbols(const std::int16_t* norm, std::size_t symbols, int log, std::vector<std::uint16_t>& cells);

}  // namespace cvt::compression::detail::zstd
