// Convertible core - encoding family: mail and news transfer encodings
// (quoted-printable, yEnc).
//
// Adding another transfer encoding: write `foo_encode` / `foo_decode` below
// (declare them in encoding.hpp) and add one `add_binary_encoding` block to
// `register_mail_encodings` at the end of this file. Use `.decode_blob`
// instead of `.decode` when the encoding carries a file name.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/encoding/detail.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/crc.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

using detail::byte_at;
using detail::byte_of;

constexpr std::size_t qp_max_line = 76;     // RFC 2045: including the '=' of a soft break
constexpr std::size_t yenc_line_length = 128;

// The value of " key=value" in a yEnc control line (up to the next space).
std::optional<std::string_view> yenc_parameter(std::string_view line, std::string_view key) {
  std::size_t from = 0;
  while (from < line.size()) {
    const std::size_t at = line.find(key, from);
    if (at == std::string_view::npos) return std::nullopt;
    const std::size_t value_start = at + key.size();
    if (at > 0 && line[at - 1] == ' ' && value_start < line.size() && line[value_start] == '=') {
      const std::string_view rest = line.substr(value_start + 1);
      return rest.substr(0, rest.find(' '));
    }
    from = at + 1;
  }
  return std::nullopt;
}

// File names are free text in yEnc, but must stay on the header line.
std::string yenc_name(std::string_view filename) {
  std::string name;
  for (const char c : filename) {
    if (byte_of(c) >= 0x20 && c != 0x7F) name.push_back(c);
  }
  name = std::string(util::trim(name));
  return name.empty() ? std::string("data.bin") : name;
}

int sniff_yenc(ByteView data, std::string_view /*filename*/) {
  return util::trim_left(as_chars(data).substr(0, 256)).starts_with("=ybegin ") ? 90 : 0;
}

}  // namespace

// --------------------------------------------------------------------------- quoted-printable

std::string quoted_printable_encode(ByteView data, bool escape_line_feeds) {
  const std::string_view soft_break = escape_line_feeds ? "=\r\n" : "=\n";
  std::string out;
  out.reserve(data.size() + data.size() / 8 + 16);
  std::size_t column = 0;
  const auto is_hard_break = [&](std::size_t i) {
    if (i >= data.size()) return false;
    if (byte_at(data, i) == '\r') return i + 1 < data.size() && byte_at(data, i + 1) == '\n';
    return byte_at(data, i) == '\n' && !escape_line_feeds;
  };
  for (std::size_t i = 0; i < data.size(); ++i) {
    const std::uint8_t value = byte_at(data, i);
    if (is_hard_break(i)) {
      if (value == '\r') {
        out.append("\r\n");
        ++i;
      } else {
        out.push_back('\n');
      }
      column = 0;
      continue;
    }
    bool literal = value >= 33 && value <= 126 && value != '=';
    if (value == ' ' || value == '\t') {
      // White space is only data when something visible follows on the line.
      literal = i + 1 < data.size() && !is_hard_break(i + 1);
    }
    const std::size_t width = literal ? 1 : 3;
    if (column + width > qp_max_line - 1) {
      out.append(soft_break);
      column = 0;
    }
    if (literal) {
      out.push_back(static_cast<char>(value));
    } else {
      out.push_back('=');
      detail::append_hex_byte(out, value, true);
    }
    column += width;
  }
  return out;
}

