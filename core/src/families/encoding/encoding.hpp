// Convertible core - encoding family: internal interface.
//
// The pure transformation functions live here so that they can be unit
// tested directly; `encoding_family.cpp` wraps them into codecs.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/errors.hpp"
#include "util/bytes.hpp"

namespace cvt {
class Registry;
}  // namespace cvt

namespace cvt::encoding {

// -- hex --------------------------------------------------------------------

struct HexStyle {
  bool uppercase = false;
  std::size_t bytes_per_line = 0;  ///< 0 = everything on one line
  std::string_view separator;      ///< between bytes on a line, e.g. " " or ":"
};
[[nodiscard]] std::string hex_text(ByteView data, const HexStyle& style);
/// Accepts what `hex_text` produces plus "0x" prefixes and , : - separators.
[[nodiscard]] Result<Bytes> parse_hex_text(std::string_view text);

// -- xxd-style hex dump -------------------------------------------------------

/// "00000000: 4865 6c6c 6f0a                           Hello."
[[nodiscard]] std::string hexdump_text(ByteView data, std::size_t bytes_per_line, bool uppercase);
/// Reverses `hexdump_text` (and `xxd` output). Offsets are not interpreted:
/// lines are taken to be contiguous.
[[nodiscard]] Result<Bytes> parse_hexdump(std::string_view text);

// -- data: URI ------------------------------------------------------------------

[[nodiscard]] std::string data_uri(ByteView data, std::string_view mime, bool base64);
struct ParsedDataUri {
  Bytes bytes;
  std::string mime;  ///< "text/plain" when the URI has none, per RFC 2397
};
[[nodiscard]] Result<ParsedDataUri> parse_data_uri(std::string_view text);

// -- C source ---------------------------------------------------------------------

/// A valid C identifier derived from arbitrary text ("my photo.png" ->
/// "my_photo_png"); "data" when nothing usable remains.
[[nodiscard]] std::string c_identifier(std::string_view text);
/// `unsigned char name[] = {...}; unsigned int name_len = N;`
[[nodiscard]] std::string c_header(ByteView data, std::string_view variable_name, std::size_t bytes_per_line,
                                   std::string_view comment);

// -- uuencode -----------------------------------------------------------------------

[[nodiscard]] std::string uuencode(ByteView data, std::string_view filename);
struct ParsedUu {
  Bytes bytes;
  std::string filename;
};
[[nodiscard]] Result<ParsedUu> uudecode(std::string_view text);

// -- percent-encoding -----------------------------------------------------------------

/// Every byte except A-Z a-z 0-9 - . _ ~ becomes %XX.
[[nodiscard]] std::string percent_encode(ByteView data, bool space_as_plus);
[[nodiscard]] Result<Bytes> percent_decode(std::string_view text, bool plus_as_space);

// -- binary digits ----------------------------------------------------------------------

/// "01001000 01101001": eight digits per byte.
[[nodiscard]] std::string binary_text(ByteView data, bool separate_bytes, std::size_t bytes_per_line);
[[nodiscard]] Result<Bytes> parse_binary_text(std::string_view text);

// -- checksums ------------------------------------------------------------------------------

/// A small text report with the size, CRC-32, Adler-32, MD5, SHA-1 and SHA-256.
[[nodiscard]] std::string checksum_report(ByteView data, std::string_view filename, bool uppercase);

// -- Base32 (base_n.cpp) --------------------------------------------------------------------

enum class Base32Alphabet {
  Standard,   ///< RFC 4648 section 6: A-Z 2-7
  Hex,        ///< RFC 4648 section 7 ("base32hex"): 0-9 A-V
  Crockford,  ///< Douglas Crockford's alphabet: 0-9 A-Z without I L O U; never padded
};
struct Base32Style {
  Base32Alphabet alphabet = Base32Alphabet::Standard;
  bool padding = true;         ///< end with '=' so the length is a multiple of 8 (ignored for Crockford)
  std::size_t line_width = 0;  ///< start a new line every N characters; 0 = single line
};
[[nodiscard]] std::string base32_encode(ByteView data, const Base32Style& style = {});
/// Case-insensitive; ASCII whitespace is ignored and padding is optional.
/// The Crockford alphabet also reads O as 0 and I/L as 1 and ignores hyphens.
[[nodiscard]] Result<Bytes> base32_decode(std::string_view text, Base32Alphabet alphabet = Base32Alphabet::Standard);

// -- whole-file big-number bases (base_n.cpp) -------------------------------------------------

/// Base58 and Base36 treat the input as one big integer, which costs
/// quadratic time; both directions refuse more than this many bytes of data
/// with Status::Limit.
inline constexpr std::size_t big_number_max_bytes = 64 * 1024;

/// Bitcoin alphabet; every leading zero byte becomes a leading '1'.
[[nodiscard]] Result<std::string> base58_encode(ByteView data);
[[nodiscard]] Result<Bytes> base58_decode(std::string_view text);
/// Digits 0-9 a-z; every leading zero byte becomes a leading '0'.
[[nodiscard]] Result<std::string> base36_encode(ByteView data, bool uppercase = false);
[[nodiscard]] Result<Bytes> base36_decode(std::string_view text);

// -- Ascii85 and Z85 (base_n.cpp) ---------------------------------------------------------------

struct Ascii85Style {
  bool wrapper = true;         ///< surround the data with "<~" and "~>" (Adobe)
  std::size_t line_width = 0;  ///< start a new line every N characters; 0 = single line
};
/// Adobe Ascii85: 'z' stands for a group of four zero bytes.
[[nodiscard]] std::string ascii85_encode(ByteView data, const Ascii85Style& style = {});
/// Accepts wrapped and bare data; ASCII whitespace is ignored.
[[nodiscard]] Result<Bytes> ascii85_decode(std::string_view text);
/// ZeroMQ Z85. Fails with Status::Encode unless the size is a multiple of 4.
[[nodiscard]] Result<std::string> z85_encode(ByteView data);
/// The number of characters must be a multiple of 5.
[[nodiscard]] Result<Bytes> z85_decode(std::string_view text);

// -- gzip + Base64 (base_n.cpp) -----------------------------------------------------------------

[[nodiscard]] Result<std::string> gzip_base64_encode(ByteView data, int compression_level, std::size_t line_width);
/// Fails with Status::Limit when the data inflates to more than `max_output` bytes.
[[nodiscard]] Result<Bytes> gzip_base64_decode(std::string_view text, std::uint64_t max_output);

// -- byte lists (byte_lists.cpp) -------------------------------------------------------------------

/// "110 145 154": three octal digits per byte, joined by `separator`.
[[nodiscard]] std::string octal_text(ByteView data, std::string_view separator = " ");
/// "72, 101, 108": decimal byte values joined by `separator`.
[[nodiscard]] std::string decimal_text(ByteView data, std::string_view separator = ", ");
/// Both parsers take values separated by whitespace and/or commas and reject
/// anything above 255.
[[nodiscard]] Result<Bytes> parse_octal_text(std::string_view text);
[[nodiscard]] Result<Bytes> parse_decimal_text(std::string_view text);

// -- quoted-printable (mail_encodings.cpp) ---------------------------------------------------------

/// RFC 2045. CRLF pairs in the data become hard line breaks; lines are kept
/// to 76 columns with soft breaks. With `escape_line_feeds` a lone LF is
/// written as "=0A" (binary safe, the canonical form); without it a lone LF
/// stays a bare line break, which reads better for Unix text.
[[nodiscard]] std::string quoted_printable_encode(ByteView data, bool escape_line_feeds = true);
/// Line breaks are CRLF or a bare LF: soft breaks vanish, hard breaks are kept
/// as written, and blanks in front of either are dropped as transport padding.
/// A CR on its own is data.
[[nodiscard]] Result<Bytes> quoted_printable_decode(std::string_view text);

// -- yEnc (mail_encodings.cpp) -----------------------------------------------------------------------

/// Single-part yEnc. NOTE: the result is 8-bit data, not UTF-8.
[[nodiscard]] std::string yenc_encode(ByteView data, std::string_view filename);
struct ParsedYenc {
  Bytes bytes;
  std::string filename;
};
/// Verifies the size and CRC-32 of the trailer when they are present.
[[nodiscard]] Result<ParsedYenc> yenc_decode(std::string_view text);

// -- array literals (array_literals.cpp) ---------------------------------------------------------------

enum class ArrayLanguage { Cpp, Python, JavaScript, Java, Go, Rust, Swift, CSharp };
/// `c_identifier(text)` adjusted for the language: upper case for a Rust
/// static, and never a reserved word.
[[nodiscard]] std::string array_identifier(std::string_view text, ArrayLanguage language);
/// The bytes as a source-code array in the given language. `comment`, when
/// not empty, becomes a one-line comment above the declaration.
[[nodiscard]] std::string array_literal(ByteView data, ArrayLanguage language, std::string_view variable_name,
                                        std::size_t bytes_per_line, std::string_view comment);

// -- HTML entities (html_entities.cpp) -------------------------------------------------------------------

/// & < > " ' become references; with `non_ascii` so does everything above
/// U+007F (as &#x...;), except the C1 controls, which HTML cannot express.
[[nodiscard]] std::string html_entities_encode(std::string_view text, bool non_ascii);
/// Decodes decimal and hexadecimal references and the HTML 4 entity names;
/// anything that is not a known reference is left untouched.
[[nodiscard]] std::string html_entities_decode(std::string_view text);

// -- percent-encoded text (text_transforms.cpp) ------------------------------------------------------------

/// Percent-encode UTF-8 text. `component` escapes everything except
/// A-Z a-z 0-9 - . _ ~; otherwise the reserved characters
/// : / ? # [ ] @ ! $ & ' ( ) * + , ; = are kept as well.
[[nodiscard]] std::string url_text_encode(std::string_view text, bool component);
/// The result is valid UTF-8 (escapes that do not form UTF-8 become U+FFFD).
[[nodiscard]] Result<std::string> url_text_decode(std::string_view text, bool plus_as_space);

// -- Unicode escapes (text_transforms.cpp) -------------------------------------------------------------------

enum class UnicodeEscapeStyle {
  U4,       ///< \uXXXX, surrogate pairs above U+FFFF
  UBraces,  ///< \u{1F600}
  XBytes,   ///< \xNN for every UTF-8 byte
  Python,   ///< \uXXXX and \UXXXXXXXX
};
/// Non-ASCII characters and backslashes are escaped; with
/// `escape_ascii_controls` so are control characters (\n, \r, \t, numeric).
[[nodiscard]] std::string unicode_escape(std::string_view text, UnicodeEscapeStyle style, bool escape_ascii_controls);
/// Understands all four styles plus \n \r \t \\ \" \' \b \f \v \0 \/.
[[nodiscard]] Result<std::string> unicode_unescape(std::string_view text);

// -- JSON string literal (text_transforms.cpp) -----------------------------------------------------------------

/// The text as one JSON string literal, quotes included.
[[nodiscard]] std::string json_string_encode(std::string_view text, bool ascii_only);
/// Strict: exactly one string literal, surrounded by nothing but JSON whitespace.
[[nodiscard]] Result<std::string> json_string_decode(std::string_view text);

// -- letter rotations (text_transforms.cpp) ----------------------------------------------------------------------

/// Both are their own inverse.
[[nodiscard]] std::string rot13(std::string_view text);
[[nodiscard]] std::string rot47(std::string_view text);

// -- punycode (punycode.cpp) ---------------------------------------------------------------------------------------

/// Longest label, in code points (encoding) or characters (decoding), that
/// the punycode functions accept; longer ones fail with Status::Limit.
inline constexpr std::size_t punycode_max_label = 4096;

/// RFC 3492 for one label, without any prefix and without case changes.
[[nodiscard]] Result<std::string> punycode_encode_label(std::u32string_view label);
[[nodiscard]] Result<std::u32string> punycode_decode_label(std::string_view label);
/// IDNA-style: every label (a run of letters, digits, hyphens and non-ASCII
/// characters) that contains non-ASCII characters is replaced by "xn--" plus
/// its punycode, ASCII letters lower-cased. Everything else is kept.
[[nodiscard]] Result<std::string> punycode_encode_text(std::string_view text);
/// Replaces every "xn--" label by the text it stands for.
[[nodiscard]] Result<std::string> punycode_decode_text(std::string_view text);

// -- Morse code and the NATO alphabet (morse_nato.cpp) ----------------------------------------------------------------

/// Letters are separated by one space and words by " / "; line breaks are
/// kept. `skipped`, when given, receives the number of characters that have
/// no code and were left out.
[[nodiscard]] std::string morse_encode(std::string_view text, std::size_t* skipped = nullptr);
/// Upper-case text. Dots may be '.', U+00B7 or U+2022; dashes '-', U+2212, U+2013 or U+2014.
[[nodiscard]] Result<std::string> morse_decode(std::string_view text);
/// "Alfa Bravo / Charlie": one code word per letter or digit.
[[nodiscard]] std::string nato_encode(std::string_view text, std::size_t* skipped = nullptr);
/// Case-insensitive; also accepts the spellings Alpha, Juliet, Xray, Whisky and Niner.
[[nodiscard]] Result<std::string> nato_decode(std::string_view text);

// -- registration of the groups above (called by register_encoding_family) ----------------------------------------------

void register_base_n(Registry& registry);
void register_byte_lists(Registry& registry);
void register_mail_encodings(Registry& registry);
void register_array_literals(Registry& registry);
void register_html_entities(Registry& registry);
void register_text_transforms(Registry& registry);
void register_punycode(Registry& registry);
void register_morse_nato(Registry& registry);

}  // namespace cvt::encoding
