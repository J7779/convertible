// Convertible core - archive family: Brotli (RFC 7932) tables shared by the decoder and the encoder.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "util/bytes.hpp"

namespace cvt::compression::detail::brotli {

// ---- static dictionary (RFC 7932 section 8, Appendix A) ---------------------

inline constexpr std::size_t dictionary_size = 122784;
/// The dictionary words, grouped by length 4..24 (brotli_dictionary.cpp).
std::span<const std::uint8_t, dictionary_size> dictionary() noexcept;

/// log2 of the number of words of each length (0 = no words of that length).
inline constexpr std::array<std::uint8_t, 25> ndbits = {0,  0,  0,  0,  10, 10, 11, 11, 10, 10, 10, 10, 10,
                                                        9,  9,  8,  7,  7,  8,  7,  7,  6,  6,  5,  5};
/// Offset of the first word of each length.
inline constexpr std::array<std::uint32_t, 25> dictionary_offset = [] {
  std::array<std::uint32_t, 25> offsets{};
  std::uint32_t at = 0;
  for (std::size_t len = 4; len < 25; ++len) {
    offsets[len] = at;
    at += static_cast<std::uint32_t>(len) << ndbits[len];
  }
  return offsets;
}();
inline constexpr int min_dictionary_word = 4;
inline constexpr int max_dictionary_word = 24;

// ---- word transforms (RFC 7932 Appendix B) ----------------------------------

enum TransformType : std::uint8_t {
  Identity = 0,
  OmitLast1 = 1,  // .. OmitLast9 = 9
  UppercaseFirst = 10,
  UppercaseAll = 11,
  OmitFirst1 = 12,  // .. OmitFirst9 = 20
};

struct Transform {
  const char* prefix;
  std::uint8_t prefix_size;
  std::uint8_t type;
  const char* suffix;
  std::uint8_t suffix_size;
};

inline constexpr int transform_count = 121;
extern const std::array<Transform, transform_count> transforms;

/// Apply transform `id` to `word`, writing at most `max_word + 13 + 13` bytes to `out`;
/// returns the number of bytes written.
std::size_t apply_transform(std::uint8_t* out, const std::uint8_t* word, int length, int id) noexcept;

// ---- context modelling (RFC 7932 section 7.1) -------------------------------

enum ContextMode : std::uint8_t { Lsb6 = 0, Msb6 = 1, Utf8 = 2, Signed = 3 };

/// Context lookup: context = table[mode][0][p1] | table[mode][1][p2].
extern const std::array<std::array<std::array<std::uint8_t, 256>, 2>, 4> context_lookup;

[[nodiscard]] inline std::uint32_t literal_context(int mode, std::uint8_t p1, std::uint8_t p2) noexcept {
  const auto& t = context_lookup[static_cast<std::size_t>(mode)];
  return static_cast<std::uint32_t>(t[0][p1] | t[1][p2]);
}

// ---- insert-and-copy lengths (RFC 7932 section 5) ---------------------------

struct LengthCode {
  std::uint32_t base;
  std::uint8_t extra;
};

inline constexpr std::array<LengthCode, 24> insert_codes = {{{0, 0},    {1, 0},    {2, 0},    {3, 0},     {4, 0},
                                                             {5, 0},    {6, 1},    {8, 1},    {10, 2},    {14, 2},
                                                             {18, 3},   {26, 3},   {34, 4},   {50, 4},    {66, 5},
                                                             {98, 5},   {130, 6},  {194, 7},  {322, 8},   {578, 9},
                                                             {1090, 10}, {2114, 12}, {6210, 14}, {22594, 24}}};
inline constexpr std::array<LengthCode, 24> copy_codes = {{{2, 0},    {3, 0},    {4, 0},    {5, 0},     {6, 0},
                                                           {7, 0},    {8, 0},    {9, 0},    {10, 1},    {12, 1},
                                                           {14, 2},   {18, 2},   {22, 3},   {30, 3},    {38, 4},
                                                           {54, 4},   {70, 5},   {102, 5},  {134, 6},   {198, 7},
                                                           {326, 8},  {582, 9},  {1094, 10}, {2118, 24}}};

/// Per 64-symbol cell of the insert-and-copy alphabet: first insert code and first copy code.
inline constexpr std::array<std::uint8_t, 11> cell_insert = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
inline constexpr std::array<std::uint8_t, 11> cell_copy = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};

/// Block count codes (RFC 7932 section 6).
inline constexpr std::array<LengthCode, 26> block_count_codes = {
    {{1, 2},    {5, 2},    {9, 2},    {13, 2},   {17, 3},   {25, 3},   {33, 3},    {41, 3},    {49, 4},
     {65, 4},   {81, 4},   {97, 4},   {113, 5},  {145, 5},  {177, 5},  {209, 5},   {241, 6},   {305, 6},
     {369, 7},  {497, 8},  {753, 9},  {1265, 10}, {2289, 11}, {4337, 12}, {8433, 13}, {16625, 24}}};

[[nodiscard]] constexpr int insert_code_of(std::uint32_t length) noexcept {
  int code = 0;
  while (code < 23 && insert_codes[static_cast<std::size_t>(code) + 1].base <= length) ++code;
  return code;
}
[[nodiscard]] constexpr int copy_code_of(std::uint32_t length) noexcept {
  int code = 0;
  while (code < 23 && copy_codes[static_cast<std::size_t>(code) + 1].base <= length) ++code;
  return code;
}
[[nodiscard]] constexpr int block_count_code_of(std::uint32_t count) noexcept {
  int code = 0;
  while (code < 25 && block_count_codes[static_cast<std::size_t>(code) + 1].base <= count) ++code;
  return code;
}

inline constexpr std::size_t literal_alphabet = 256;
inline constexpr std::size_t command_alphabet = 704;
inline constexpr std::size_t block_count_alphabet = 26;
inline constexpr std::uint32_t max_metablock = 1u << 24;
/// Code length code order (RFC 7932 section 3.5).
inline constexpr std::array<std::uint8_t, 18> code_length_order = {1, 2, 3, 4, 0, 5, 17, 6, 16,
                                                                   7, 8, 9, 10, 11, 12, 13, 14, 15};

}  // namespace cvt::compression::detail::brotli