Result<Bytes> quoted_printable_decode(std::string_view text) {
  Bytes out;
  out.reserve(text.size());
  const auto is_blank = [](char c) { return c == ' ' || c == '\t'; };
  // Length of the line break (CRLF or a bare LF) at `position`, or 0. A CR
  // on its own is data: encoders that follow the RFC never write one, and
  // those that do not mean the byte.
  const auto break_length = [text](std::size_t position) -> std::size_t {
    if (position >= text.size()) return 0;
    if (text[position] == '\n') return 1;
    return text[position] == '\r' && position + 1 < text.size() && text[position + 1] == '\n' ? 2 : 0;
  };
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '=') {
      // Soft line break: '=', optional transport padding, end of line.
      std::size_t end = i + 1;
      while (end < text.size() && is_blank(text[end])) ++end;
      if (end == text.size()) break;
      if (const std::size_t length = break_length(end); length > 0) {
        i = end + length;
        continue;
      }
      const int high = i + 1 < text.size() ? util::hex_digit_value(text[i + 1]) : -1;
      const int low = i + 2 < text.size() ? util::hex_digit_value(text[i + 2]) : -1;
      if (high < 0 || low < 0) return fail(Status::Decode, "invalid quoted-printable escape at offset {}", i);
      out.push_back(static_cast<std::byte>((high << 4) | low));
      i += 3;
      continue;
    }
    if (is_blank(c)) {
      // White space at the end of a line was added in transport and is dropped.
      std::size_t end = i;
      while (end < text.size() && is_blank(text[end])) ++end;
      if (end < text.size() && break_length(end) == 0) append(out, text.substr(i, end - i));
      i = end;
      continue;
    }
    out.push_back(static_cast<std::byte>(byte_of(c)));
    ++i;
  }
  return out;
}

// --------------------------------------------------------------------------- yEnc

std::string yenc_encode(ByteView data, std::string_view filename) {
  std::string out = std::format("=ybegin line={} size={} name={}\r\n", yenc_line_length, data.size(), yenc_name(filename));
  out.reserve(out.size() + data.size() + data.size() / 32 + 64);
  std::size_t column = 0;
  for (std::size_t i = 0; i < data.size(); ++i) {
    const auto value = static_cast<std::uint8_t>((byte_at(data, i) + 42u) & 0xFFu);
    bool escape = value == 0x00 || value == '\n' || value == '\r' || value == '=';
    // Blanks at either end of a line and a leading dot do not survive every
    // news server, so they are escaped as well.
    if ((value == '\t' || value == ' ') && (column == 0 || column + 1 >= yenc_line_length || i + 1 == data.size())) {
      escape = true;
    }
    if (value == '.' && column == 0) escape = true;
    if (escape) {
      out.push_back('=');
      out.push_back(static_cast<char>((value + 64u) & 0xFFu));
      column += 2;
    } else {
      out.push_back(static_cast<char>(value));
      column += 1;
    }
    if (column >= yenc_line_length) {
      out.append("\r\n");
      column = 0;
    }
  }
  if (column > 0) out.append("\r\n");
  out += std::format("=yend size={} crc32={:08x}\r\n", data.size(), util::crc32(data));
  return out;
}

