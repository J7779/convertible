// Convertible core - subtitle family: EBU STL (EBU Tech 3264-E), binary.
//
// A 1024-byte General Subtitle Information block (code page, frame rate
// "STL25.01" / "STL30.01", character code table, language, titles, start of
// programme ...) followed by 128-byte Text and Timing Information blocks:
// subtitle number, extension block number, cumulative status, time code in /
// out (h, m, s, frames), vertical position, justification, comment flag and
// a 112-byte text field. The text field uses ISO 6937 (character code table
// 00, with non-spacing diacritic prefixes) or ISO 8859-5/6/7/8 (tables 01-04),
// 0x8A for line breaks, 0x8F as filler, 0x80/0x81 italics and 0x82/0x83
// underline; teletext control codes below 0x20 act as spaces.
//
// Reading subtracts the start-of-programme time code (often 10:00:00:00)
// when every subtitle starts after it (option stl_subtract_tcp). Writing
// produces a level-1 teletext file with deterministic GSI fields: no current
// dates, the subtitle count and the first time code filled in.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/subtitle/stl_iso6937.hpp"
#include "families/subtitle/subtitle.hpp"
#include "util/text.hpp"

namespace cvt::subtitle {

namespace {

constexpr std::size_t gsi_size = 1024;
constexpr std::size_t tti_size = 128;
constexpr std::size_t text_size = 112;

std::string_view field(ByteView data, std::size_t offset, std::size_t length) {
  return as_chars(data.subspan(offset, length));
}

/// Language codes of EBU Tech 3264 appendix 3 (the common ones).
constexpr std::array<std::pair<std::string_view, std::string_view>, 24> languages = {{
    {"01", "sq"}, {"06", "cs"}, {"07", "da"}, {"08", "de"}, {"09", "en"}, {"0A", "es"}, {"0B", "eo"}, {"0C", "et"},
    {"0D", "eu"}, {"0E", "fo"}, {"0F", "fr"}, {"10", "fy"}, {"11", "ga"}, {"12", "gd"}, {"13", "gl"}, {"14", "is"},
    {"15", "it"}, {"18", "lv"}, {"1D", "nl"}, {"1E", "no"}, {"1F", "pl"}, {"20", "pt"}, {"21", "ro"}, {"27", "sv"},
}};

std::string language_tag(std::string_view code) {
  for (const auto& [ebu, tag] : languages) {
    if (util::iequals(ebu, code)) return std::string(tag);
  }
  if (util::iequals(code, "56")) return "ru";
  if (util::iequals(code, "57")) return "pa";
  if (util::iequals(code, "45")) return "zh";
  if (util::iequals(code, "69")) return "ja";
  if (util::iequals(code, "65")) return "ko";
  if (util::iequals(code, "55")) return "el";  // listed as "Greek" in later revisions
  return {};
}

std::string language_code(std::string_view tag) {
  const std::string primary = util::to_lower(tag.substr(0, tag.find('-')));
  for (const auto& [ebu, known] : languages) {
    if (known == primary) return std::string(ebu);
  }
  if (primary == "ru") return "56";
  return "00";
}

// ---------------------------------------------------------------------------- text

/// Decode one text field (possibly the concatenation of extension blocks).
std::string decode_text_field(ByteView bytes, std::string_view table) {
  std::string out;
  bool italic = false;
  bool underline = false;
  std::optional<std::uint8_t> diacritic;
  std::string_view page;
  if (table == "01") page = "iso-8859-5";
  else if (table == "02") page = "iso-8859-6";
  else if (table == "03") page = "iso-8859-7";
  else if (table == "04") page = "iso-8859-8";
  for (const std::byte raw : bytes) {
    const auto b = std::to_integer<std::uint8_t>(raw);
    if (b == 0x8F) continue;  // unused space
    if (b == 0x8A) {
      out.push_back('\n');
      continue;
    }
    if (b >= 0x80 && b <= 0x85) {
      if (b == 0x80 || b == 0x81) {
        const bool on = b == 0x80;
        if (on != italic) out += on ? "<i>" : "</i>";
        italic = on;
      } else if (b == 0x82 || b == 0x83) {
        const bool on = b == 0x82;
        if (on != underline) out += on ? "<u>" : "</u>";
        underline = on;
      }
      continue;
    }
    if (b < 0x20 || (b >= 0x86 && b < 0xA0)) {
      out.push_back(' ');  // teletext spacing attributes
      continue;
    }
    if (!page.empty()) {
      if (b < 0x80) out.push_back(static_cast<char>(b));
      else if (const auto decoded = decode_codepage(ByteView(&raw, 1), page)) out += *decoded;
      continue;
    }
    // ISO 6937.
    if (b >= 0xC1 && b <= 0xCF) {
      diacritic = b;
      continue;
    }
    if (diacritic) {
      char32_t composed = 0;
      for (const iso6937::Combination& combo : iso6937::combinations) {
        if (combo.diacritic == *diacritic && static_cast<std::uint8_t>(combo.base) == b) composed = combo.code_point;
      }
      diacritic.reset();
      if (composed != 0) {
        util::append_utf8(out, composed);
        continue;
      }
    }
    if (b < 0x80) {
      out.push_back(b == 0x24 ? '$' : static_cast<char>(b));
      continue;
    }
    char32_t mapped = 0;
    for (const iso6937::Spacing& spacing : iso6937::spacing) {
      if (spacing.byte == b) mapped = spacing.code_point;
    }
    if (b == 0xA0) mapped = 0xA0;
    if (mapped != 0) util::append_utf8(out, mapped);
  }
  if (italic) out += "</i>";
  if (underline) out += "</u>";
  // Tidy teletext layout: trim lines, drop empty ones (double-height spacing).
  std::vector<std::string> lines;
  for (const std::string_view line : util::split(out, '\n')) {
    const std::string_view trimmed = util::trim(line);
    if (!trimmed.empty() && !is_blank_text(trimmed)) lines.emplace_back(trimmed);
  }
  return balance_markup(util::join(lines, "\n"));
}

/// Encode IR text into ISO 6937 with control codes; false characters become '?'.
Bytes encode_text_field(std::string_view ir_text, bool& unmappable) {
  Bytes out;
  const auto push = [&out](std::uint8_t b) { out.push_back(static_cast<std::byte>(b)); };
  for (const MarkupToken& token : tokenize_markup(ir_text)) {
    switch (token.type) {
      case MarkupToken::Type::LineBreak: push(0x8A); break;
      case MarkupToken::Type::Open:
      case MarkupToken::Type::Close:
        if (token.text == "i") push(token.type == MarkupToken::Type::Open ? 0x80 : 0x81);
        if (token.text == "u") push(token.type == MarkupToken::Type::Open ? 0x82 : 0x83);
        break;
      case MarkupToken::Type::Text:
        for (const char32_t c : util::utf8_to_utf32(token.text)) {
          if (c >= 0x20 && c < 0x7F) {
            push(static_cast<std::uint8_t>(c));
            continue;
          }
          bool done = false;
          for (const iso6937::Spacing& spacing : iso6937::spacing) {
            if (spacing.code_point == c) {
              push(spacing.byte);
              done = true;
              break;
            }
          }
          for (const iso6937::Combination& combo : iso6937::combinations) {
            if (done) break;
            if (combo.code_point == c) {
              push(combo.diacritic);
              push(static_cast<std::uint8_t>(combo.base));
              done = true;
            }
          }
          if (!done && c == 0xA0) {
            push(0xA0);
            done = true;
          }
          if (!done) {
            unmappable = true;
            push('?');
          }
        }
        break;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------- reading

std::optional<double> frame_rate(std::string_view dfc) {
  if (!dfc.starts_with("STL") || dfc.size() < 5) return std::nullopt;
  const auto rate = util::parse_int(dfc.substr(3, 2));
  if (!rate || *rate < 20 || *rate > 60) return std::nullopt;
  return *rate == 29 ? 30000.0 / 1001.0 : *rate == 23 ? 24000.0 / 1001.0 : static_cast<double>(*rate);
}

std::int64_t timecode_ms(ByteView tc, double fps) {
  const auto at = [&](std::size_t i) { return static_cast<std::int64_t>(std::to_integer<std::uint8_t>(tc[i])); };
  return ((at(0) * 60 + at(1)) * 60 + at(2)) * 1000 + frames_to_ms(at(3), fps);
}

Result<AnyIR> read_stl(ReadContext& ctx) {
  const ByteView input = ctx.input;
  if (input.size() < gsi_size) return fail(Status::Decode, "EBU STL file is shorter than its 1024-byte header");
  const auto fps = frame_rate(field(input, 3, 8));
  if (!fps) return fail(Status::Decode, "not an EBU STL file (disk format code '{}')", util::sanitize_utf8(field(input, 3, 8)));
  const std::string_view code_page = field(input, 0, 3);
  const std::string_view table = field(input, 12, 2);
  const std::string gsi_page = code_page == "437" ? "cp437" : "cp850";
  ir::Subtitles subs;
  const auto gsi_text = [&](std::size_t offset, std::size_t length) {
    return std::string(util::trim(decode_codepage(input.subspan(offset, length), gsi_page).value_or(std::string())));
  };
  if (std::string title = gsi_text(16, 32); !title.empty()) subs.metadata["title"] = title;
  if (std::string episode = gsi_text(48, 32); !episode.empty()) subs.metadata["episode"] = episode;
  if (std::string language = language_tag(field(input, 14, 2)); !language.empty()) subs.metadata["language"] = language;
  subs.metadata["fps"] = util::format_double(*fps);

  // Start of programme ("HHMMSSFF").
  std::optional<std::int64_t> programme_start;
  {
    const std::string_view tcp = field(input, 256, 8);
    if (std::ranges::all_of(tcp, [](char c) { return util::is_ascii_digit(c); })) {
      const auto two = [&](std::size_t i) { return static_cast<std::int64_t>((tcp[i] - '0') * 10 + (tcp[i + 1] - '0')); };
      programme_start = ((two(0) * 60 + two(2)) * 60 + two(4)) * 1000 + frames_to_ms(two(6), *fps);
    }
  }
  const std::uint64_t blocks = (input.size() - gsi_size) / tti_size;
  CVT_TRY(ctx.limits.check_archive_entries(blocks));
  std::string pending_text_bytes;
  std::optional<ir::Cue> pending;
  Bytes text_field;
  for (std::uint64_t n = 0; n < blocks; ++n) {
    if ((n & 1023) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(n) / static_cast<double>(blocks)));
    const ByteView block = input.subspan(gsi_size + static_cast<std::size_t>(n) * tti_size, tti_size);
    const auto ebn = std::to_integer<std::uint8_t>(block[3]);
    const auto comment = std::to_integer<std::uint8_t>(block[15]);
    if (ebn == 0xFE || ebn == 0xEF) continue;  // user data
    if (!pending) {
      ir::Cue cue;
      cue.start_ms = timecode_ms(block.subspan(5, 4), *fps);
      cue.end_ms = timecode_ms(block.subspan(9, 4), *fps);
      const auto vp = std::to_integer<std::uint8_t>(block[13]);
      const auto jc = std::to_integer<std::uint8_t>(block[14]);
      const int row = vp > 0 && vp < 12 ? 6 : 0;
      const int column = jc == 1 ? 1 : jc == 3 ? 3 : 2;
      if (row != 0 || column != 2) {
        ir::CuePosition position;
        position.alignment = row + column;
        cue.position = position;
      }
      cue.attributes["ebu-stl.vp"] = std::to_string(vp);
      pending = std::move(cue);
      text_field.clear();
    }
    const ByteView text = block.subspan(16, text_size);
    text_field.insert(text_field.end(), text.begin(), text.end());
    if (ebn != 0xFF && text_field.size() < 256 * text_size) continue;  // more extension blocks follow
    pending->text = decode_text_field(text_field, table);
    if (comment == 0) subs.cues.push_back(std::move(*pending));
    pending.reset();
  }
  if (programme_start && *programme_start > 0 && ctx.options.get_bool("stl_subtract_tcp", true) && !subs.cues.empty() &&
      std::ranges::all_of(subs.cues, [&](const ir::Cue& cue) { return cue.start_ms >= *programme_start; })) {
    for (ir::Cue& cue : subs.cues) {
      cue.start_ms -= *programme_start;
      cue.end_ms -= *programme_start;
    }
  }
  return finish_read(std::move(subs), ctx);
}

// ---------------------------------------------------------------------------- writing

void put_field(Bytes& out, std::size_t offset, std::size_t length, std::string_view value) {
  for (std::size_t i = 0; i < length; ++i) {
    out[offset + i] = static_cast<std::byte>(i < value.size() ? static_cast<std::uint8_t>(value[i]) : ' ');
  }
}

/// GSI text in code page 850 (ASCII subset; other characters become '?').
std::string ascii_field(std::string_view text) {
  std::string out;
  for (const char32_t c : util::utf8_to_utf32(text)) out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
  return out;
}

Result<Bytes> write_stl(const AnyIR& value, WriteContext& ctx) {
  const ir::Subtitles& subs = value.get<ir::Subtitles>();
  const std::int64_t requested = ctx.options.get_int("stl_fps", 25, 24, 30);
  const int fps_whole = requested >= 28 ? 30 : 25;
  const double fps = fps_whole;
  bool unmappable = false;
  // Text and Timing Information blocks first, to know the block count.
  std::vector<Bytes> blocks;
  const auto timecode = [&](std::int64_t ms, Bytes& block, std::size_t offset) {
    ms = std::max<std::int64_t>(ms, 0);
    std::int64_t seconds = ms / 1000;
    std::int64_t frames = ms_to_frames(ms - seconds * 1000, fps);
    if (frames >= fps_whole) {
      frames = 0;
      ++seconds;
    }
    block[offset] = static_cast<std::byte>(std::min<std::int64_t>(seconds / 3600, 99));
    block[offset + 1] = static_cast<std::byte>((seconds / 60) % 60);
    block[offset + 2] = static_cast<std::byte>(seconds % 60);
    block[offset + 3] = static_cast<std::byte>(frames);
  };
  std::size_t subtitle_number = 0;
  for (const ir::Cue& cue : subs.cues) {
    const Bytes text = encode_text_field(cue.text, unmappable);
    const std::size_t parts = std::max<std::size_t>(1, (text.size() + text_size - 1) / text_size);
    if (parts > 0xEF) return fail(Status::Encode, "a subtitle's text is too long for EBU STL");
    const std::size_t lines = static_cast<std::size_t>(std::ranges::count(cue.text, '\n')) + 1;
    const int alignment = cue.position ? std::clamp(cue.position->alignment, 1, 9) : 2;
    const auto vp = static_cast<std::uint8_t>(alignment >= 7 ? 1 : std::max<int>(1, 22 - 2 * static_cast<int>(lines - 1)));
    const int column = (alignment - 1) % 3;
    for (std::size_t part = 0; part < parts; ++part) {
      Bytes block(tti_size, std::byte{0x8F});
      block[0] = std::byte{0};  // subtitle group
      block[1] = static_cast<std::byte>(subtitle_number & 0xFF);
      block[2] = static_cast<std::byte>((subtitle_number >> 8) & 0xFF);
      block[3] = static_cast<std::byte>(part + 1 == parts ? 0xFF : part);
      block[4] = std::byte{0};  // not cumulative
      timecode(cue.start_ms, block, 5);
      timecode(cue.end_ms, block, 9);
      block[13] = static_cast<std::byte>(vp);
      block[14] = static_cast<std::byte>(column == 0 ? 1 : column == 2 ? 3 : 2);
      block[15] = std::byte{0};
      for (std::size_t i = 0; i < text_size && part * text_size + i < text.size(); ++i) {
        block[16 + i] = text[part * text_size + i];
      }
      blocks.push_back(std::move(block));
    }
    ++subtitle_number;
  }
  if (subtitle_number > 0xFFFF) return fail(Status::Encode, "EBU STL holds at most 65535 subtitles");

  Bytes out(gsi_size, std::byte{' '});
  put_field(out, 0, 3, "850");
  put_field(out, 3, 8, fps_whole == 30 ? "STL30.01" : "STL25.01");
  put_field(out, 11, 1, "1");   // level-1 teletext
  put_field(out, 12, 2, "00");  // ISO 6937
  const auto language = subs.metadata.find("language");
  put_field(out, 14, 2, language == subs.metadata.end() ? std::string("00") : language_code(language->second));
  const auto title = subs.metadata.find("title");
  put_field(out, 16, 32, title == subs.metadata.end() ? std::string() : ascii_field(title->second));
  put_field(out, 224, 6, "000101");  // creation date: fixed, never the current date
  put_field(out, 230, 6, "000101");
  put_field(out, 236, 2, "00");
  put_field(out, 238, 5, std::format("{:05}", std::min<std::size_t>(blocks.size(), 99999)));
  put_field(out, 243, 5, std::format("{:05}", std::min<std::size_t>(subtitle_number, 99999)));
  put_field(out, 248, 3, "001");
  put_field(out, 251, 2, "40");
  put_field(out, 253, 2, "23");
  put_field(out, 255, 1, "1");
  put_field(out, 256, 8, "00000000");
  std::string first = "00000000";
  if (!subs.cues.empty()) {
    Bytes tmp(4);
    timecode(subs.cues.front().start_ms, tmp, 0);
    first = std::format("{:02}{:02}{:02}{:02}", std::to_integer<int>(tmp[0]), std::to_integer<int>(tmp[1]),
                        std::to_integer<int>(tmp[2]), std::to_integer<int>(tmp[3]));
  }
  put_field(out, 264, 8, first);
  put_field(out, 272, 1, "1");
  put_field(out, 273, 1, "1");
  for (const Bytes& block : blocks) out.insert(out.end(), block.begin(), block.end());
  if (unmappable) ctx.warn("Some characters have no ISO 6937 equivalent and were written as '?'.");
  return out;
}

int sniff_stl(ByteView data, std::string_view /*filename*/) {
  if (data.size() < 16) return 0;
  const std::string_view head = as_chars(data.first(16));
  const bool page = head.starts_with("437") || head.starts_with("850") || head.starts_with("860") ||
                    head.starts_with("863") || head.starts_with("865");
  if (!page || !head.substr(3).starts_with("STL") || !frame_rate(head.substr(3, 8))) return 0;
  return 97;
}

}  // namespace

void register_ebu_stl(Registry& registry) {
  FormatInfo info = subtitle_format("ebu-stl", "EBU STL subtitles", "EBU Tech 3264 binary subtitle exchange files.",
                                    {"stl"}, "application/x-ebu-stl", 20);
  info.sniff = sniff_stl;
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = "ebu-stl";
  codec.kind = Kind::Subtitles;
  codec.read = read_stl;
  codec.write = write_stl;
  codec.read_options = common_read_options();
  codec.read_options.push_back(OptionSpec::boolean("stl_subtract_tcp", "Start at programme start", true,
                                                   "Subtract the start-of-programme time code (often 10:00:00:00) "
                                                   "from all times.")
                                   .advanced());
  codec.write_options = {OptionSpec::integer("stl_fps", "Frame rate", 25, 24, 30,
                                             "Time-code frame rate of the file: 25 (PAL) or 30 (NTSC); other values "
                                             "use the nearer of the two.")};
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::subtitle
