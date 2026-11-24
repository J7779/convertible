// Convertible core - encoding family: byte views and embeddable listings.
//
//   hexdump-c    `hexdump -C` canonical dump (with '*' for repeated lines)
//   od           `od` dumps: octal words (default), hex bytes or hex words
//   asm-bytes    assembler data directives: NASM/MASM `db`, GNU as `.byte`
//   bytes-inc    a bare comma-separated byte list to #include inside an array
//   base64-json  a JSON envelope: file name, media type, size, SHA-256, Base64 data
// Every dump reads back to the original bytes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/encoding/detail.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/base64.hpp"
#include "util/hash.hpp"
#include "util/hex.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

using detail::byte_at;

bool same_line(ByteView data, std::size_t a, std::size_t b, std::size_t width) {
  return b + width <= data.size() && std::equal(data.begin() + static_cast<std::ptrdiff_t>(a),
                                                data.begin() + static_cast<std::ptrdiff_t>(a + width),
                                                data.begin() + static_cast<std::ptrdiff_t>(b));
}

// ---------------------------------------------------------------------------- hexdump -C

std::string hexdump_canonical(ByteView data, bool squeeze) {
  std::string out;
  out.reserve(data.size() * 4 + 32);
  bool starred = false;
  for (std::size_t at = 0; at < data.size(); at += 16) {
    if (squeeze && at >= 16 && same_line(data, at - 16, at, 16)) {
      if (!starred) out += "*\n";
      starred = true;
      continue;
    }
    starred = false;
    out += std::format("{:08x} ", at);
    const std::size_t n = std::min<std::size_t>(16, data.size() - at);
    for (std::size_t k = 0; k < 16; ++k) {
      if (k % 8 == 0) out.push_back(' ');
      if (k < n) {
        detail::append_hex_byte(out, byte_at(data, at + k), false);
        out.push_back(' ');
      } else {
        out += "   ";
      }
    }
    out += " |";
    for (std::size_t k = 0; k < n; ++k) {
      const std::uint8_t b = byte_at(data, at + k);
      out.push_back(b >= 0x20 && b < 0x7F ? static_cast<char>(b) : '.');
    }
    out += "|\n";
  }
  if (!data.empty()) out += std::format("{:08x}\n", data.size());
  return out;
}

// ---------------------------------------------------------------------------- od

enum class OdType { Octal2, Hex1, Hex2 };

std::string od_dump(ByteView data, OdType type, bool squeeze) {
  std::string out;
  out.reserve(data.size() * 4 + 32);
  bool starred = false;
  for (std::size_t at = 0; at < data.size(); at += 16) {
    if (squeeze && at >= 16 && same_line(data, at - 16, at, 16)) {
      if (!starred) out += "*\n";
      starred = true;
      continue;
    }
    starred = false;
    out += std::format("{:07o}", at);
    const std::size_t n = std::min<std::size_t>(16, data.size() - at);
    if (type == OdType::Hex1) {
      for (std::size_t k = 0; k < n; ++k) out += std::format(" {:02x}", byte_at(data, at + k));
    } else {
      for (std::size_t k = 0; k < n; k += 2) {
        const unsigned word = byte_at(data, at + k) | (k + 1 < n ? static_cast<unsigned>(byte_at(data, at + k + 1)) << 8 : 0u);
        out += type == OdType::Octal2 ? std::format(" {:06o}", word) : std::format(" {:04x}", word);
      }
    }
    out.push_back('\n');
  }
  out += std::format("{:07o}\n", data.size());
  return out;
}

// ---------------------------------------------------------------------------- reading dumps back