Result<ParsedYenc> yenc_decode(std::string_view text) {
  const std::vector<std::string_view> lines = util::split_lines(text);
  std::size_t index = 0;
  while (index < lines.size() && !lines[index].starts_with("=ybegin ")) ++index;
  if (index == lines.size()) return fail(Status::Decode, "no yEnc '=ybegin' line found");

  ParsedYenc out;
  std::string_view header = lines[index];
  if (const std::size_t name = header.find(" name="); name != std::string_view::npos) {
    out.filename = util::sanitize_utf8(util::trim(header.substr(name + 6)));
    header = header.substr(0, name);
  }
  if (yenc_parameter(header, "part")) return fail(Status::Decode, "multi-part yEnc files are not supported");
  const auto declared_size = [](std::string_view line) -> Result<std::optional<std::uint64_t>> {
    const auto value = yenc_parameter(line, "size");
    if (!value) return std::optional<std::uint64_t>{};
    const auto parsed = util::parse_int(*value);
    if (!parsed || *parsed < 0) return fail(Status::Decode, "the yEnc size field is not a number");
    return std::optional<std::uint64_t>{static_cast<std::uint64_t>(*parsed)};
  };
  CVT_TRY_ASSIGN(const std::optional<std::uint64_t> header_size, declared_size(header));

  std::string_view trailer;
  bool ended = false;
  for (++index; index < lines.size(); ++index) {
    const std::string_view line = lines[index];
    if (line.starts_with("=yend")) {
      trailer = line;
      ended = true;
      break;
    }
    if (line.starts_with("=ypart ")) return fail(Status::Decode, "multi-part yEnc files are not supported");
    for (std::size_t k = 0; k < line.size(); ++k) {
      unsigned value = byte_of(line[k]);
      if (line[k] == '=') {
        if (++k >= line.size()) return fail(Status::Decode, "yEnc line {} ends inside an escape sequence", index + 1);
        value = byte_of(line[k]) - 64u;
      }
      out.bytes.push_back(static_cast<std::byte>((value - 42u) & 0xFFu));
    }
  }
  if (!ended) return fail(Status::Decode, "the yEnc data has no '=yend' line");

  CVT_TRY_ASSIGN(const std::optional<std::uint64_t> trailer_size, declared_size(trailer));
  for (const std::optional<std::uint64_t>& size : {header_size, trailer_size}) {
    if (size && *size != out.bytes.size()) {
      return fail(Status::Decode, "the yEnc data holds {} bytes but declares {}", out.bytes.size(), *size);
    }
  }
  if (const auto crc = yenc_parameter(trailer, "crc32")) {
    const auto expected = util::parse_int(*crc, 16);
    if (!expected || *expected < 0 || *expected > 0xFFFF'FFFFll) {
      return fail(Status::Decode, "the yEnc crc32 field is not a hexadecimal number");
    }
    const std::uint32_t actual = util::crc32(out.bytes);
    if (static_cast<std::uint32_t>(*expected) != actual) {
      return fail(Status::Decode, "yEnc checksum mismatch: the data has CRC-32 {:08x} but {:08x} was declared", actual,
                  static_cast<std::uint32_t>(*expected));
    }
  }
  return out;
}

// --------------------------------------------------------------------------- registration

void register_mail_encodings(Registry& registry) {
  add_binary_encoding(registry, {
      .id = "quoted-printable", .name = "Quoted-printable text",
      .description = "The MIME encoding that keeps ASCII text readable and writes other bytes as =XX (RFC 2045).",
      .extensions = {"qp"}, .popularity = 30,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        return quoted_printable_encode(blob.bytes, ctx.options.get_bool("escape_line_feeds", true));
      },
      .decode = [](std::string_view text, const ReadContext&) { return quoted_printable_decode(text); },
      .encode_options = {OptionSpec::boolean(
          "escape_line_feeds", "Escape line feeds", true,
          "Write a line feed without a carriage return as =0A; turn off to keep Unix line breaks readable.")},
      .expansion = 4,  // three characters per byte plus a soft break every 75 characters
  });

  add_binary_encoding(registry, {
      .id = "yenc", .name = "yEnc data", .description = "The compact 8-bit encoding used for binaries on Usenet.",
      .extensions = {"yenc", "ync"}, .mime = "application/x-yenc", .popularity = 10,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        std::string name = ctx.options.get_string("filename");
        if (name.empty()) name = blob.filename.empty() ? ctx.source_filename : blob.filename;
        return yenc_encode(blob.bytes, name);
      },
      .decode_blob = [](std::string_view text, const ReadContext&) -> Result<ir::Blob> {
        CVT_TRY_ASSIGN(ParsedYenc parsed, yenc_decode(text));
        ir::Blob blob;
        blob.bytes = std::move(parsed.bytes);
        blob.filename = std::move(parsed.filename);
        return blob;
      },
      .encode_options = {OptionSpec::text("filename", "File name", "",
                                          "Name stored in the '=ybegin' line; empty uses the source file name.")},
      .expansion = 3,  // two characters per byte in the worst case, plus line breaks
      .sniff = sniff_yenc,
  });
}

}  // namespace cvt::encoding
