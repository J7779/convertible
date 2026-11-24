// Convertible core - archive family: Brotli word transforms and context lookup tables (RFC 7932).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <string>

#include "families/archive/codecs/brotli_internal.hpp"

namespace cvt::compression::detail::brotli {

namespace {

constexpr Transform t(const char* prefix, std::uint8_t type, const char* suffix) {
  return Transform{prefix, static_cast<std::uint8_t>(std::char_traits<char>::length(prefix)), type, suffix,
                   static_cast<std::uint8_t>(std::char_traits<char>::length(suffix))};
}

constexpr std::uint8_t omit_last(int n) { return static_cast<std::uint8_t>(n); }
constexpr std::uint8_t omit_first(int n) { return static_cast<std::uint8_t>(OmitFirst1 + n - 1); }
constexpr std::uint8_t UF = UppercaseFirst;
constexpr std::uint8_t UA = UppercaseAll;
constexpr std::uint8_t ID = Identity;

// Lut0 / Lut1 of the UTF8 context mode for bytes 0..127 (RFC 7932 section 7.1).
constexpr std::array<std::uint8_t, 128> utf8_lut0 = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  4,  4,  0,  0,  4,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  8,  12, 16, 12, 12, 20, 12, 16, 24, 28, 12, 12, 32, 12, 36, 12, 44, 44, 44, 44,
    44, 44, 44, 44, 44, 44, 32, 32, 24, 40, 28, 12, 12, 48, 52, 52, 52, 48, 52, 52, 52, 48, 52, 52, 52, 52,
    52, 48, 52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 24, 12, 28, 12, 12, 12, 56, 60, 60, 60, 56, 60, 60,
    60, 56, 60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 24, 12, 28, 12, 0};
constexpr std::array<std::uint8_t, 128> utf8_lut1 = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
    1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
    1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 0};

constexpr std::uint8_t signed_class(int b) {
  if (b == 0) return 0;
  if (b < 16) return 1;
  if (b < 64) return 2;
  if (b < 128) return 3;
  if (b < 192) return 4;
  if (b < 240) return 5;
  if (b < 255) return 6;
  return 7;
}

// Uppercase one UTF-8 character in place (RFC 7932 Appendix B); returns its length.
int ferment(std::uint8_t* p, int left) noexcept {
  if (p[0] < 0xC0) {
    if (p[0] >= 'a' && p[0] <= 'z') p[0] ^= 32u;
    return 1;
  }
  if (p[0] < 0xE0) {
    if (left > 1) p[1] ^= 32u;
    return 2;
  }
  if (left > 2) p[2] ^= 5u;
  return 3;
}

}  // namespace

const std::array<std::array<std::array<std::uint8_t, 256>, 2>, 4> context_lookup = [] {
  std::array<std::array<std::array<std::uint8_t, 256>, 2>, 4> lut{};
  for (int b = 0; b < 256; ++b) {
    const auto i = static_cast<std::size_t>(b);
    lut[Lsb6][0][i] = static_cast<std::uint8_t>(b & 0x3F);
    lut[Msb6][0][i] = static_cast<std::uint8_t>(b >> 2);
    if (b < 128) {
      lut[Utf8][0][i] = utf8_lut0[i];
      lut[Utf8][1][i] = utf8_lut1[i];
    } else {
      lut[Utf8][0][i] = static_cast<std::uint8_t>(b < 192 ? (b & 1) : 2 + (b & 1));
      lut[Utf8][1][i] = static_cast<std::uint8_t>(b < 224 ? 0 : 2);
    }
    lut[Signed][0][i] = static_cast<std::uint8_t>(signed_class(b) << 3);
    lut[Signed][1][i] = signed_class(b);
  }
  return lut;
}();