/// Parses lines of "<offset> <values...>" with '*' repeats. `value_digits`
/// and `radix` describe one value; `bytes_per_value` and little-endian
/// order turn it into bytes. The last offset, when present, gives the length.
Result<Bytes> parse_dump(std::string_view text, int offset_radix, bool canonical, std::uint64_t max_output) {
  Bytes out;
  Bytes previous;
  bool repeat = false;
  std::size_t number = 0;
  std::optional<std::uint64_t> final_length;
  for (std::string_view line : util::split_lines(text)) {
    ++number;
    line = util::trim(line);
    if (line.empty()) continue;
    if (line == "*") {
      repeat = true;
      continue;
    }
    if (canonical) {
      const std::size_t bar = line.find("  |");
      if (bar != std::string_view::npos) line = line.substr(0, bar);
    }
    const std::vector<std::string_view> fields = util::split(line, ' ', false);
    const auto offset = util::parse_int(fields[0], offset_radix);
    if (!offset || *offset < 0) return fail(Status::Decode, "bad offset on dump line {}", number);
    if (repeat) {
      if (previous.empty()) return fail(Status::Decode, "'*' without a preceding line on dump line {}", number);
      while (out.size() < static_cast<std::uint64_t>(*offset)) {
        if (out.size() + previous.size() > max_output) return fail(Status::Limit, "the dump expands beyond the limit");
        out.insert(out.end(), previous.begin(), previous.end());
      }
      repeat = false;
    }
    if (fields.size() == 1) {  // the final offset: the total length
      final_length = static_cast<std::uint64_t>(*offset);
      break;
    }
    if (static_cast<std::uint64_t>(*offset) != out.size()) {
      return fail(Status::Decode, "dump line {} starts at offset {} but {} bytes precede it", number, *offset, out.size());
    }
    Bytes row;
    for (std::size_t i = 1; i < fields.size(); ++i) {
      const std::string_view value = fields[i];
      if (value.size() == 2) {
        const auto v = util::parse_int(value, 16);
        if (!v) return fail(Status::Decode, "bad byte '{}' on dump line {}", value, number);
        row.push_back(static_cast<std::byte>(*v));
      } else if (value.size() == 4 || value.size() == 6) {
        const auto v = util::parse_int(value, value.size() == 6 ? 8 : 16);
        if (!v || *v > 0xFFFF) return fail(Status::Decode, "bad word '{}' on dump line {}", value, number);
        row.push_back(static_cast<std::byte>(*v & 0xFF));
        row.push_back(static_cast<std::byte>(*v >> 8));
      } else {
        return fail(Status::Decode, "unexpected value '{}' on dump line {}", value, number);
      }
    }
    if (out.size() + row.size() > max_output) return fail(Status::Limit, "the dump expands beyond the limit");
    out.insert(out.end(), row.begin(), row.end());
    previous = std::move(row);
  }
  if (final_length) {
    if (*final_length > out.size() || out.size() - *final_length > 1) {
      return fail(Status::Decode, "the final offset does not match the dumped data");
    }
    out.resize(static_cast<std::size_t>(*final_length));  // drops the padding byte of an odd-sized word dump
  }
  return out;
}

// ---------------------------------------------------------------------------- listings

std::string asm_listing(ByteView data, std::string_view syntax, std::string_view label, std::size_t per_line) {
  std::string out;
  const bool gas = syntax == "gas";
  const bool masm = syntax == "masm";
  if (!label.empty()) {
    if (gas) {
      out += std::format("    .global {0}\n{0}:\n", label);
    } else if (masm) {
      out += std::format("{}\n", label);
    } else {
      out += std::format("global {0}\n{0}:\n", label);
    }
  }
  for (std::size_t at = 0; at < data.size(); at += per_line) {
    out += gas ? "    .byte " : "    db ";
    const std::size_t n = std::min(per_line, data.size() - at);
    for (std::size_t k = 0; k < n; ++k) {
      if (k > 0) out += ", ";
      if (masm) {
        // MASM hex literals end in 'h' and must start with a digit.
        out += std::format("0{:02X}h", byte_at(data, at + k));
      } else {
        out += std::format("0x{:02x}", byte_at(data, at + k));
      }
    }
    out.push_back('\n');
  }
  if (!label.empty() && !masm) out += gas ? std::format("    .set {0}_len, . - {0}\n", label) : std::format("{0}_len equ $ - {0}\n", label);
  return out;
}

std::string inc_listing(ByteView data, std::size_t per_line, bool uppercase) {
  std::string out;
  for (std::size_t at = 0; at < data.size(); at += per_line) {
    const std::size_t n = std::min(per_line, data.size() - at);
    for (std::size_t k = 0; k < n; ++k) {
      if (k > 0) out.push_back(' ');
      out += "0x";
      detail::append_hex_byte(out, byte_at(data, at + k), uppercase);
      out.push_back(',');
    }
    out.push_back('\n');
  }
  return out;
}

