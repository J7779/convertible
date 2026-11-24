// Convertible core - encoding family.
//
// Two groups of formats live here:
//
//  1. Binary-to-text encodings and reports that accept ANY file. They are
//     Blob writers, and since the engine can read every format as a Blob,
//     they show up as targets for every input: base64, base64url, hex,
//     hexdump, data-uri, c-header, uue, url-encoded, binary-text, checksums,
//     and bin (the raw bytes). Most also have a Blob reader that decodes
//     them back, so e.g. base64 -> bin restores the original file.
//
//  2. Plain text (`txt`, txt.cpp), read into and written from ir::PlainText
//     with a choice of character encoding, byte order mark and line endings,
//     plus the PlainText -> Document bridge that lets every document writer
//     accept plain text.
//
// This file is also the reference for how a family registers itself; see
// core/README.md for the walkthrough.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "families/families.hpp"
#include "util/base64.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt {

namespace {

using namespace std::string_view_literals;

// --------------------------------------------------------------------------- shared option specs

OptionSpec line_ending_option() {
  return OptionSpec::choice("line_ending", "Line ending", "lf", {"lf", "crlf"},
                            "Line terminator used when the output is wrapped.")
      .advanced();
}

OptionSpec uppercase_option() {
  return OptionSpec::boolean("uppercase", "Uppercase digits", false, "Write hexadecimal digits A-F in upper case.");
}

std::string_view newline_for(const Options& options) {
  return options.get_enum("line_ending", "lf", {"lf", "crlf"}) == "crlf" ? "\r\n"sv : "\n"sv;
}

// --------------------------------------------------------------------------- binary-to-text encodings

Result<std::string> encode_base64(const ir::Blob& blob, const WriteContext& ctx) {
  util::Base64Options style;
  style.url_safe = ctx.format_id == "base64url";
  style.padding = ctx.options.get_bool("padding", !style.url_safe);
  style.line_width = static_cast<std::size_t>(ctx.options.get_int("line_width", 0, 0, 1024));
  style.newline = newline_for(ctx.options);
  std::string text = util::base64_encode(blob.bytes, style);
  if (style.line_width > 0 && !text.empty()) text.append(style.newline);
  return text;
}

Result<Bytes> decode_base64(std::string_view text, const ReadContext&) { return util::base64_decode(text); }

Result<std::string> encode_hex(const ir::Blob& blob, const WriteContext& ctx) {
  encoding::HexStyle style;
  style.uppercase = ctx.options.get_bool("uppercase", false);
  style.bytes_per_line = static_cast<std::size_t>(ctx.options.get_int("bytes_per_line", 0, 0, 4096));
  const std::string separator =
      ctx.options.get_enum("separator", "none", {"none", "space", "colon", "comma", "comma-space", "dash"});
  style.separator = separator == "space"         ? " "sv
                    : separator == "colon"       ? ":"sv
                    : separator == "comma"       ? ","sv
                    : separator == "comma-space" ? ", "sv
                    : separator == "dash"        ? "-"sv
                                                 : ""sv;
  const std::string prefix = ctx.options.get_enum("prefix", "none", {"none", "0x", "\\x", "%"});
  style.prefix = prefix == "0x" ? "0x"sv : prefix == "\\x" ? "\\x"sv : prefix == "%" ? "%"sv : ""sv;
  return encoding::hex_text(blob.bytes, style);
}

Result<std::string> encode_data_uri(const ir::Blob& blob, const WriteContext& ctx) {
  std::string mime = ctx.options.get_string("mime");
  if (mime.empty() && blob.mime != "application/octet-stream") mime = blob.mime;
  if (mime.empty() && ctx.registry != nullptr) {
    // Sniff the content with the registry (a PNG file called "x.bin" is still image/png).
    if (const auto detection = detect_format(*ctx.registry, blob.bytes, blob.filename);
        detection && detection->format != nullptr && detection->confidence >= 50) {
      mime = detection->format->mime;
    }
  }
  if (mime.empty()) mime = "application/octet-stream";
  for (const char c : mime) {
    // A media type is token "/" token with optional parameters; anything
    // that could break out of the URI header is rejected.
    if (static_cast<unsigned char>(c) <= 0x20 || c == ',' || c == 0x7F) {
      return fail(Status::InvalidArgument, "option 'mime' is not a valid media type");
    }
  }
  return encoding::data_uri(blob.bytes, mime, ctx.options.get_bool("base64", true));
}

Result<ir::Blob> decode_data_uri(std::string_view text, const ReadContext&) {
  CVT_TRY_ASSIGN(encoding::ParsedDataUri parsed, encoding::parse_data_uri(text));
  ir::Blob blob;
  blob.bytes = std::move(parsed.bytes);
  blob.mime = std::move(parsed.mime);
  return blob;
}

Result<std::string> encode_c_header(const ir::Blob& blob, const WriteContext& ctx) {
  std::string name = ctx.options.get_string("variable_name");
  if (name.empty()) name = blob.filename.empty() ? ctx.source_filename : blob.filename;
  const auto bytes_per_line = static_cast<std::size_t>(ctx.options.get_int("bytes_per_line", 12, 1, 64));
  std::string comment;
  if (!blob.filename.empty()) comment = blob.filename + ", " + std::to_string(blob.bytes.size()) + " bytes";
  return encoding::c_header(blob.bytes, name, bytes_per_line, comment);
}

Result<ir::Blob> decode_uue(std::string_view text, const ReadContext&) {
  CVT_TRY_ASSIGN(encoding::ParsedUu parsed, encoding::uudecode(text));
  ir::Blob blob;
  blob.bytes = std::move(parsed.bytes);
  blob.filename = std::move(parsed.filename);
  return blob;
}

// --------------------------------------------------------------------------- detection


int sniff_hexdump(ByteView data, std::string_view /*filename*/) {
  const std::string_view text = as_chars(data).substr(0, 64);
  if (text.size() < 12 || text[8] != ':' || text[9] != ' ') return 0;
  for (std::size_t i = 0; i < 8; ++i) {
    if (util::hex_digit_value(text[i]) < 0) return 0;
  }
  return util::hex_digit_value(text[10]) >= 0 && util::hex_digit_value(text[11]) >= 0 ? 85 : 0;
}

int sniff_data_uri(ByteView data, std::string_view /*filename*/) {
  const std::string_view text = util::trim_left(as_chars(data).substr(0, 512));
  return util::istarts_with(text, "data:") && text.find(',') != std::string_view::npos ? 90 : 0;
}

int sniff_c_header(ByteView data, std::string_view /*filename*/) {
  const std::string_view text = as_chars(data).substr(0, 1024);
  return text.find("unsigned char ") != std::string_view::npos && text.find("[] = {") != std::string_view::npos &&
                 text.find("0x") != std::string_view::npos
             ? 82
             : 0;
}

int sniff_uue(ByteView data, std::string_view /*filename*/) {
  const std::string_view text = as_chars(data).substr(0, 16);
  if (!text.starts_with("begin ") || text.size() < 10) return 0;
  for (std::size_t i = 6; i < 9; ++i) {
    if (text[i] < '0' || text[i] > '7') return 0;
  }
  if (text[9] != ' ') return 0;
  // xxencode shares the "begin" line; its data lines use lower-case letters,
  // which uuencode (characters 0x20-0x60) never does.
  const std::string_view body = as_chars(data).substr(0, 1024);
  const std::size_t first_line = body.find('\n');
  if (first_line != std::string_view::npos) {
    const std::string_view rest = body.substr(first_line + 1);
    if (std::ranges::any_of(rest.substr(0, rest.find("\nend")), [](char c) { return c >= 'a' && c <= 'z'; })) return 0;
  }
  return 88;
}


}  // namespace

