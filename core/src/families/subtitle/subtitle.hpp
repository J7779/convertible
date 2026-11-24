// Convertible core - subtitle family: internal declarations shared by its sources.
//
// Every reader produces an ir::Subtitles and then calls `finish_read`, which
// sorts the cues and applies the processing options shared by all readers
// (`common_read_options`): time shift, frame-rate retiming, duration and
// overlap repair, formatting removal and line re-flow. Because they are read
// options, they apply on every route, including the same-format "clean up"
// conversion (srt -> srt).
//
// Cue text uses the IR's inline markup (<i>, <b>, <u> only). The helpers
// below convert between that and format-specific markup.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "engine/options.hpp"
#include "engine/registry.hpp"
#include "ir/subtitles.hpp"
#include "util/bytes.hpp"

namespace cvt::subtitle {

// -- registration (one function per source file) --------------------------------------

void register_text_formats(Registry& registry);  // text_formats.cpp: srt, vtt, sbv, subviewer, microdvd, mpl2, tmplayer, qttext, spruce-stl, lrc
void register_ass(Registry& registry);           // ass.cpp: ass, ssa
void register_xml_formats(Registry& registry);   // xml_formats.cpp: ttml, dfxp, ebu-tt, smpte-tt, sami, srv3
void register_scc(Registry& registry);           // scc.cpp: Scenarist SCC (CEA-608)
void register_ebu_stl(Registry& registry);       // ebu_stl.cpp: EBU Tech 3264 binary STL
void register_youtube(Registry& registry);       // youtube.cpp: YouTube json3 transcripts
void register_bridges(Registry& registry);       // bridges.cpp: Subtitles -> Table / Data / PlainText / Document

// -- reading -----------------------------------------------------------------------------

/// Read options shared by every subtitle reader: source_encoding, offset_ms,
/// fps_from, fps_to, min_duration_ms, max_duration_ms, fix_overlaps,
/// strip_formatting, max_line_length, merge_lines, drop_empty.
[[nodiscard]] std::vector<OptionSpec> common_read_options();

/// Decode the input bytes to UTF-8 text (BOM removed) honouring the
/// `source_encoding` option: "auto" detects UTF-8/16/32 by BOM and content,
/// then picks the most plausible legacy single-byte code page.
[[nodiscard]] Result<std::string> decode_input(const ReadContext& ctx);
/// As above for explicit bytes and option value.
[[nodiscard]] Result<std::string> decode_bytes(ByteView data, std::string_view encoding);
/// Decode bytes of a known single-byte code page ("windows-1251", "cp850",
/// "iso-8859-7", ...). nullopt when the name is not known.
[[nodiscard]] std::optional<std::string> decode_codepage(ByteView data, std::string_view name);
/// Best legacy code page for non-UTF-8 bytes (one of the names above or
/// "windows-1252").
[[nodiscard]] std::string guess_codepage(ByteView data);

/// Sort the cues, drop empty ones (option) and apply the processing options.
/// Every reader ends with `return finish_read(std::move(subs), ctx);`.
[[nodiscard]] Result<AnyIR> finish_read(ir::Subtitles subs, const ReadContext& ctx);

/// Signature of a text-format parser: UTF-8 text in, cues out.
using TextParser = std::function<Result<ir::Subtitles>(std::string_view text, const ReadContext& ctx)>;
/// Signature of a text-format serialiser.
using TextWriter = std::function<Result<std::string>(const ir::Subtitles& subs, const WriteContext& ctx)>;

/// Writer options shared by text writers: line_ending ("lf" | "crlf"), bom.
[[nodiscard]] std::vector<OptionSpec> common_text_write_options(std::string_view default_line_ending = "lf");
/// Apply line_ending and bom to serialised text (which uses '\n').
[[nodiscard]] Bytes finish_text(std::string text, const WriteContext& ctx);

/// A Subtitles codec for a text format: decodes the input (decode_input),
/// parses, finishes (finish_read); writes and applies finish_text.
/// `parse` or `write` may be null.
[[nodiscard]] Codec make_text_codec(std::string format_id, TextParser parse, TextWriter write,
                                    std::vector<OptionSpec> extra_read_options = {},
                                    std::vector<OptionSpec> extra_write_options = {},
                                    std::string_view default_line_ending = "lf");

/// A FormatInfo with category Subtitle.
[[nodiscard]] FormatInfo subtitle_format(std::string id, std::string name, std::string description,
                                         std::vector<std::string> extensions, std::string mime, int popularity);

// -- time ------------------------------------------------------------------------------------

/// Parse clock times: "01:02:03,456", "1:02:03.45", "02:03.456", "01:02:03:456",
/// " 1 : 2 : 3 , 4 " (spaces tolerated), "-00:00:01,000". Fractions are
/// scaled by their number of digits ("5" = 500 ms, "45" = 450 ms, "4567" = 456 ms).
/// With `minutes_first` = true a two-part value is mm:ss (else it is hh:mm).
[[nodiscard]] std::optional<std::int64_t> parse_clock(std::string_view text, bool minutes_first = true);

struct ClockStyle {
  int hour_digits = 2;       ///< minimum digits of the hour field; 0 = omit hours when zero
  char fraction_separator = ',';
  int fraction_digits = 3;   ///< 3 = milliseconds, 2 = centiseconds, 0 = none
};
/// "01:02:03,456". Negative values are clamped to 0.
[[nodiscard]] std::string format_clock(std::int64_t ms, const ClockStyle& style = {});

/// Round a millisecond value to the nearest frame at `fps` and back.
[[nodiscard]] std::int64_t ms_to_frames(std::int64_t ms, double fps);
[[nodiscard]] std::int64_t frames_to_ms(std::int64_t frames, double fps);

// -- inline markup ------------------------------------------------------------------------------

/// One piece of IR cue text.
struct MarkupToken {
  enum class Type { Text, Open, Close, LineBreak };
  Type type = Type::Text;
  std::string text;  ///< Text: the characters (no '\n'); Open/Close: "i", "b" or "u"
};
/// Split IR cue text into text runs, <i>/<b>/<u> open/close tags and line breaks.
[[nodiscard]] std::vector<MarkupToken> tokenize_markup(std::string_view ir_text);

/// HTML-like markup from SRT/WebVTT/SAMI/... to IR text: keeps <i>, <b>,
/// <u> (also <em>, <strong>, <I> ...), drops every other tag (<font>, <c.x>,
/// <v Name>, <ruby>, timestamps ...), decodes character references, turns
/// <br> into '\n', and repairs nesting (unclosed tags are closed at the end,
/// stray closers dropped).
[[nodiscard]] std::string html_to_ir(std::string_view text);
/// IR text with the <i>/<b>/<u> tags removed.
[[nodiscard]] std::string strip_markup(std::string_view ir_text);
/// IR text for HTML-like formats: text escaped (&, <, >), tags kept.
[[nodiscard]] std::string ir_to_html(std::string_view ir_text);
/// Repair tag balance of IR text (close what is open at the end, drop closers
/// of tags that are not open).
[[nodiscard]] std::string balance_markup(std::string_view ir_text);

// -- small text helpers ------------------------------------------------------------------------

/// Lines of `text` split on LF/CRLF/CR, with trailing white space removed.
[[nodiscard]] std::vector<std::string_view> text_lines(std::string_view text);
/// True when the cue text has no visible characters after removing markup.
[[nodiscard]] bool is_blank_text(std::string_view ir_text);
/// First non-empty, trimmed lines of UTF-8 text decoded leniently from
/// `data`, for sniffers (handles BOMs and UTF-16).
[[nodiscard]] std::string sniff_prefix(ByteView data, std::size_t limit = 2048);

}  // namespace cvt::subtitle
