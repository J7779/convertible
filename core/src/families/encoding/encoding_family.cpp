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
//  2. Plain text (`txt`), read into and written from ir::PlainText with a
//     choice of character encoding, byte order mark and line endings, plus
//     the PlainText -> Document bridge that lets every document writer accept
//     plain text.
//
// This file is also the reference for how a family registers itself; see
// core/README.md for the walkthrough.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  const std::string separator = ctx.options.get_enum("separator", "none", {"none", "space", "colon"});
  style.separator = separator == "space" ? " "sv : separator == "colon" ? ":"sv : ""sv;
  return encoding::hex_text(blob.bytes, style);
}

Result<std::string> encode_data_uri(const ir::Blob& blob, const WriteContext& ctx) {
  std::string mime = ctx.options.get_string("mime");
  if (mime.empty()) mime = blob.mime.empty() ? "application/octet-stream" : blob.mime;
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

// --------------------------------------------------------------------------- plain text

const std::vector<std::string>& encoding_choices() {
  static const std::vector<std::string> choices = {"utf-8",    "utf-16le", "utf-16be",    "utf-32le",
                                                    "utf-32be", "latin1",   "windows-1252"};
  return choices;
}

Result<AnyIR> read_txt(ReadContext& ctx) {
  const std::string requested = ctx.options.get_string("source_encoding", "auto");
  util::TextEncoding encoding = util::TextEncoding::Utf8;
  bool had_bom = false;
  if (requested == "auto") {
    const util::EncodingGuess guess = util::guess_encoding(ctx.input);
    encoding = guess.encoding;
    had_bom = guess.bom_length > 0;
    if (!guess.confident) {
      ctx.warn("The text is not valid Unicode; it was read as Windows-1252. Set 'source_encoding' if that is wrong.");
    }
  } else {
    const auto parsed = util::parse_encoding(requested);
    if (!parsed) return fail(Status::InvalidArgument, "unknown source encoding '{}'", requested);
    encoding = *parsed;
    const auto bom = util::detect_bom(ctx.input);
    had_bom = bom && bom->encoding == encoding;
  }
  CVT_TRY(ctx.limits.check_decompressed(static_cast<std::uint64_t>(ctx.input.size()) * 3));
  util::DecodedText decoded = util::decode_text(ctx.input, encoding);
  if (decoded.replaced > 0) {
    ctx.warn(std::to_string(decoded.replaced) + " malformed byte sequence(s) were replaced with U+FFFD.");
  }
  ir::PlainText text;
  text.text = std::move(decoded.text);
  text.source_encoding = std::string(util::encoding_name(encoding));
  text.had_bom = had_bom;
  return text;
}

Result<Bytes> write_txt(const AnyIR& value, WriteContext& ctx) {
  const ir::PlainText& text = value.get<ir::PlainText>();
  const auto encoding = util::parse_encoding(ctx.options.get_string("encoding", "utf-8"));
  if (!encoding) return fail(Status::InvalidArgument, "unknown text encoding");

  const std::string line_endings = ctx.options.get_enum("line_endings", "keep", {"keep", "lf", "crlf", "cr"});
  std::string converted;
  std::string_view content = text.text;
  if (line_endings != "keep") {
    converted = util::convert_line_endings(
        text.text, line_endings == "lf" ? util::LineEnding::Lf
                                         : line_endings == "crlf" ? util::LineEnding::CrLf : util::LineEnding::Cr);
    content = converted;
  }
  CVT_TRY(ctx.limits.check_output(static_cast<std::uint64_t>(content.size()) * 4 + 4));
  util::EncodedText encoded = util::encode_text(content, *encoding, ctx.options.get_bool("bom", false));
  if (encoded.replaced > 0) {
    ctx.warn(std::to_string(encoded.replaced) + " character(s) cannot be represented in " +
             std::string(util::encoding_name(*encoding)) + " and were replaced with '?'.");
  }
  return std::move(encoded.bytes);
}

// Plain text becomes one paragraph per block of non-blank lines; single line
// breaks inside a block are kept as soft breaks.
Result<AnyIR> plain_text_to_document(AnyIR value, Context& ctx) {
  const ir::PlainText text = std::move(value).get<ir::PlainText>();
  ir::Document document;
  std::string paragraph;
  const auto flush = [&] {
    if (paragraph.empty()) return;
    document.blocks.push_back(ir::Paragraph{{ir::Text{std::move(paragraph)}}});
    paragraph.clear();
  };
  const std::vector<std::string_view> lines = util::split_lines(text.text);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if ((i & 0xFFF) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
    const std::string_view line = util::trim_right(lines[i]);
    if (util::trim(line).empty()) {
      flush();
      continue;
    }
    if (!paragraph.empty()) paragraph.push_back('\n');
    paragraph.append(line);
  }
  flush();
  return document;
}

// --------------------------------------------------------------------------- detection

int sniff_txt(ByteView data, std::string_view /*filename*/) { return util::looks_like_text(data) ? 60 : 0; }

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
  return text[9] == ' ' ? 88 : 0;
}