void register_encoding_family(Registry& registry) {
  using encoding::add_binary_encoding;

  // ---- raw bytes -------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "bin", .name = "Raw binary data",
      .description = "The bytes of the file, unchanged or decoded from a text encoding.",
      .extensions = {"bin"}, .mime = "application/octet-stream", .popularity = 30,
      .encode = [](const ir::Blob& blob, const WriteContext&) -> Result<std::string> { return to_string(blob.bytes); },
      .expansion = 1,
  });

  // ---- Base64 ----------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "base64", .name = "Base64 text", .description = "Binary data as printable ASCII (RFC 4648).",
      .extensions = {"b64", "base64"}, .popularity = 70,
      .encode = encode_base64, .decode = decode_base64,
      .encode_options = {OptionSpec::integer("line_width", "Line width", 76, 0, 1024,
                                             "Characters per line; 0 writes a single unbroken line."),
                         OptionSpec::boolean("padding", "Padding", true, "End the output with '=' padding characters."),
                         line_ending_option()},
      .expansion = 5,  // 4/3, plus up to two line-ending bytes per character at line_width 1
  });
  add_binary_encoding(registry, {
      .id = "base64url", .name = "Base64 URL-safe text",
      .description = "Base64 with '-' and '_' so it can be used in URLs and file names.",
      .extensions = {"b64url"}, .popularity = 40,
      .encode = encode_base64, .decode = decode_base64,
      .encode_options = {OptionSpec::boolean("padding", "Padding", false, "End the output with '=' padding characters.")},
      .expansion = 2,
  });

  // ---- hexadecimal -------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "hex", .name = "Hexadecimal text", .description = "Two hexadecimal digits per byte.",
      .extensions = {"hex"}, .popularity = 60,
      .encode = encode_hex,
      .decode = [](std::string_view text, const ReadContext&) { return encoding::parse_hex_text(text); },
      .encode_options = {uppercase_option(),
                         OptionSpec::integer("bytes_per_line", "Bytes per line", 0, 0, 4096,
                                             "Start a new line after this many bytes; 0 writes a single line."),
                         OptionSpec::choice("separator", "Separator", "none",
                                            {"none", "space", "colon", "comma", "comma-space", "dash"},
                                            "Character placed between bytes."),
                         OptionSpec::choice("prefix", "Byte prefix", "none", {"none", "0x", "\\x", "%"},
                                            "Written before every byte: 0x41, \\x41 or %41.")},
      .expansion = 8,
  });
  add_binary_encoding(registry, {
      .id = "hexdump", .name = "Hex dump",
      .description = "Offsets, hexadecimal bytes and printable characters, like xxd.",
      .extensions = {"txt", "hexdump", "xxd"}, .popularity = 45,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        return encoding::hexdump_text(blob.bytes, static_cast<std::size_t>(ctx.options.get_int("bytes_per_line", 16, 1, 64)),
                                      ctx.options.get_bool("uppercase", false));
      },
      .decode = [](std::string_view text, const ReadContext&) { return encoding::parse_hexdump(text); },
      .encode_options = {OptionSpec::integer("bytes_per_line", "Bytes per line", 16, 1, 64, "Bytes shown on each line."),
                         uppercase_option()},
      .expansion = 16,
      .sniff = sniff_hexdump,
  });

  // ---- data: URI ---------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "data-uri", .name = "Data URI",
      .description = "The file embedded in a data: URL for use in HTML or CSS.",
      .extensions = {"txt", "datauri"}, .popularity = 50,
      .encode = encode_data_uri, .decode_blob = decode_data_uri,
      .encode_options = {OptionSpec::text("mime", "Media type", "",
                                          "Media type written into the URI; empty uses the type of the source file."),
                         OptionSpec::boolean("base64", "Base64", true,
                                             "Encode the data as Base64; otherwise percent-encode it.")},
      .expansion = 4,
      .sniff = sniff_data_uri,
  });

  // ---- C source ----------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "c-header", .name = "C array header",
      .description = "The file as an unsigned char array for embedding in C or C++.",
      .extensions = {"h"}, .mime = "text/x-c", .popularity = 40,
      .encode = encode_c_header,
      .encode_options = {OptionSpec::text("variable_name", "Variable name", "",
                                          "Name of the array; empty derives it from the file name."),
                         OptionSpec::integer("bytes_per_line", "Bytes per line", 12, 1, 64,
                                             "Array elements on each line.")},
      .expansion = 8,
      .sniff = sniff_c_header,
  });

  // ---- uuencode ------------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "uue", .name = "Uuencoded text", .description = "Classic Unix-to-Unix encoding.",
      .extensions = {"uue", "uu"}, .mime = "text/x-uuencode", .popularity = 20,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        std::string name = ctx.options.get_string("filename");
        return encoding::uuencode(blob.bytes, name.empty() ? blob.filename : name);
      },
      .decode_blob = decode_uue,
      .encode_options = {OptionSpec::text("filename", "File name", "",
                                          "Name stored in the 'begin' line; empty uses the source file name.")},
      .expansion = 2,
      .sniff = sniff_uue,
  });

  // ---- percent-encoding ------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "url-encoded", .name = "URL-encoded text", .description = "Percent-encoding as used in URLs and form data.",
      .extensions = {"urlencoded"}, .popularity = 35,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        return encoding::percent_encode(blob.bytes, ctx.options.get_bool("space_as_plus", false));
      },
      .decode = [](std::string_view text, const ReadContext& ctx) {
        return encoding::percent_decode(util::trim(text), ctx.options.get_bool("plus_as_space", false));
      },
      .encode_options = {OptionSpec::boolean("space_as_plus", "Space as +", false,
                                             "Write spaces as '+' (form encoding) instead of %20.")},
      .decode_options = {OptionSpec::boolean("plus_as_space", "+ means space", false,
                                             "Treat '+' in the input as a space (form encoding).")},
      .expansion = 3,
  });

  // ---- binary digits -----------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "binary-text", .name = "Binary digits", .description = "Every byte as eight 0/1 characters.",
      .extensions = {"bits"}, .popularity = 25,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        return encoding::binary_text(blob.bytes, ctx.options.get_bool("separate_bytes", true),
                                     static_cast<std::size_t>(ctx.options.get_int("bytes_per_line", 8, 0, 64)));
      },
      .decode = [](std::string_view text, const ReadContext&) { return encoding::parse_binary_text(text); },
      .encode_options = {OptionSpec::boolean("separate_bytes", "Separate bytes", true, "Put a space between bytes."),
                         OptionSpec::integer("bytes_per_line", "Bytes per line", 8, 0, 64,
                                             "Start a new line after this many bytes; 0 writes a single line.")},
      .expansion = 9,
  });

  // ---- further encodings, one group per source file ---------------------------------------------
  encoding::register_txt(registry);
  encoding::register_base_n(registry);
  encoding::register_byte_lists(registry);
  encoding::register_mail_encodings(registry);
  encoding::register_array_literals(registry);
  encoding::register_html_entities(registry);
  encoding::register_text_transforms(registry);
  encoding::register_punycode(registry);
  encoding::register_morse_nato(registry);
  encoding::register_hashes(registry);
  encoding::register_unicode(registry);
  encoding::register_charsets(registry);
  encoding::register_more_bases(registry);
  encoding::register_firmware_hex(registry);
  encoding::register_text_tools(registry);
  encoding::register_byte_views(registry);
}

}  // namespace cvt