const std::array<Transform, transform_count> transforms = {{
    t("", ID, ""),          t("", ID, " "),           t(" ", ID, " "),         t("", omit_first(1), ""),
    t("", UF, " "),         t("", ID, " the "),       t(" ", ID, ""),          t("s ", ID, " "),
    t("", ID, " of "),      t("", UF, ""),            t("", ID, " and "),      t("", omit_first(2), ""),
    t("", omit_last(1), ""), t(", ", ID, " "),        t("", ID, ", "),         t(" ", UF, " "),
    t("", ID, " in "),      t("", ID, " to "),        t("e ", ID, " "),        t("", ID, "\""),
    t("", ID, "."),         t("", ID, "\">"),         t("", ID, "\n"),         t("", omit_last(3), ""),
    t("", ID, "]"),         t("", ID, " for "),       t("", omit_first(3), ""), t("", omit_last(2), ""),
    t("", ID, " a "),       t("", ID, " that "),      t(" ", UF, ""),          t("", ID, ". "),
    t(".", ID, ""),         t(" ", ID, ", "),         t("", omit_first(4), ""), t("", ID, " with "),
    t("", ID, "'"),         t("", ID, " from "),      t("", ID, " by "),       t("", omit_first(5), ""),
    t("", omit_first(6), ""), t(" the ", ID, ""),     t("", omit_last(4), ""), t("", ID, ". The "),
    t("", UA, ""),          t("", ID, " on "),        t("", ID, " as "),       t("", ID, " is "),
    t("", omit_last(7), ""), t("", omit_last(1), "ing "), t("", ID, "\n\t"),   t("", ID, ":"),
    t(" ", ID, ". "),       t("", ID, "ed "),         t("", omit_first(9), ""), t("", omit_first(7), ""),
    t("", omit_last(6), ""), t("", ID, "("),          t("", UF, ", "),         t("", omit_last(8), ""),
    t("", ID, " at "),      t("", ID, "ly "),         t(" the ", ID, " of "),  t("", omit_last(5), ""),
    t("", omit_last(9), ""), t(" ", UF, ", "),        t("", UF, "\""),         t(".", ID, "("),
    t("", UA, " "),         t("", UF, "\">"),         t("", ID, "=\""),        t(" ", ID, "."),
    t(".com/", ID, ""),     t(" the ", ID, " of the "), t("", UF, "'"),        t("", ID, ". This "),
    t("", ID, ","),         t(".", ID, " "),          t("", UF, "("),          t("", UF, "."),
    t("", ID, " not "),     t(" ", ID, "=\""),        t("", ID, "er "),        t(" ", UA, " "),
    t("", ID, "al "),       t(" ", UA, ""),           t("", ID, "='"),         t("", UA, "\""),
    t("", UF, ". "),        t(" ", ID, "("),          t("", ID, "ful "),       t(" ", UF, ". "),
    t("", ID, "ive "),      t("", ID, "less "),       t("", UA, "'"),          t("", ID, "est "),
    t(" ", UF, "."),        t("", UA, "\">"),         t(" ", ID, "='"),        t("", UF, ","),
    t("", ID, "ize "),      t("", UA, "."),           t("\xC2\xA0", ID, ""),   t(" ", ID, ","),
    t("", UF, "=\""),       t("", UA, "=\""),         t("", ID, "ous "),       t("", UA, ", "),
    t("", UF, "='"),        t(" ", UF, ","),          t(" ", UA, "=\""),       t(" ", UA, ", "),
    t("", UA, ","),         t("", UA, "("),           t("", UA, ". "),         t(" ", UA, "."),
    t("", UA, "='"),        t(" ", UA, ". "),         t(" ", UF, "=\""),       t(" ", UA, "='"),
    t(" ", UF, "='"),
}};

std::size_t apply_transform(std::uint8_t* out, const std::uint8_t* word, int length, int id) noexcept {
  const Transform& tr = transforms[static_cast<std::size_t>(id)];
  std::size_t n = 0;
  std::memcpy(out, tr.prefix, tr.prefix_size);
  n += tr.prefix_size;
  int skip = 0;
  int keep = length;
  if (tr.type >= OmitFirst1) {
    skip = std::min(length, tr.type - OmitFirst1 + 1);
    keep = length - skip;
  } else if (tr.type >= OmitLast1 && tr.type <= OmitLast1 + 8) {
    keep = std::max(0, length - tr.type);
  }
  if (keep > 0) std::memcpy(out + n, word + skip, static_cast<std::size_t>(keep));
  if (tr.type == UppercaseFirst && keep > 0) {
    (void)ferment(out + n, keep);
  } else if (tr.type == UppercaseAll) {
    int at = 0;
    while (at < keep) at += ferment(out + n + static_cast<std::size_t>(at), keep - at);
  }
  n += static_cast<std::size_t>(keep);
  std::memcpy(out + n, tr.suffix, tr.suffix_size);
  return n + tr.suffix_size;
}

}  // namespace cvt::compression::detail::brotli