Result<Bytes> parse_byte_list(std::string_view text) {
  // Comments and labels are skipped; every 0x.., $.., ..h or decimal number up to 255 is a byte.
  Bytes out;
  std::size_t number = 0;
  for (std::string_view line : util::split_lines(text)) {
    ++number;
    for (const std::string_view marker : {";", "//", "#", "@"}) {
      const std::size_t at = line.find(marker);
      if (at != std::string_view::npos && !(marker == "#" && line.substr(at).starts_with("#0x"))) line = line.substr(0, at);
    }
    line = util::trim(line);
    bool directive_found = false;
    for (const std::string_view directive : {".byte", "db", "DB", ".BYTE", "defb", "DEFB", ".db"}) {
      const std::size_t at = line.find(directive);
      if (at != std::string_view::npos && (at == 0 || util::is_ascii_space(line[at - 1])) &&
          (at + directive.size() == line.size() || util::is_ascii_space(line[at + directive.size()]))) {
        line = line.substr(at + directive.size());
        directive_found = true;
        break;
      }
    }
    if (!directive_found) {
      // Labels and other statements: only bare value lists (an .inc file) carry bytes.
      if (line.empty() || !(util::is_ascii_digit(line[0]) || line[0] == '$')) continue;
    }
    for (std::string_view token : util::split(line, ',', false)) {
      token = util::trim(token);
      if (token.empty()) continue;
      std::optional<std::int64_t> value;
      if (util::istarts_with(token, "0x")) {
        value = util::parse_int(token.substr(2), 16);
      } else if (token.starts_with('$')) {
        value = util::parse_int(token.substr(1), 16);
      } else if (token.ends_with('h') || token.ends_with('H')) {
        value = util::parse_int(token.substr(0, token.size() - 1), 16);
      } else {
        value = util::parse_int(token, 10);
      }
      if (!value || *value < 0 || *value > 255) return fail(Status::Decode, "bad byte '{}' on line {}", token, number);
      out.push_back(static_cast<std::byte>(*value));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------- JSON envelope

std::string media_type_of(const ir::Blob& blob, const WriteContext& ctx) {
  if (!blob.mime.empty() && blob.mime != "application/octet-stream") return blob.mime;
  if (ctx.registry != nullptr) {
    if (const auto detection = detect_format(*ctx.registry, blob.bytes, blob.filename);
        detection && detection->format != nullptr && !detection->format->mime.empty()) {
      return detection->format->mime;
    }
  }
  return "application/octet-stream";
}

std::string sha256_hex_of(ByteView data) { return util::sha256_hex(data); }

}  // namespace

void register_byte_views(Registry& registry) {
  const OptionSpec squeeze = OptionSpec::boolean("squeeze", "Collapse repeated lines", true,
                                                 "Write '*' instead of lines that repeat the previous one, like hexdump and od.");
  add_binary_encoding(registry, {
      .id = "hexdump-c", .name = "Hex dump (hexdump -C)",
      .description = "Canonical hex+ASCII dump in the layout of `hexdump -C`.",
      .extensions = {"txt", "hexdump"}, .popularity = 30,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        return hexdump_canonical(blob.bytes, ctx.options.get_bool("squeeze", true));
      },
      .decode = [](std::string_view text, const ReadContext& ctx) {
        return parse_dump(text, 16, true, ctx.limits.max_decompressed_bytes);
      },
      .encode_options = {squeeze},
      .expansion = 6,
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = as_chars(data).substr(0, 80);
        return text.size() >= 14 && text.substr(8, 2) == "  " && text.find('|') != std::string_view::npos &&
                       std::ranges::all_of(text.substr(0, 8), [](char c) { return util::hex_digit_value(c) >= 0; }) &&
                       util::hex_digit_value(text[10]) >= 0 && util::hex_digit_value(text[11]) >= 0 && text[12] == ' '
                   ? 88
                   : 0;
      },
  });
  add_binary_encoding(registry, {
      .id = "od", .name = "Octal dump (od)",
      .description = "Dump in the layout of `od`: octal offsets with octal or hexadecimal values.",
      .extensions = {"od"}, .popularity = 15,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        const std::string type = ctx.options.get_enum("type", "o2", {"o2", "x1", "x2"});
        return od_dump(blob.bytes, type == "x1" ? OdType::Hex1 : type == "x2" ? OdType::Hex2 : OdType::Octal2,
                       ctx.options.get_bool("squeeze", true));
      },
      .decode = [](std::string_view text, const ReadContext& ctx) {
        return parse_dump(text, 8, false, ctx.limits.max_decompressed_bytes);
      },
      .encode_options = {OptionSpec::choice("type", "Values", "o2", {"o2", "x1", "x2"},
                                            "o2: octal 2-byte words (od's default); x1: hex bytes (od -t x1); "
                                            "x2: hex 2-byte words (od -t x2)."),
                         squeeze},
      .expansion = 6,
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = as_chars(data).substr(0, 64);
        if (text.size() < 10 || text.substr(0, 7) != "0000000" || text[7] != ' ') return 0;
        const std::string_view first = text.substr(8, text.find_first_of(" \n", 8) - 8);
        return (first.size() == 2 || first.size() == 4 || first.size() == 6) &&
                       std::ranges::all_of(first, [](char c) { return util::hex_digit_value(c) >= 0; })
                   ? 80
                   : 0;
      },
  });
  add_binary_encoding(registry, {
      .id = "asm-bytes", .name = "Assembly data listing",
      .description = "The bytes as assembler data directives: NASM/MASM `db` or GNU as `.byte`.",
      .extensions = {"asm", "s", "inc"}, .mime = "text/x-asm", .popularity = 12,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        std::string label = ctx.options.get_string("label");
        if (label.empty()) label = c_identifier(blob.filename.empty() ? ctx.source_filename : blob.filename);
        return asm_listing(blob.bytes, ctx.options.get_enum("syntax", "nasm", {"nasm", "gas", "masm"}), c_identifier(label),
                           static_cast<std::size_t>(ctx.options.get_int("bytes_per_line", 16, 1, 64)));
      },
      .decode = [](std::string_view text, const ReadContext&) { return parse_byte_list(text); },
      .encode_options = {OptionSpec::choice("syntax", "Assembler", "nasm", {"nasm", "gas", "masm"},
                                            "NASM/YASM `db 0x..`, GNU as `.byte 0x..`, or MASM `db 0..h`."),
                         OptionSpec::text("label", "Label", "", "Symbol name; empty derives it from the file name."),
                         OptionSpec::integer("bytes_per_line", "Bytes per line", 16, 1, 64, "Values per directive.")},
      .expansion = 8,
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = as_chars(data).substr(0, 512);
        return (text.find("\n    db 0") != std::string_view::npos || text.find("\n    .byte 0x") != std::string_view::npos ||
                text.starts_with("    db 0") || text.starts_with("    .byte 0x"))
                   ? 84
                   : 0;
      },
  });
  add_binary_encoding(registry, {
      .id = "bytes-inc", .name = "Byte list include file",
      .description = "Comma-separated 0x.. values to #include inside a C/C++ array initialiser (or any build tool).",
      .extensions = {"inc"}, .mime = "text/plain", .popularity = 15,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        return inc_listing(blob.bytes, static_cast<std::size_t>(ctx.options.get_int("bytes_per_line", 12, 1, 64)),
                           ctx.options.get_bool("uppercase", false));
      },
      .decode = [](std::string_view text, const ReadContext&) { return parse_byte_list(text); },
      .encode_options = {OptionSpec::integer("bytes_per_line", "Bytes per line", 12, 1, 64, "Values on each line."),
                         OptionSpec::boolean("uppercase", "Uppercase digits", false, "Write A-F in upper case.")},
      .expansion = 7,
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = as_chars(data).substr(0, 256);
        return text.starts_with("0x") && text.size() > 5 && text[4] == ',' ? 82 : 0;
      },
  });
  add_binary_encoding(registry, {
      .id = "base64-json", .name = "Base64 JSON envelope",
      .description = "A JSON object with the file name, media type, size, SHA-256 and the content as Base64.",
      .extensions = {"json", "b64.json"}, .mime = "application/json", .popularity = 20,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        Json root = Json::object();
        const std::string name = blob.filename.empty() ? ctx.source_filename : blob.filename;
        if (!name.empty()) root["filename"] = util::sanitize_utf8(name);
        root["mime"] = media_type_of(blob, ctx);
        root["size"] = static_cast<std::uint64_t>(blob.bytes.size());
        root["sha256"] = sha256_hex_of(blob.bytes);
        root["encoding"] = "base64";
        root["data"] = util::base64_encode(blob.bytes);
        return util::dump_json(root, 2) + "\n";
      },
      .decode_blob = [](std::string_view text, const ReadContext&) -> Result<ir::Blob> {
        CVT_TRY_ASSIGN(const Json root, util::parse_json(text));
        const Json* data = root.find("data");
        if (!root.is_object() || data == nullptr || !data->is_string()) {
          return fail(Status::Decode, "the JSON has no \"data\" string");
        }
        ir::Blob blob;
        CVT_TRY_ASSIGN(blob.bytes, util::base64_decode(data->as_string()));
        if (const Json* size = root.find("size"); size != nullptr && size->as_uint64(blob.bytes.size()) != blob.bytes.size()) {
          return fail(Status::Decode, "the decoded data has {} bytes, the envelope says {}", blob.bytes.size(),
                      size->as_uint64());
        }
        if (const Json* sha = root.find("sha256"); sha != nullptr && sha->is_string() &&
                                                   !util::iequals(sha->as_string(), sha256_hex_of(blob.bytes))) {
          return fail(Status::Decode, "SHA-256 mismatch: the data is damaged");
        }
        if (const Json* name = root.find("filename"); name != nullptr && name->is_string()) blob.filename = name->as_string();
        if (const Json* mime = root.find("mime"); mime != nullptr && mime->is_string()) blob.mime = mime->as_string();
        return blob;
      },
      .expansion = 2,
      .sniff = [](ByteView data, std::string_view) {
        const std::string_view text = as_chars(data).substr(0, 1024);
        return util::trim_left(text).starts_with("{") && text.find("\"encoding\": \"base64\"") != std::string_view::npos &&
                       text.find("\"data\": \"") != std::string_view::npos
                   ? 88
                   : 0;
      },
  });
}

}  // namespace cvt::encoding