int sniff_checksums(ByteView data, std::string_view /*filename*/) {
  const std::string_view text = as_chars(data).substr(0, 1024);
  return text.find("\nCRC-32: ") != std::string_view::npos && text.find("\nSHA-256: ") != std::string_view::npos ? 85 : 0;
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
                         OptionSpec::choice("separator", "Separator", "none", {"none", "space", "colon"},
                                            "Character placed between bytes.")},
      .expansion = 4,
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

  // ---- checksums ------------------------------------------------------------------------
  add_binary_encoding(registry, {
      .id = "checksums", .name = "Checksum report",
      .description = "CRC-32, Adler-32, MD5, SHA-1 and SHA-256 of the file.",
      .extensions = {"txt", "checksums"}, .popularity = 55,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        CVT_TRY(ctx.checkpoint(0.0, "Hashing"));
        return encoding::checksum_report(blob.bytes, blob.filename, ctx.options.get_bool("uppercase", false));
      },
      .encode_options = {uppercase_option()},
      .expansion = 0,  // fixed-size output
      .sniff = sniff_checksums,
  });

  // ---- plain text ---------------------------------------------------------------------------
  FormatInfo txt;
  txt.id = "txt";
  txt.name = "Plain text";
  txt.description = "Unformatted text in a choice of character encodings.";
  txt.extensions = {"txt", "text", "log"};
  txt.mime = "text/plain";
  txt.popularity = 90;
  txt.category = Category::Document;
  txt.sniff = sniff_txt;
  registry.add_format(std::move(txt));

  std::vector<std::string> source_encodings = {"auto"};
  source_encodings.insert(source_encodings.end(), encoding_choices().begin(), encoding_choices().end());
  Codec text_codec;
  text_codec.format_id = "txt";
  text_codec.kind = Kind::PlainText;
  text_codec.read = read_txt;
  text_codec.write = write_txt;
  text_codec.read_options = {OptionSpec::choice(
      "source_encoding", "Source encoding", "auto", std::move(source_encodings),
      "Character encoding of the input; 'auto' detects Unicode and falls back to Windows-1252.")};
  text_codec.write_options = {
      OptionSpec::choice("encoding", "Encoding", "utf-8", encoding_choices(), "Character encoding of the output."),
      OptionSpec::boolean("bom", "Byte order mark", false, "Start the output with a Unicode byte order mark."),
      OptionSpec::choice("line_endings", "Line endings", "keep", {"keep", "lf", "crlf", "cr"},
                         "Convert line endings: lf (Unix), crlf (Windows), cr (classic Mac), or keep them as they are."),
  };
  registry.add_codec(std::move(text_codec));

  // ---- bridges --------------------------------------------------------------------------------
  Bridge to_document;
  to_document.from = Kind::PlainText;
  to_document.to = Kind::Document;
  to_document.fn = plain_text_to_document;
  to_document.cost = 15;
  to_document.label = "split plain text into paragraphs";
  registry.add_bridge(std::move(to_document));

  // ---- further encodings, one group per source file ---------------------------------------------
  encoding::register_base_n(registry);
  encoding::register_byte_lists(registry);
  encoding::register_mail_encodings(registry);
  encoding::register_array_literals(registry);
  encoding::register_html_entities(registry);
  encoding::register_text_transforms(registry);
  encoding::register_punycode(registry);
  encoding::register_morse_nato(registry);
}

}  // namespace cvt
