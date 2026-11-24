// Convertible core - encoding family: further binary-to-text encodings
// (more_bases.cpp) - pure functions, unit-tested directly.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt::encoding {

/// RFC 4648 Base16: upper-case digits, no separators. Decoding accepts
/// either case and ignores ASCII whitespace.
[[nodiscard]] std::string base16_encode(ByteView data);
[[nodiscard]] Result<Bytes> base16_decode(std::string_view text);

/// RFC 9285 Base45 (as used by QR codes).
[[nodiscard]] std::string base45_encode(ByteView data);
[[nodiscard]] Result<Bytes> base45_decode(std::string_view text);

/// basE91 (Joachim Henke). Decoding ignores characters outside the alphabet.
[[nodiscard]] std::string base91_encode(ByteView data);
[[nodiscard]] Bytes base91_decode(std::string_view text);

/// Base92 (thenoviceoof): 13 bits per two characters, "~" for no data.
[[nodiscard]] std::string base92_encode(ByteView data);
[[nodiscard]] Result<Bytes> base92_decode(std::string_view text);

/// Base122 (Kevin Albertson): 7 bits per UTF-8 character; the six characters
/// that are awkward in HTML/JS strings become two-byte sequences.
[[nodiscard]] std::string base122_encode(ByteView data);
[[nodiscard]] Result<Bytes> base122_decode(std::string_view text);

/// Bech32 (BIP 173) or Bech32m (BIP 350) with a human-readable part. The data
/// part carries the bytes regrouped into 5-bit values, padded with zeros.
enum class Bech32Variant { Bech32, Bech32m };
[[nodiscard]] Result<std::string> bech32_encode(std::string_view hrp, ByteView data, Bech32Variant variant);
struct Bech32Decoded {
  std::string hrp;
  Bytes data;
  Bech32Variant variant = Bech32Variant::Bech32;
};
[[nodiscard]] Result<Bech32Decoded> bech32_decode(std::string_view text);

/// Base58Check: Base58 of data + the first 4 bytes of SHA-256(SHA-256(data)).
[[nodiscard]] Result<std::string> base58check_encode(ByteView data);
[[nodiscard]] Result<Bytes> base58check_decode(std::string_view text);

/// Base64 armour: "-----BEGIN <label>-----", 64-column Base64, "-----END <label>-----".
[[nodiscard]] std::string pem_encode(ByteView data, std::string_view label);
struct PemDecoded {
  std::string label;
  Bytes data;
};
/// The first armoured block; text outside it is ignored. Headers ("Key: value"
/// lines before a blank line, RFC 1421 style) are skipped.
[[nodiscard]] Result<PemDecoded> pem_decode(std::string_view text);

/// xxencode: uuencode's line format with the alphabet +-0-9A-Za-z.
[[nodiscard]] std::string xxencode(ByteView data, std::string_view filename);
struct XxDecoded {
  Bytes bytes;
  std::string filename;
};
[[nodiscard]] Result<XxDecoded> xxdecode(std::string_view text);

/// BinHex 4.0 (.hqx): header, data fork and an empty resource fork, each with
/// its CRC, run-length compressed and written in 64-column lines.
[[nodiscard]] std::string binhex_encode(ByteView data, std::string_view filename);
struct BinHexDecoded {
  Bytes data;
  Bytes resource;
  std::string filename;
  std::string type;     ///< four-character file type
  std::string creator;  ///< four-character creator code
};
/// Verifies all three CRCs.
[[nodiscard]] Result<BinHexDecoded> binhex_decode(std::string_view text, std::size_t max_output);

/// RFC 2047 encoded words ("=?UTF-8?B?...?="), folded so that no word
/// exceeds 75 characters; `q_encoding` selects the Q form.
[[nodiscard]] std::string mime_words_encode(std::string_view text, bool q_encoding);
/// Decodes every encoded word (UTF-8, US-ASCII, ISO-8859-1, Windows-1252,
/// UTF-16/32 charsets) and drops the whitespace between adjacent ones.
[[nodiscard]] Result<std::string> mime_words_decode(std::string_view text);

}  // namespace cvt::encoding
