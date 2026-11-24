// Convertible core - subtitle family: bridges from subtitles to other kinds.
//
//   Subtitles -> Table      one row per cue: start, end, duration, speaker, style, text
//   Subtitles -> Data       {"metadata": {...}, "cues": [{start_ms, end_ms, text, ...}]}
//   Subtitles -> PlainText  a transcript: cues merged into paragraphs by speaker and pauses,
//                           optionally with time stamps
//   Subtitles -> Document   the same transcript with speakers in bold and italics kept
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "families/subtitle/subtitle.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/plain_text.hpp"
#include "ir/table.hpp"
#include "util/text.hpp"

namespace cvt::subtitle {

namespace {

std::string clock(std::int64_t ms) { return format_clock(ms, ClockStyle{2, '.', 3}); }

Result<AnyIR> to_table(AnyIR value, Context& ctx) {
  const ir::Subtitles subs = std::move(value).get<ir::Subtitles>();
  const bool keep_markup = ctx.options.get_bool("table_markup", false);
  ir::Sheet sheet;
  sheet.name = "Subtitles";
  sheet.has_header = true;
  sheet.rows.push_back({ir::Cell::string("start"), ir::Cell::string("end"), ir::Cell::string("duration_ms"),
                        ir::Cell::string("speaker"), ir::Cell::string("style"), ir::Cell::string("text")});
  for (std::size_t i = 0; i < subs.cues.size(); ++i) {
    if ((i & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(subs.cues.size())));
    const ir::Cue& cue = subs.cues[i];
    const std::int64_t duration = cue.end_ms - cue.start_ms;
    sheet.rows.push_back({
        ir::Cell::string(clock(cue.start_ms)),
        ir::Cell::string(clock(cue.end_ms)),
        ir::Cell::numeric(static_cast<double>(duration), std::to_string(duration)),
        cue.speaker ? ir::Cell::string(*cue.speaker) : ir::Cell{},
        cue.style ? ir::Cell::string(*cue.style) : ir::Cell{},
        ir::Cell::string(keep_markup ? cue.text : strip_markup(cue.text)),
    });
  }
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

Result<AnyIR> to_data(AnyIR value, Context& ctx) {
  const ir::Subtitles subs = std::move(value).get<ir::Subtitles>();
  Json root = Json::object();
  Json metadata = Json::object();
  for (const auto& [key, text] : subs.metadata) {
    if (!key.starts_with("ass.style.")) metadata[key] = text;
  }
  root["metadata"] = std::move(metadata);
  Json cues = Json::array();
  for (std::size_t i = 0; i < subs.cues.size(); ++i) {
    if ((i & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(subs.cues.size())));
    const ir::Cue& cue = subs.cues[i];
    Json item = Json::object({{"start_ms", cue.start_ms}, {"end_ms", cue.end_ms}, {"text", cue.text}});
    if (cue.speaker) item["speaker"] = *cue.speaker;
    if (cue.style) item["style"] = *cue.style;
    if (cue.position) {
      Json position = Json::object({{"alignment", cue.position->alignment}});
      if (cue.position->x) position["x"] = *cue.position->x;
      if (cue.position->y) position["y"] = *cue.position->y;
      item["position"] = std::move(position);
    }
    cues.push_back(std::move(item));
  }
  root["cues"] = std::move(cues);
  return ir::Data{std::move(root)};
}

/// One transcript paragraph: consecutive cues of one speaker without a long pause.
struct Paragraph {
  std::int64_t start_ms = 0;
  std::optional<std::string> speaker;
  std::vector<std::string> pieces;  // IR text of each cue with its lines joined
};

std::vector<Paragraph> paragraphs(const ir::Subtitles& subs, std::int64_t gap_ms) {
  std::vector<Paragraph> out;
  std::int64_t last_end = 0;
  for (const ir::Cue& cue : subs.cues) {
    std::string text;
    for (const std::string_view line : util::split(cue.text, '\n', false)) {
      const std::string_view trimmed = util::trim(line);
      if (trimmed.empty()) continue;
      if (!text.empty()) text.push_back(' ');
      text.append(trimmed);
    }
    if (is_blank_text(text)) continue;
    const bool new_paragraph = out.empty() || cue.speaker != out.back().speaker ||
                               (gap_ms > 0 && cue.start_ms - last_end > gap_ms);
    if (new_paragraph) {
      Paragraph paragraph;
      paragraph.start_ms = cue.start_ms;
      paragraph.speaker = cue.speaker;
      out.push_back(std::move(paragraph));
    }
    // The same line repeated in consecutive cues (roll-up captions) is said once.
    if (out.back().pieces.empty() || out.back().pieces.back() != text) out.back().pieces.push_back(std::move(text));
    last_end = std::max(last_end, cue.end_ms);
  }
  return out;
}

std::string stamp(std::int64_t ms) { return format_clock(ms, ClockStyle{2, '.', 0}); }

Result<AnyIR> to_plain_text(AnyIR value, Context& ctx) {
  const ir::Subtitles subs = std::move(value).get<ir::Subtitles>();
  const bool timestamps = ctx.options.get_bool("transcript_timestamps", false);
  const std::int64_t gap = ctx.options.get_int("paragraph_gap_ms", 2000, 0, 3'600'000);
  ir::PlainText out;
  for (const Paragraph& paragraph : paragraphs(subs, gap)) {
    if (!out.text.empty()) out.text += "\n";
    if (timestamps) out.text += "[" + stamp(paragraph.start_ms) + "] ";
    if (paragraph.speaker && !paragraph.speaker->empty()) out.text += *paragraph.speaker + ": ";
    out.text += strip_markup(util::join(paragraph.pieces, " ")) + "\n";
  }
  return out;
}

/// IR cue markup to document inlines.
ir::Inlines markup_inlines(std::string_view ir_text) {
  ir::Inlines out;
  bool italic = false;
  bool bold = false;
  bool underline = false;
  for (const MarkupToken& token : tokenize_markup(ir_text)) {
    if (token.type == MarkupToken::Type::Open || token.type == MarkupToken::Type::Close) {
      const bool on = token.type == MarkupToken::Type::Open;
      if (token.text == "i") italic = on;
      if (token.text == "b") bold = on;
      if (token.text == "u") underline = on;
      continue;
    }
    ir::Inline node = ir::Text{token.type == MarkupToken::Type::LineBreak ? std::string(" ") : token.text};
    if (underline) node = ir::Underline{{std::move(node)}};
    if (bold) node = ir::Strong{{std::move(node)}};
    if (italic) node = ir::Emphasis{{std::move(node)}};
    out.push_back(std::move(node));
  }
  return out;
}

Result<AnyIR> to_document(AnyIR value, Context& ctx) {
  const ir::Subtitles subs = std::move(value).get<ir::Subtitles>();
  const bool timestamps = ctx.options.get_bool("transcript_timestamps", false);
  const std::int64_t gap = ctx.options.get_int("paragraph_gap_ms", 2000, 0, 3'600'000);
  ir::Document document;
  if (const auto title = subs.metadata.find("title"); title != subs.metadata.end()) {
    document.metadata.title = title->second;
    document.blocks.push_back(ir::build::heading(1, title->second));
  }
  if (const auto language = subs.metadata.find("language"); language != subs.metadata.end()) {
    document.metadata.language = language->second;
  }
  for (const Paragraph& paragraph : paragraphs(subs, gap)) {
    ir::Inlines content;
    if (timestamps) content.push_back(ir::Text{"[" + stamp(paragraph.start_ms) + "] "});
    if (paragraph.speaker && !paragraph.speaker->empty()) {
      content.push_back(ir::Strong{{ir::Text{*paragraph.speaker + ":"}}});
      content.push_back(ir::Text{" "});
    }
    for (std::size_t i = 0; i < paragraph.pieces.size(); ++i) {
      if (i > 0) content.push_back(ir::Text{" "});
      for (ir::Inline& node : markup_inlines(paragraph.pieces[i])) content.push_back(std::move(node));
    }
    document.blocks.push_back(ir::Paragraph{std::move(content)});
  }
  return document;
}

std::vector<OptionSpec> transcript_options() {
  return {
      OptionSpec::boolean("transcript_timestamps", "Time stamps", false,
                          "Start every transcript paragraph with its time ([hh:mm:ss])."),
      OptionSpec::integer("paragraph_gap_ms", "Paragraph pause (ms)", 2000, 0, 3'600'000,
                          "Start a new paragraph after a pause longer than this, as well as whenever the speaker "
                          "changes. 0 = only on speaker changes.")
          .advanced(),
  };
}

}  // namespace

void register_bridges(Registry& registry) {
  Bridge table;
  table.from = Kind::Subtitles;
  table.to = Kind::Table;
  table.fn = to_table;
  table.cost = 15;
  table.lossy = true;
  table.label = "list subtitle cues as a table";
  table.options = {OptionSpec::boolean("table_markup", "Keep formatting tags", false,
                                       "Keep <i>, <b> and <u> tags in the text column.")
                       .advanced()};
  registry.add_bridge(std::move(table));

  Bridge data;
  data.from = Kind::Subtitles;
  data.to = Kind::Data;
  data.fn = to_data;
  data.cost = 10;
  data.label = "subtitle cues as structured data";
  registry.add_bridge(std::move(data));

  Bridge text;
  text.from = Kind::Subtitles;
  text.to = Kind::PlainText;
  text.fn = to_plain_text;
  text.cost = 20;
  text.lossy = true;
  text.label = "subtitles as a plain-text transcript";
  text.options = transcript_options();
  registry.add_bridge(std::move(text));

  Bridge document;
  document.from = Kind::Subtitles;
  document.to = Kind::Document;
  document.fn = to_document;
  document.cost = 20;
  document.lossy = true;
  document.label = "subtitles as a transcript document";
  document.options = transcript_options();
  registry.add_bridge(std::move(document));
}

}  // namespace cvt::subtitle
