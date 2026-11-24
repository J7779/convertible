// Convertible core - encoding family: plain text (`txt`) with a choice of
// character encodings, byte order mark and line endings, plus the
// PlainText -> Document bridge that lets every document writer accept text.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/encoding/charsets.hpp"
#include "families/encoding/encoding.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

// --------------------------------------------------------------------------- plain text

// The Unicode encodings and Latin-1/Windows-1252 come from util/text.hpp,
// every other charset from charsets.cpp.
const std::vector<std::string>& encoding_choices() {
  static const std::vector<std::string> choices = [] {
    std::vector<std::string> list = {"utf-8", "utf-16le", "utf-16be", "utf-32le", "utf-32be", "latin1", "windows-1252"};
    for (const CharsetInfo& info : legacy_charsets()) {
      if (info.id != "windows-1252") list.emplace_back(info.id);
    }
    return list;
  }();
  return choices;
}

/// Decodes with util for the Unicode encodings, with charsets.cpp otherwise.
Result<util::DecodedText> decode_with(ByteView input, std::string_view label, std::string& canonical) {
  if (const auto parsed = util::parse_encoding(label)) {
    canonical = std::string(util::encoding_name(*parsed));
    return util::decode_text(input, *parsed);
  }
  if (const CharsetInfo* charset = find_charset(label)) {
    canonical = std::string(charset->id);
    CharsetText decoded = charset_decode(input, *charset);
    return util::DecodedText{std::move(decoded.text), decoded.replaced};
  }
  return fail(Status::InvalidArgument, "unknown source encoding '{}'", label);
}

Result<AnyIR> read_txt(ReadContext& ctx) {
  std::string requested = ctx.options.get_string("source_encoding", "auto");
  bool had_bom = false;
  CVT_TRY(ctx.limits.check_decompressed(static_cast<std::uint64_t>(ctx.input.size()) * 3));
  if (requested == "auto") {
    const util::EncodingGuess guess = util::guess_encoding(ctx.input);
    had_bom = guess.bom_length > 0;
    requested = std::string(util::encoding_name(guess.encoding));
    if (!guess.confident) {
      // Not Unicode: rank the legacy charsets; a clear winner (Shift_JIS, GBK,
      // KOI8-R ...) is used, otherwise Windows-1252 as before.
      const std::vector<CharsetGuess> guesses = detect_charsets(ctx.input);
      if (!guesses.empty() && guesses.front().confidence >= 50 && guesses.front().id != "utf-8" &&
          guesses.front().id != "windows-1252") {
        requested = guesses.front().id;
        ctx.warn("The text is not Unicode; it was read as " + requested + " (detected with " +
                 std::to_string(guesses.front().confidence) + "% confidence). Set 'source_encoding' if that is wrong.");
      } else {
        ctx.warn("The text is not valid Unicode; it was read as Windows-1252. Set 'source_encoding' if that is wrong.");
      }
    }
  } else if (const auto parsed = util::parse_encoding(requested)) {
    const auto bom = util::detect_bom(ctx.input);
    had_bom = bom && bom->encoding == *parsed;
  }
  std::string canonical;
  CVT_TRY_ASSIGN(util::DecodedText decoded, decode_with(ctx.input, requested, canonical));
  if (decoded.replaced > 0) {
    ctx.warn(std::to_string(decoded.replaced) + " malformed byte sequence(s) were replaced with U+FFFD.");
  }
  ir::PlainText text;
  text.text = std::move(decoded.text);
  text.source_encoding = std::move(canonical);
  text.had_bom = had_bom;
  return text;
}

Result<Bytes> write_txt(const AnyIR& value, WriteContext& ctx) {
  const ir::PlainText& text = value.get<ir::PlainText>();
  const std::string label = ctx.options.get_string("encoding", "utf-8");
  const auto encoding = util::parse_encoding(label);
  const CharsetInfo* charset = encoding ? nullptr : find_charset(label);
  if (!encoding && charset == nullptr) return fail(Status::InvalidArgument, "unknown text encoding '{}'", label);

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
  if (charset != nullptr) {
    if (ctx.options.get_bool("bom", false)) ctx.warn("A byte order mark exists only for Unicode encodings; none was written.");
    CharsetBytes encoded = charset_encode(content, *charset);
    if (encoded.replaced > 0) {
      ctx.warn(std::to_string(encoded.replaced) + " character(s) cannot be represented in " + std::string(charset->id) +
               " and were replaced with '?'.");
    }
    return std::move(encoded.bytes);
  }
  // The Unicode encodings (and Latin-1 / Windows-1252) are stateless: the
  // text is encoded in pieces cut at character boundaries, so that progress
  // is reported and a cancellation is seen about every MiB.
  util::EncodedText encoded;
  constexpr std::size_t piece = std::size_t{1} << 20;
  bool bom = ctx.options.get_bool("bom", false);
  for (std::size_t at = 0; at < content.size() || (at == 0 && content.empty());) {
    CVT_TRY(ctx.checkpoint(content.empty() ? 0.0 : static_cast<double>(at) / static_cast<double>(content.size())));
    std::size_t end = std::min(content.size(), at + piece);
    while (end < content.size() && (static_cast<unsigned char>(content[end]) & 0xC0u) == 0x80u) ++end;  // UTF-8 continuation
    util::EncodedText part = util::encode_text(content.substr(at, end - at), *encoding, bom);
    bom = false;
    encoded.replaced += part.replaced;
    if (encoded.bytes.empty()) {
      encoded.bytes = std::move(part.bytes);
    } else {
      append(encoded.bytes, ByteView(part.bytes));
    }
    if (content.empty()) break;
    at = end;
  }
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

int sniff_txt(ByteView data, std::string_view /*filename*/) { return util::looks_like_text(data) ? 60 : 0; }

}  // namespace

void register_txt(Registry& registry) {
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
      "Character encoding of the input; 'auto' detects Unicode, then the most likely legacy charset "
      "(Shift_JIS, GBK, Big5, EUC-KR, KOI8-R, Windows code pages ...), falling back to Windows-1252.")};
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
}

}  // namespace cvt::encoding
