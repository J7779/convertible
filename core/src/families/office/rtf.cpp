// Convertible core - office family: Rich Text Format reader and writer.
//
// Reader: a complete control-word tokenizer with a group state stack;
// destinations are skipped correctly (including \* ones), text in any
// ANSI code page (\ansicpg, \fcharset) or as \u with \uc fallback skipping,
// character formatting, style sheet (heading styles, outline levels), the
// Word 97 list table and the older \pn numbering, tables (\trowd, \cellx,
// horizontal and vertical merges, nested tables flattened), fields
// (HYPERLINK), bookmarks, pictures (PNG, JPEG, and WMF/EMF kept as bytes),
// footnotes, document information.
//
// Writer: RTF 1.9 that Word, LibreOffice and TextEdit open: style sheet with
// heading styles, list table, tables with merged cells, PNG/JPEG pictures,
// hyperlink fields, footnotes, page setup; non-ASCII text as \u escapes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "families/table/codepages.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

constexpr std::size_t max_group_depth = 256;

// =========================================================================== reader

enum class Dest : std::uint8_t {
  Text,       // normal document text
  Skip,       // ignored destination
  FontTable,
  StyleSheet,
  Info,       // \info: sub-destinations carry the values
  InfoValue,  // \title, \author ...
  Picture,
  FieldInstruction,
  ListTable,
  ListOverride,
  ListLevel,
  Bookmark,
  Numbering,  // \pn (Word 6/95 paragraph numbering)
};

struct CharState {
  bool bold = false, italic = false, underline = false, strike = false, super = false, sub = false, hidden = false;
  int font = -1;
  int uc = 1;
  [[nodiscard]] RunFormat format(bool mono, const std::string& link) const {
    RunFormat f;
    f.bold = bold;
    f.italic = italic;
    f.underline = underline && link.empty();
    f.strike = strike;
    f.superscript = super;
    f.subscript = sub;
    f.code = mono;
    f.hidden = hidden;
    f.link = link;
    return f;
  }
};

struct ParaState {
  int style = 0;
  int outline = -1;
  int list = 0;   // \ls
  int ilvl = 0;
  bool in_table = false;
  int nest = 1;   // \itap
  bool pn_bullet = false;
  bool pn_ordered = false;
  int left_indent = 0;  // \li in twips
  ir::ListNumbering pn_numbering = ir::ListNumbering::Decimal;
};

struct Group {
  Dest dest = Dest::Text;
  CharState chars;
  std::string info_key;
  bool field_result = false;
  bool footnote = false;  // this group opened a footnote
  bool field = false;     // this group opened a \field
  bool shppict = false;
};

struct Font {
  int code_page = 1252;
  bool mono = false;
  bool symbol = false;
};

struct StyleDef {
  std::string name;
  int outline = -1;
};

struct ListDef {
  std::vector<std::pair<bool, ir::ListNumbering>> levels;  // ordered?, numbering
  std::vector<int> starts;
};

struct PictureState {
  std::string hex;
  Bytes binary;
  std::string mime;
  int width = 0, height = 0, goal_w = 0, goal_h = 0;
};

struct CellDef {
  int right = 0;
  bool hmerge_first = false, hmerge = false, vmerge_first = false, vmerge = false;
};

// The flow of one story (main text or a footnote).
struct Story {
  FlowBuilder flow;
  InlineBuilder inlines;
  ParaState para;
  // Table under construction.
  bool table_open = false;
  ir::TableBlock table;
  std::vector<ir::TableCell> row_cells;
  ir::Inlines cell;
  std::vector<CellDef> cells;  // \cellx definitions of the current row
  std::vector<std::vector<CellDef>> row_defs;
  std::vector<int> header_rows;
  bool row_header = false;
  int row_left = 0;                     // \trleft
  std::vector<std::vector<std::pair<int, int>>> row_bounds;  // per row: (left, right) of each emitted cell
  // Lists written as plain paragraphs with a marker and a tab (pandoc, old writers).
  std::vector<int> pseudo_indents;
};

class RtfReader {
 public:
  RtfReader(const ReadContext& ctx, ReadSettings settings) : ctx_(ctx), settings_(settings) {}
  Result<ir::Document> read(std::string_view rtf);

 private:
  void control(std::string_view word, std::optional<int> param);
  void symbol(char c);
  void text(std::string_view chars);
  void raw_byte(std::uint8_t b);
  void flush_bytes();
  void add_text_utf8(std::string_view utf8);
  void end_paragraph(bool cell_end);
  void end_cell();
  void end_row();
  void close_table();
  void open_group();
  void close_group();
  [[nodiscard]] Story& story() { return *stories_.back(); }
  [[nodiscard]] Group& group() { return groups_.back(); }
  [[nodiscard]] bool mono() const;
  [[nodiscard]] int code_page() const;
  [[nodiscard]] std::string current_link() const;

  const ReadContext& ctx_;
  ReadSettings settings_;
  std::vector<Group> groups_;
  std::vector<std::unique_ptr<Story>> stories_;
  std::map<int, Font> fonts_;
  std::map<int, StyleDef> styles_;
  std::map<int, ListDef> list_by_id_;  // \listid -> definition
  std::map<int, int> ls_to_list_;      // \ls -> \listid
  ListDef pending_list_;
  int pending_list_id_ = 0;
  int pending_override_list_ = 0;
  int ansi_cpg_ = 1252;
  int default_font_ = 0;
  int font_being_defined_ = -1;
  int style_being_defined_ = -1;
  std::string style_name_;
  std::string field_instr_;
  std::vector<std::string> link_stack_;
  std::string bookmark_;
  std::string pending_bookmark_;
  PictureState picture_;
  std::string bytes_;  // pending \'hh bytes
  int skip_ = 0;       // characters still to skip after \u
  std::vector<ir::Footnote> footnotes_;
  ir::Document doc_;
  std::size_t steps_ = 0;
  bool warned_dbcs_ = false;
  int current_size_ = 24;
  int high_surrogate_ = 0;
  int bin_pending_ = 0;
  CellDef pending_cell_;
};

const table::CodePageInfo* page_for(int cp) {
  if (cp == 0) cp = 1252;
  return table::find_code_page(cp);
}

int charset_code_page(int charset) {
  switch (charset) {
    case 0: return 0;
    case 2: return -1;  // symbol
    case 77: return 10000;
    case 128: return 932;
    case 129: return 949;
    case 134: return 936;
    case 136: return 950;
    case 161: return 1253;
    case 162: return 1254;
    case 163: return 1258;
    case 177: return 1255;
    case 178: return 1256;
    case 186: return 1257;
    case 204: return 1251;
    case 222: return 874;
    case 238: return 1250;
    case 255: return 437;
    default: return 0;
  }
}

bool RtfReader::mono() const {
  const auto it = fonts_.find(groups_.back().chars.font >= 0 ? groups_.back().chars.font : default_font_);
  return it != fonts_.end() && it->second.mono;
}

int RtfReader::code_page() const {
  const auto it = fonts_.find(groups_.back().chars.font >= 0 ? groups_.back().chars.font : default_font_);
  if (it != fonts_.end() && it->second.code_page != 0) return it->second.code_page;
  return ansi_cpg_;
}

std::string RtfReader::current_link() const { return link_stack_.empty() ? std::string() : link_stack_.back(); }

void RtfReader::add_text_utf8(std::string_view utf8) {
  if (utf8.empty()) return;
  Group& g = group();
  switch (g.dest) {
    case Dest::Text: {
      if (g.chars.hidden && !settings_.hidden) return;
      story().inlines.text(utf8, g.chars.format(mono(), current_link()));
      return;
    }
    case Dest::FontTable: style_name_ += utf8; return;
    case Dest::StyleSheet: style_name_ += utf8; return;
    case Dest::InfoValue: {
      std::string& value = g.info_key == "title"    ? doc_.metadata.title
                           : g.info_key == "author" ? doc_.metadata.author
                                                    : doc_.metadata.extra[g.info_key];
      value += utf8;
      return;
    }
    case Dest::FieldInstruction: field_instr_ += utf8; return;
    case Dest::Bookmark: bookmark_ += utf8; return;
    case Dest::Picture: {
      for (char c : utf8) {
        if (util::is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) picture_.hex.push_back(c);
      }
      return;
    }
    default: return;
  }
}

void RtfReader::flush_bytes() {
  if (bytes_.empty()) return;
  const int cp = code_page();
  std::string utf8;
  if (cp == -1) {
    // Symbol font: Greek letters and a few signs.
    for (char c : bytes_) {
      const auto b = static_cast<unsigned char>(c);
      if (b == 0xB7) utf8 += "\xE2\x80\xA2";
      else if (b < 0x80) utf8.push_back(c);
    }
  } else if (const table::CodePageInfo* page = page_for(cp)) {
    utf8 = table::decode_code_page(as_bytes(bytes_), *page);
  } else {
    if (!warned_dbcs_) {
      ctx_.warn("Text in code page " + std::to_string(cp) + " was read as Windows-1252.");
      warned_dbcs_ = true;
    }
    utf8 = util::decode_text(as_bytes(bytes_), util::TextEncoding::Windows1252).text;
  }
  bytes_.clear();
  add_text_utf8(utf8);
}

void RtfReader::raw_byte(std::uint8_t b) {
  if (skip_ > 0) {
    --skip_;
    return;
  }
  bytes_.push_back(static_cast<char>(b));
}

void RtfReader::text(std::string_view chars) {
  for (char c : chars) {
    if (c == '\r' || c == '\n') continue;
    if (skip_ > 0) {
      --skip_;
      continue;
    }
    if (static_cast<unsigned char>(c) >= 0x80) {
      bytes_.push_back(c);
    } else {
      flush_bytes();
      if (group().dest == Dest::FontTable || group().dest == Dest::StyleSheet) {
        if (c == ';') {
          // End of an entry.
          if (group().dest == Dest::FontTable && font_being_defined_ >= 0) {
            const std::string name = util::to_lower(util::trim(style_name_));
            Font& font = fonts_[font_being_defined_];
            font.mono = font.mono || name.find("courier") != std::string::npos || name.find("mono") != std::string::npos ||
                        name.find("consolas") != std::string::npos || name.find("menlo") != std::string::npos ||
                        name.find("monaco") != std::string::npos;
            if (name == "symbol") font.code_page = -1;
          }
          if (group().dest == Dest::StyleSheet && style_being_defined_ >= 0) {
            styles_[style_being_defined_].name = std::string(util::trim(style_name_));
          }
          style_name_.clear();
          continue;
        }
      }
      const char one[1] = {c};
      add_text_utf8(std::string_view(one, 1));
    }
  }
}

void RtfReader::open_group() {
  flush_bytes();
  Group g = groups_.back();
  g.footnote = false;
  g.field = false;
  g.shppict = false;
  if (g.dest == Dest::FontTable || g.dest == Dest::StyleSheet) style_name_.clear();
  groups_.push_back(g);
}

void RtfReader::close_group() {
  flush_bytes();
  skip_ = 0;
  if (groups_.size() <= 1) return;
  Group closing = groups_.back();
  groups_.pop_back();
  if (closing.dest == Dest::Picture && groups_.back().dest != Dest::Picture) {
    Bytes data = std::move(picture_.binary);
    if (data.empty() && picture_.hex.size() >= 2) {
      data.reserve(picture_.hex.size() / 2);
      for (std::size_t i = 0; i + 1 < picture_.hex.size(); i += 2) {
        const auto value = util::parse_int(std::string_view(picture_.hex).substr(i, 2), 16);
        data.push_back(static_cast<std::byte>(value.value_or(0)));
      }
    }
    if (!data.empty() && groups_.back().dest == Dest::Text) {
      ir::EncodedImage image;
      image.bytes = std::move(data);
      image.mime = picture_.mime.empty() ? document::sniff_image_mime(image.bytes) : picture_.mime;
      story().inlines.node(ir::InlineImage{std::move(image), {}}, RunFormat{});
    }
    picture_ = PictureState{};
  }
  if (closing.dest == Dest::FieldInstruction && groups_.back().dest != Dest::FieldInstruction) {
    // The instruction is complete; the result group follows.
  }
  if (closing.field) {
    if (!link_stack_.empty()) link_stack_.pop_back();
    field_instr_.clear();
  }
  if (closing.dest == Dest::Bookmark && groups_.back().dest != Dest::Bookmark) {
    if (pending_bookmark_.empty()) pending_bookmark_ = std::string(util::trim(bookmark_));
    bookmark_.clear();
  }
  if (closing.dest == Dest::ListLevel && groups_.back().dest == Dest::ListTable) {
    // Level already recorded while parsing.
  }
  if (closing.dest == Dest::ListTable && groups_.back().dest == Dest::Skip) {
    // nothing
  }
  if (closing.footnote) {
    // Finish the note's story.
    end_paragraph(false);
    close_table();
    std::unique_ptr<Story> note = std::move(stories_.back());
    stories_.pop_back();
    ir::Blocks blocks = note->flow.take();
    merge_adjacent_blocks(blocks);
    const std::string label = std::to_string(footnotes_.size() + 1);
    footnotes_.push_back(ir::Footnote{label, std::move(blocks)});
    ir::Link link;
    link.url = "#fn-" + label;
    link.note = label;
    link.children.push_back(ir::Text{label});
    if (settings_.notes) story().inlines.node(std::move(link), RunFormat{});
  }
}

void RtfReader::close_table() {
  Story& s = story();
  if (!s.table_open) return;
  if (!s.row_cells.empty() || !s.cell.empty()) end_row();
  s.table_open = false;
  ir::TableBlock table = std::move(s.table);
  s.table = ir::TableBlock{};
  s.row_defs.clear();
  // Cells wider than the narrowest grid columns span several of them
  // (writers such as LibreOffice express merges through \cellx only).
  if (s.row_bounds.size() == table.rows.size()) {
    std::vector<int> edges;
    bool usable = true;
    for (const auto& row : s.row_bounds) {
      if (row.empty()) usable = false;
      for (const auto& [l, r] : row) edges.push_back(r);
    }
    std::ranges::sort(edges);
    edges.erase(std::unique(edges.begin(), edges.end(), [](int a, int b) { return std::abs(a - b) < 20; }), edges.end());
    if (usable && edges.size() <= 64) {
      for (std::size_t r = 0; r < table.rows.size(); ++r) {
        for (std::size_t c = 0; c < table.rows[r].cells.size(); ++c) {
          const auto [left, right] = s.row_bounds[r][c];
          int span = 0;
          for (int e : edges) {
            if (e > left + 10 && e <= right + 10) ++span;
          }
          if (span > table.rows[r].cells[c].colspan) table.rows[r].cells[c].colspan = span;
        }
      }
    }
  }
  s.row_bounds.clear();
  if (!table.rows.empty()) s.flow.block(std::move(table));
}

void RtfReader::end_cell() {
  Story& s = story();
  ir::Inlines content = s.inlines.take();
  trim_inlines(content);
  if (!s.cell.empty() && !content.empty()) s.cell.push_back(ir::LineBreak{});
  for (ir::Inline& x : content) s.cell.push_back(std::move(x));
  ir::TableCell cell;
  cell.content = std::move(s.cell);
  s.cell.clear();
  s.row_cells.push_back(std::move(cell));
}

void RtfReader::end_row() {
  Story& s = story();
  if (!s.cell.empty() || !s.inlines.empty()) end_cell();
  // Apply merges from the cell definitions of this row.
  const std::vector<CellDef>& defs = s.cells;
  ir::TableRow row;
  row.header = s.row_header && (s.table.rows.empty() || s.table.rows.back().header);
  const std::size_t row_index = s.table.rows.size();
  std::vector<std::pair<int, int>> bounds;
  for (std::size_t i = 0; i < s.row_cells.size(); ++i) {
    const CellDef def = i < defs.size() ? defs[i] : CellDef{};
    const int left = i == 0 ? s.row_left : (i - 1 < defs.size() ? defs[i - 1].right : 0);
    const int right = def.right;
    if (def.hmerge && !row.cells.empty()) {
      row.cells.back().colspan += 1;
      if (!bounds.empty()) bounds.back().second = right;
      continue;
    }
    if (def.vmerge && row_index > 0) {
      // Find the cell above at the same column and extend it.
      std::size_t column = 0;
      for (const ir::TableCell& c : row.cells) column += static_cast<std::size_t>(c.colspan);
      for (std::size_t r = row_index; r-- > 0;) {
        std::size_t col = 0;
        bool found = false;
        for (ir::TableCell& c : s.table.rows[r].cells) {
          if (col == column) {
            if (c.rowspan + static_cast<int>(r) >= static_cast<int>(row_index)) {
              c.rowspan += 1;
              found = true;
            }
            break;
          }
          col += static_cast<std::size_t>(c.colspan);
        }
        if (found) break;
      }
      // Keep the grid aligned: a covered slot is not a cell of its own.
      continue;
    }
    row.cells.push_back(std::move(s.row_cells[i]));
    bounds.emplace_back(left, right);
  }
  if (!row.cells.empty()) s.row_bounds.push_back(bounds.size() == row.cells.size() ? bounds : std::vector<std::pair<int, int>>{});
  s.row_cells.clear();
  if (!row.cells.empty()) s.table.rows.push_back(std::move(row));
  s.cells.clear();
  s.row_header = false;
}

void RtfReader::end_paragraph(bool cell_end) {
  flush_bytes();
  Story& s = story();
  const ParaState& p = s.para;
  if (p.in_table && p.nest <= 1) {
    if (!s.table_open) {
      s.table_open = true;
    }
    ir::Inlines content = s.inlines.take();
    trim_inlines(content);
    if (!s.cell.empty() && !content.empty()) s.cell.push_back(ir::LineBreak{});
    for (ir::Inline& x : content) s.cell.push_back(std::move(x));
    if (cell_end) {
      ir::TableCell cell;
      cell.content = std::move(s.cell);
      s.cell.clear();
      s.row_cells.push_back(std::move(cell));
    }
    return;
  }
  if (p.in_table && p.nest > 1) {
    // Nested table: its text joins the enclosing cell, line by line.
    ir::Inlines content = s.inlines.take();
    trim_inlines(content);
    if (!content.empty()) {
      if (!s.cell.empty()) s.cell.push_back(cell_end ? ir::Inline(ir::Text{" "}) : ir::Inline(ir::LineBreak{}));
      for (ir::Inline& x : content) s.cell.push_back(std::move(x));
    }
    return;
  }
  if (s.table_open) close_table();
  if (s.inlines.empty()) {
    pending_bookmark_.clear();
    return;
  }
  const bool blank = s.inlines.blank();
  ir::Inlines content = s.inlines.take();
  trim_inlines(content);
  if (blank && content.empty()) return;
  if (content.empty()) return;

  int outline = p.outline;
  std::string sname;
  if (auto it = styles_.find(p.style); it != styles_.end()) {
    sname = util::to_lower(it->second.name);
    if (outline < 0) outline = it->second.outline;
  }
  int heading = outline >= 0 && outline < 9 ? std::min(outline + 1, 6) : 0;
  if (heading == 0 && p.style != 0) heading = heading_level_from_style_name(sname);
  if (heading == 0 && !settings_.strict_headings && p.list == 0 && content.size() == 1 && content.front().is<ir::Strong>()) {
    const std::string plain = ir::plain_text(content);
    if (plain.size() < 120 && current_size_ >= 32) {
      heading = current_size_ >= 40 ? 1 : 2;
    }
  }
  ir::Attributes attributes;
  if (!pending_bookmark_.empty()) {
    attributes.id = pending_bookmark_;
    pending_bookmark_.clear();
  }
  if (heading != 0) {
    if (content.size() == 1 && content.front().is<ir::Strong>()) {
      ir::Inlines inner = std::move(content.front().get_if<ir::Strong>()->children);
      content = std::move(inner);
    }
    s.flow.block(ir::Block(ir::Heading{heading, std::move(content)}, std::move(attributes)));
    return;
  }
  if (p.list != 0 || p.pn_bullet || p.pn_ordered) {
    FlowBuilder::ListInfo info;
    info.level = p.ilvl;
    if (p.list != 0) {
      info.list_id = std::to_string(p.list);
      const auto ls = ls_to_list_.find(p.list);
      const auto def = ls == ls_to_list_.end() ? list_by_id_.end() : list_by_id_.find(ls->second);
      if (def != list_by_id_.end() && static_cast<std::size_t>(p.ilvl) < def->second.levels.size()) {
        info.ordered = def->second.levels[static_cast<std::size_t>(p.ilvl)].first;
        info.numbering = def->second.levels[static_cast<std::size_t>(p.ilvl)].second;
        info.start = def->second.starts[static_cast<std::size_t>(p.ilvl)];
      }
    } else {
      info.list_id = "pn";
      info.ordered = p.pn_ordered;
      info.numbering = p.pn_numbering;
    }
    s.flow.list_item(info, ir::Paragraph{std::move(content)});
    return;
  }
  // A paragraph of monospaced text only is a code block.
  bool all_code = true;
  for (const ir::Inline& x : content) {
    if (!x.is<ir::Code>() && !x.is<ir::LineBreak>()) all_code = false;
  }
  // A list written as "marker TAB text" paragraphs.
  {
    std::string plain = ir::plain_text(content);
    const std::size_t tab = plain.find('\t');
    std::string_view marker = tab == std::string::npos ? std::string_view() : std::string_view(plain).substr(0, tab);
    static const std::set<std::string_view> bullets = {"\xE2\x80\xA2", "\xE2\x80\x93", "-", "*", "\xE2\x97\xA6",
                                                       "\xE2\x96\xAA", "\xC2\xB7", "o", "\xE2\x80\x94"};
    bool ordered = false;
    bool bullet = bullets.contains(marker);
    if (!bullet && marker.size() >= 2 && marker.size() <= 5 && (marker.back() == '.' || marker.back() == ')')) {
      ordered = std::ranges::all_of(marker.substr(0, marker.size() - 1), [](char ch) { return util::is_ascii_digit(ch); });
    }
    if ((bullet || ordered) && tab != std::string::npos && p.left_indent > 0) {
      // Drop the marker and the tab from the content.
      std::size_t to_drop = tab + 1;
      ir::Inlines rest;
      for (ir::Inline& x : content) {
        if (to_drop > 0) {
          if (auto* t = x.get_if<ir::Text>()) {
            if (t->text.size() <= to_drop) {
              to_drop -= t->text.size();
              continue;
            }
            t->text.erase(0, to_drop);
            to_drop = 0;
          } else {
            to_drop = 0;
          }
        }
        rest.push_back(std::move(x));
      }
      std::vector<int>& indents = s.pseudo_indents;
      while (!indents.empty() && indents.back() > p.left_indent) indents.pop_back();
      if (indents.empty() || indents.back() < p.left_indent) indents.push_back(p.left_indent);
      FlowBuilder::ListInfo info;
      info.list_id = "pseudo";
      info.level = static_cast<int>(indents.size()) - 1;
      info.ordered = ordered;
      if (ordered) info.start = static_cast<int>(util::parse_int(marker.substr(0, marker.size() - 1)).value_or(1));
      s.flow.list_item(info, ir::Paragraph{std::move(rest)});
      return;
    }
    s.pseudo_indents.clear();
  }
  if (all_code && !content.empty()) {
    std::string code;
    for (const ir::Inline& x : content) {
      if (const auto* c = x.get_if<ir::Code>()) code += c->text;
      else code += "\n";
    }
    s.flow.block(ir::CodeBlock{{}, std::move(code)});
    return;
  }
  const bool code = sname.find("code") != std::string::npos || sname.find("preformatted") != std::string::npos;
  if (code) {
    s.flow.block(ir::CodeBlock{{}, ir::plain_text(content)});
    return;
  }
  if (sname == "quote" || sname == "block text") {
    s.flow.block(ir::BlockQuote{{ir::Paragraph{std::move(content)}}});
    return;
  }
  if (content.size() == 1 && content.front().is<ir::InlineImage>()) {
    auto& image = *content.front().get_if<ir::InlineImage>();
    s.flow.block(ir::ImageBlock{std::move(image.source), image.alt, {}});
    return;
  }
  s.flow.block(ir::Block(ir::Paragraph{std::move(content)}, std::move(attributes)));
}

void RtfReader::symbol(char c) {
  switch (c) {
    case '\\':
    case '{':
    case '}': {
      const char one[1] = {c};
      text(std::string_view(one, 1));
      return;
    }
    case '~': flush_bytes(); add_text_utf8("\xC2\xA0"); return;
    case '_': flush_bytes(); add_text_utf8("\xE2\x80\x91"); return;
    case '-': return;  // optional hyphen
    case '*': group().dest = group().dest == Dest::Text ? Dest::Skip : group().dest == Dest::ListTable ? Dest::ListTable
                                                                                                        : Dest::Skip;
      return;
    case '\n':
    case '\r': end_paragraph(false); return;
    case '\t': flush_bytes(); add_text_utf8("\t"); return;
    default: return;
  }
}

ir::ListNumbering nfc_numbering(int nfc) {
  switch (nfc) {
    case 1: return ir::ListNumbering::UpperRoman;
    case 2: return ir::ListNumbering::LowerRoman;
    case 3: return ir::ListNumbering::UpperAlpha;
    case 4: return ir::ListNumbering::LowerAlpha;
    default: return ir::ListNumbering::Decimal;
  }
}

void RtfReader::control(std::string_view word, std::optional<int> param) {
  Group& g = group();
  const int value = param.value_or(1);
  // Destinations.
  static const std::set<std::string_view> skipped = {
      "colortbl", "header", "headerl", "headerr", "headerf", "footer", "footerl", "footerr", "footerf", "pntext",
      "listtext", "xmlnstbl", "rsidtbl", "generator", "themedata", "colorschememapping", "latentstyles", "datastore",
      "nonshppict", "object", "fldtype", "revtbl", "mmathPr", "pgdsctbl", "userprops", "docvar", "xmlopen", "ftnsep",
      "ftnsepc", "aftnsep", "aftnsepc", "annotation", "atnid", "atnauthor", "atrfstart", "atrfend", "bkmkend",
      "background", "shpinst", "sp", "sn", "sv", "objdata", "result", "template", "filetbl", "keywords", "operator",
      "doccomm", "company", "manager", "category", "hlinkbase", "passwordhash", "protusertbl", "wgrffmtfilter",
      "fchars", "lchars", "picprop", "blipuid", "listpicture", "pgptbl", "falt", "panose", "fname", "stshfdbch",
      "ftnalt", "expandedcolortbl", "listname"};
  if (word == "pn") {
    g.dest = Dest::Numbering;
    return;
  }
  if (g.dest == Dest::Numbering) {
    Story& st = story();
    if (word == "pnlvlblt") st.para.pn_bullet = true;
    if (word == "pnlvlbody") st.para.pn_ordered = true;
    if (word == "pnlvl") {
      st.para.pn_ordered = true;
      st.para.ilvl = std::clamp(value - 1, 0, 8);
    }
    if (word == "pnlcltr") st.para.pn_numbering = ir::ListNumbering::LowerAlpha;
    if (word == "pnucltr") st.para.pn_numbering = ir::ListNumbering::UpperAlpha;
    if (word == "pnlcrm") st.para.pn_numbering = ir::ListNumbering::LowerRoman;
    if (word == "pnucrm") st.para.pn_numbering = ir::ListNumbering::UpperRoman;
    return;
  }
  if (word == "fonttbl") { g.dest = Dest::FontTable; return; }
  if (word == "stylesheet") { g.dest = Dest::StyleSheet; return; }
  if (word == "info") { g.dest = Dest::Info; return; }
  if (g.dest == Dest::Info && (word == "title" || word == "author" || word == "subject" || word == "comment")) {
    g.dest = Dest::InfoValue;
    g.info_key = std::string(word);
    if (!settings_.metadata && word != "title") g.dest = Dest::Skip;
    return;
  }
  if (g.dest == Dest::Info && (word == "creatim")) { g.dest = Dest::Skip; return; }
  if (word == "pict") {
    g.dest = Dest::Picture;
    picture_ = PictureState{};
    return;
  }
  if (word == "shppict") { g.shppict = true; return; }
  if (word == "fldinst") { g.dest = Dest::FieldInstruction; field_instr_.clear(); return; }
  if (word == "fldrslt") {
    g.field_result = true;
    std::string_view instr = util::trim(field_instr_);
    std::string link;
    if (util::istarts_with(instr, "HYPERLINK")) {
      instr.remove_prefix(9);
      instr = util::trim(instr);
      std::string target;
      bool anchor = false;
      while (!instr.empty()) {
        if (instr.starts_with("\\l")) {
          anchor = true;
          instr = util::trim(instr.substr(2));
          continue;
        }
        if (instr.starts_with('\\')) {
          const std::size_t sp = instr.find(' ');
          instr = sp == std::string_view::npos ? std::string_view() : util::trim(instr.substr(sp));
          if (instr.starts_with('"')) {
            const std::size_t close = instr.find('"', 1);
            instr = close == std::string_view::npos ? std::string_view() : util::trim(instr.substr(close + 1));
          }
          continue;
        }
        std::string token;
        if (instr.starts_with('"')) {
          const std::size_t close = instr.find('"', 1);
          token = std::string(instr.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
          instr = close == std::string_view::npos ? std::string_view() : util::trim(instr.substr(close + 1));
        } else {
          const std::size_t sp = instr.find(' ');
          token = std::string(instr.substr(0, sp));
          instr = sp == std::string_view::npos ? std::string_view() : util::trim(instr.substr(sp));
        }
        if (anchor) {
          target += "#" + token;
          anchor = false;
        } else if (target.empty()) {
          target = token;
        }
      }
      link = target;
    }
    // Inside the field group (the parent), push the link.
    if (groups_.size() >= 2) {
      // Find the nearest enclosing field group.
      for (std::size_t i = groups_.size(); i-- > 0;) {
        if (groups_[i].field) {
          if (!link.empty()) {
            link_stack_.push_back(link);
          } else {
            link_stack_.push_back(current_link());
          }
          break;
        }
      }
    }
    return;
  }
  if (word == "field") { g.field = true; return; }
  if (word == "bkmkstart") { g.dest = Dest::Bookmark; bookmark_.clear(); return; }
  if (word == "footnote") {
    if (stories_.size() > 8) {
      g.dest = Dest::Skip;
      return;
    }
    g.footnote = true;
    g.dest = Dest::Text;
    g.chars = CharState{};
    stories_.push_back(std::make_unique<Story>());
    return;
  }
  if (word == "listtable") { g.dest = Dest::ListTable; return; }
  if (word == "listoverridetable") { g.dest = Dest::ListOverride; return; }
  if (g.dest == Dest::ListTable || g.dest == Dest::ListLevel) {
    if (word == "list") { pending_list_ = ListDef{}; pending_list_id_ = 0; return; }
    if (word == "listlevel") {
      g.dest = Dest::ListLevel;
      pending_list_.levels.emplace_back(true, ir::ListNumbering::Decimal);
      pending_list_.starts.push_back(1);
      return;
    }
    if (word == "levelnfc" || word == "levelnfcn") {
      if (!pending_list_.levels.empty()) {
        pending_list_.levels.back().first = value != 23 && value != 255;
        pending_list_.levels.back().second = nfc_numbering(value);
      }
      return;
    }
    if (word == "levelstartat") {
      if (!pending_list_.starts.empty()) pending_list_.starts.back() = std::clamp(value, 0, 1000000);
      return;
    }
    if (word == "listid" && g.dest == Dest::ListTable) {
      pending_list_id_ = value;
      list_by_id_[value] = pending_list_;
      return;
    }
    return;
  }
  if (g.dest == Dest::ListOverride) {
    if (word == "listid") pending_override_list_ = value;
    if (word == "ls") ls_to_list_[value] = pending_override_list_;
    return;
  }
  if (skipped.contains(word)) {
    if (word == "header" || word == "headerl" || word == "headerr" || word == "headerf" || word == "footer" ||
        word == "footerl" || word == "footerr" || word == "footerf") {
      if (settings_.headers) return;
    }
    g.dest = Dest::Skip;
    return;
  }
  if (g.dest == Dest::Skip) return;

  if (g.dest == Dest::FontTable) {
    if (word == "f") font_being_defined_ = value;
    if (word == "fcharset" && font_being_defined_ >= 0) fonts_[font_being_defined_].code_page = charset_code_page(value);
    if (word == "cpg" && font_being_defined_ >= 0) fonts_[font_being_defined_].code_page = value;
    if (word == "fmodern" && font_being_defined_ >= 0) fonts_[font_being_defined_].mono = true;
    return;
  }
  if (g.dest == Dest::StyleSheet) {
    if (word == "s" || word == "cs" || word == "ds" || word == "ts") style_being_defined_ = word == "s" ? value : -1;
    if (word == "outlinelevel" && style_being_defined_ >= 0) styles_[style_being_defined_].outline = value;
    return;
  }
  if (g.dest == Dest::Picture) {
    if (word == "pngblip") picture_.mime = "image/png";
    else if (word == "jpegblip") picture_.mime = "image/jpeg";
    else if (word == "emfblip") picture_.mime = "image/x-emf";
    else if (word == "wmetafile") picture_.mime = "image/x-wmf";
    else if (word == "dibitmap" || word == "wbitmap") picture_.mime = "image/bmp";
    else if (word == "picw") picture_.width = value;
    else if (word == "pich") picture_.height = value;
    else if (word == "picwgoal") picture_.goal_w = value;
    else if (word == "pichgoal") picture_.goal_h = value;
    return;
  }
  if (g.dest != Dest::Text && g.dest != Dest::FieldInstruction) {
    if (word == "ansicpg") ansi_cpg_ = value;
    return;
  }

  Story& s = story();
  CharState& c = g.chars;
  const bool on = !param || *param != 0;
  if (word == "par" || word == "sect") { end_paragraph(false); return; }
  if (word == "cell") { flush_bytes(); end_paragraph(true); return; }
  if (word == "nestcell") { flush_bytes(); end_paragraph(true); return; }
  if (word == "row") { flush_bytes(); end_row(); return; }
  if (word == "nestrow") { return; }
  if (word == "line") { flush_bytes(); s.inlines.line_break(c.format(mono(), current_link())); return; }
  if (word == "tab") { flush_bytes(); add_text_utf8("\t"); return; }
  if (word == "page") {
    flush_bytes();
    end_paragraph(false);
    if (!s.para.in_table) s.flow.block(ir::PageBreak{});
    return;
  }
  if (word == "u") {
    flush_bytes();
    int code = value;
    if (code < 0) code += 65536;
    std::u32string cp;
    // Surrogate pairs arrive as two \u words.
    if (code >= 0xD800 && code <= 0xDBFF) {
      high_surrogate_ = code;
    } else if (code >= 0xDC00 && code <= 0xDFFF && high_surrogate_ != 0) {
      cp.push_back(static_cast<char32_t>(0x10000 + ((high_surrogate_ - 0xD800) << 10) + (code - 0xDC00)));
      high_surrogate_ = 0;
    } else if (code > 0) {
      cp.push_back(static_cast<char32_t>(code));
      high_surrogate_ = 0;
    }
    if (!cp.empty()) add_text_utf8(util::utf32_to_utf8(cp));
    skip_ = c.uc;
    return;
  }
  if (word == "uc") { c.uc = std::clamp(value, 0, 8); return; }
  if (word == "bin") {
    // Raw binary data follows (only meaningful inside \pict).
    bin_pending_ = std::max(0, value);
    return;
  }
  if (word == "ansicpg") { ansi_cpg_ = value; return; }
  if (word == "deff") { default_font_ = value; return; }
  // Character formatting.
  if (word == "plain") {
    const int uc = c.uc;
    c = CharState{};
    c.uc = uc;
    current_size_ = 24;
    return;
  }
  if (word == "b") { c.bold = on; return; }
  if (word == "i") { c.italic = on; return; }
  if (word == "ul" || word == "uld" || word == "uldb" || word == "ulw" || word == "uldash" || word == "ulth") {
    c.underline = on;
    return;
  }
  if (word == "ulnone") { c.underline = false; return; }
  if (word == "strike" || word == "striked") { c.strike = on; return; }
  if (word == "super") { c.super = true; c.sub = false; return; }
  if (word == "sub") { c.sub = true; c.super = false; return; }
  if (word == "nosupersub") { c.super = c.sub = false; return; }
  if (word == "up") { c.super = value > 0; return; }
  if (word == "dn") { c.sub = value > 0; return; }
  if (word == "v") { c.hidden = on; return; }
  if (word == "f") { flush_bytes(); c.font = value; return; }
  if (word == "fs") { current_size_ = value; return; }
  // Paragraph formatting.
  if (word == "pard") {
    const bool in_table = s.para.in_table;
    s.para = ParaState{};
    (void)in_table;
    return;
  }
  if (word == "s") { s.para.style = value; return; }
  if (word == "outlinelevel") { s.para.outline = value; return; }
  if (word == "ls") { s.para.list = value; return; }
  if (word == "ilvl") { s.para.ilvl = std::clamp(value, 0, 8); return; }
  if (word == "intbl") { s.para.in_table = true; return; }
  if (word == "itap") { s.para.nest = value; if (value == 0) s.para.in_table = false; return; }
  if (word == "pnlvlblt") { s.para.pn_bullet = true; return; }
  if (word == "pnlvlbody" || word == "pndec") { s.para.pn_ordered = true; return; }
  if (word == "pnlvl") { s.para.pn_ordered = true; s.para.ilvl = std::clamp(value - 1, 0, 8); return; }
  // Table definitions.
  if (word == "trleft") { s.row_left = value; return; }
  if (word == "li") { s.para.left_indent = value; return; }
  if (word == "trowd") {
    s.cells.clear();
    s.row_left = 0;
    s.table_open = true;
    pending_cell_ = CellDef{};
    return;
  }
  if (word == "trhdr") { s.row_header = true; return; }
  if (word == "clmgf") { pending_cell_.hmerge_first = true; return; }
  if (word == "clmrg") { pending_cell_.hmerge = true; return; }
  if (word == "clvmgf") { pending_cell_.vmerge_first = true; return; }
  if (word == "clvmrg") { pending_cell_.vmerge = true; return; }
  if (word == "cellx") {
    pending_cell_.right = value;
    s.cells.push_back(pending_cell_);
    pending_cell_ = CellDef{};
    return;
  }
  // Special characters.
  static const std::map<std::string_view, std::string_view> chars = {
      {"emdash", "\xE2\x80\x94"}, {"endash", "\xE2\x80\x93"}, {"bullet", "\xE2\x80\xA2"},
      {"lquote", "\xE2\x80\x98"}, {"rquote", "\xE2\x80\x99"}, {"ldblquote", "\xE2\x80\x9C"},
      {"rdblquote", "\xE2\x80\x9D"}, {"emspace", "\xE2\x80\x83"}, {"enspace", "\xE2\x80\x82"},
      {"qmspace", "\xE2\x80\x85"}, {"zwj", "\xE2\x80\x8D"}, {"zwnj", "\xE2\x80\x8C"},
      {"ltrmark", "\xE2\x80\x8E"}, {"rtlmark", "\xE2\x80\x8F"}, {"zwbo", "\xE2\x80\x8B"}};
  if (const auto it = chars.find(word); it != chars.end()) {
    flush_bytes();
    add_text_utf8(it->second);
    return;
  }
  if (word == "chftn") return;  // the automatic note number: we number notes ourselves
}

Result<ir::Document> RtfReader::read(std::string_view rtf) {
  groups_.push_back(Group{});
  groups_.back().dest = Dest::Skip;  // until the outer {\rtf group opens
  stories_.push_back(std::make_unique<Story>());
  std::size_t i = 0;
  const std::size_t n = rtf.size();
  bool started = false;
  while (i < n) {
    if ((++steps_ & 0xFFFF) == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(n), "Reading"));
    const char c = rtf[i];
    if (bin_pending_ > 0) {
      const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(bin_pending_), n - i);
      if (group().dest == Dest::Picture) {
        const ByteView raw = as_bytes(rtf.substr(i, count));
        picture_.binary.insert(picture_.binary.end(), raw.begin(), raw.end());
      }
      i += count;
      bin_pending_ = 0;
      continue;
    }
    if (c == '{') {
      if (groups_.size() >= max_group_depth) return fail(Status::Limit, "RTF groups are nested too deeply");
      if (!started) {
        started = true;
        groups_.back().dest = Dest::Text;
        groups_.push_back(groups_.back());
      } else {
        open_group();
      }
      ++i;
    } else if (c == '}') {
      close_group();
      ++i;
    } else if (c == '\\') {
      if (i + 1 >= n) break;
      const char next = rtf[i + 1];
      if (util::is_ascii_alpha(next)) {
        std::size_t j = i + 1;
        while (j < n && util::is_ascii_alpha(rtf[j]) && j - i < 33) ++j;
        const std::string_view word = rtf.substr(i + 1, j - i - 1);
        std::optional<int> param;
        if (j < n && (rtf[j] == '-' || util::is_ascii_digit(rtf[j]))) {
          std::size_t k = j + (rtf[j] == '-' ? 1 : 0);
          std::int64_t v = 0;
          std::size_t digits = 0;
          while (k < n && util::is_ascii_digit(rtf[k])) {
            if (digits < 10) v = v * 10 + (rtf[k] - '0');
            ++digits;
            ++k;
          }
          if (rtf[j] == '-') v = -v;
          param = static_cast<int>(std::clamp<std::int64_t>(v, -2147483647, 2147483647));
          j = k;
        }
        if (j < n && rtf[j] == ' ') ++j;
        i = j;
        if (word != "u" && word != "bin") flush_bytes();
        control(word, param);
      } else if (next == '\'') {
        if (i + 3 < n) {
          const auto value = util::parse_int(rtf.substr(i + 2, 2), 16);
          if (value) raw_byte(static_cast<std::uint8_t>(*value));
        }
        i += 4;
      } else {
        symbol(next);
        i += 2;
      }
    } else {
      std::size_t j = i;
      while (j < n && rtf[j] != '{' && rtf[j] != '}' && rtf[j] != '\\') ++j;
      text(rtf.substr(i, j - i));
      i = j;
    }
  }
  flush_bytes();
  while (stories_.size() > 1) {
    // Unclosed footnote: finish it as text.
    stories_.pop_back();
  }
  end_paragraph(false);
  close_table();
  doc_.blocks = story().flow.take();
  merge_adjacent_blocks(doc_.blocks);
  doc_.footnotes = std::move(footnotes_);
  if (!settings_.metadata) doc_.metadata.extra.clear();
  for (auto it = doc_.metadata.extra.begin(); it != doc_.metadata.extra.end();) {
    it->second = std::string(util::trim(it->second));
    it = it->second.empty() ? doc_.metadata.extra.erase(it) : std::next(it);
  }
  doc_.metadata.title = std::string(util::trim(doc_.metadata.title));
  doc_.metadata.author = std::string(util::trim(doc_.metadata.author));
  finish_metadata(doc_, settings_);
  return std::move(doc_);
}

Result<AnyIR> read_rtf(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  std::size_t start = 0;
  if (text.starts_with("\xEF\xBB\xBF")) start = 3;
  if (!text.substr(start).starts_with("{\\rtf")) return fail(Status::Decode, "not an RTF document");
  RtfReader reader(ctx, read_settings(ctx.options));
  CVT_TRY_ASSIGN(ir::Document doc, reader.read(text.substr(start)));
  return AnyIR(std::move(doc));
}

// =========================================================================== writer

class RtfWriter {
 public:
  RtfWriter(const ir::Document& doc, WriteContext& ctx) : doc_(doc), ctx_(ctx) {
    policy_ = image_policy(ctx.options);
    page_ = page_setup(ctx.options);
  }
  Result<Bytes> write();

 private:
  void blocks(const ir::Blocks& items, int quote, int depth);
  void block(const ir::Block& b, int quote, int depth);
  void inlines(const ir::Inlines& items, int depth);
  void text(std::string_view utf8);
  void picture(const ir::ImageSource& source, std::string_view alt);
  void list(const ir::List& list, int quote, int level, int depth);
  void table(const ir::TableBlock& table, int quote, int depth);
  void para_start(std::string_view style_words, int quote);

  const ir::Document& doc_;
  WriteContext& ctx_;
  ImagePolicy policy_;
  PageSetup page_;
  std::string body_;
  std::string list_table_;
  std::string override_table_;
  int list_count_ = 0;
  int dropped_ = 0;
};

void RtfWriter::text(std::string_view utf8) {
  std::size_t i = 0;
  while (i < utf8.size()) {
    const util::Utf8Decoded d = util::decode_utf8(utf8, i);
    const char32_t cp = d.ok ? d.code_point : util::replacement_character;
    i += d.ok ? d.length : 1;
    if (cp == '\\' || cp == '{' || cp == '}') {
      body_.push_back('\\');
      body_.push_back(static_cast<char>(cp));
    } else if (cp == '\t') {
      body_ += "\\tab ";
    } else if (cp == '\n') {
      body_ += "\\line ";
    } else if (cp == '\r') {
      continue;
    } else if (cp < 0x20) {
      continue;
    } else if (cp < 0x80) {
      body_.push_back(static_cast<char>(cp));
    } else if (cp == 0xA0) {
      body_ += "\\~";
    } else if (cp <= 0xFFFF) {
      const int signed_value = cp > 32767 ? static_cast<int>(cp) - 65536 : static_cast<int>(cp);
      body_ += "\\u" + std::to_string(signed_value) + "?";
    } else {
      const char32_t v = cp - 0x10000;
      const int high = static_cast<int>(0xD800 + (v >> 10)) - 65536;
      const int low = static_cast<int>(0xDC00 + (v & 0x3FF)) - 65536;
      body_ += "\\u" + std::to_string(high) + "?\\u" + std::to_string(low) + "?";
    }
  }
}

void RtfWriter::picture(const ir::ImageSource& source, std::string_view alt) {
  auto prepared = prepare_image(source, policy_, ctx_);
  if (prepared && prepared->extension != "png" && prepared->extension != "jpg") prepared = convert_to_png(*prepared, ctx_);
  if (!prepared) {
    if (policy_.mode == ImageMode::Drop) ++dropped_;
    if (!alt.empty()) text(alt);
    return;
  }
  PixelSize px = prepared->size;
  if (px.width == 0 || px.height == 0) px = PixelSize{100, 100};
  double w = px.width * 15.0;  // twips at 96 dpi
  double h = px.height * 15.0;
  const double max_w = page_.width - 2.0 * page_.margin;
  if (w > max_w) {
    h *= max_w / w;
    w = max_w;
  }
  body_ += "{\\*\\shppict{\\pict";
  body_ += prepared->extension == "png" ? "\\pngblip" : "\\jpegblip";
  body_ += "\\picw" + std::to_string(px.width) + "\\pich" + std::to_string(px.height) + "\\picwgoal" +
           std::to_string(static_cast<int>(w)) + "\\pichgoal" + std::to_string(static_cast<int>(h)) + "\n";
  static constexpr char digits[] = "0123456789abcdef";
  std::size_t column = 0;
  for (std::byte b : prepared->bytes) {
    const auto v = std::to_integer<unsigned>(b);
    body_.push_back(digits[v >> 4]);
    body_.push_back(digits[v & 15]);
    if (++column == 64) {
      body_.push_back('\n');
      column = 0;
    }
  }
  body_ += "}}";
}

void RtfWriter::inlines(const ir::Inlines& items, int depth) {
  if (depth > ir::max_document_depth) return;
  for (const ir::Inline& item : items) {
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Text>) {
            text(node.text);
          } else if constexpr (std::same_as<T, ir::Code>) {
            body_ += "{\\f2 ";
            text(node.text);
            body_ += "}";
          } else if constexpr (std::same_as<T, ir::Emphasis>) {
            body_ += "{\\i ";
            inlines(node.children, depth + 1);
            body_ += "}";
          } else if constexpr (std::same_as<T, ir::Strong>) {
            body_ += "{\\b ";
            inlines(node.children, depth + 1);
            body_ += "}";
          } else if constexpr (std::same_as<T, ir::Underline>) {
            body_ += "{\\ul ";
            inlines(node.children, depth + 1);
            body_ += "}";
          } else if constexpr (std::same_as<T, ir::Strikethrough>) {
            body_ += "{\\strike ";
            inlines(node.children, depth + 1);
            body_ += "}";
          } else if constexpr (std::same_as<T, ir::Superscript>) {
            body_ += "{\\super ";
            inlines(node.children, depth + 1);
            body_ += "}";
          } else if constexpr (std::same_as<T, ir::Subscript>) {
            body_ += "{\\sub ";
            inlines(node.children, depth + 1);
            body_ += "}";
          } else if constexpr (std::same_as<T, ir::LineBreak>) {
            body_ += "\\line ";
          } else if constexpr (std::same_as<T, ir::InlineImage>) {
            picture(node.source, node.alt);
          } else if constexpr (std::same_as<T, ir::Link>) {
            if (!node.note.empty()) {
              const ir::Footnote* note = nullptr;
              for (const ir::Footnote& f : doc_.footnotes) {
                if (f.label == node.note) note = &f;
              }
              if (note && depth < 8) {
                body_ += "{\\super\\chftn}{\\footnote\\pard\\plain\\s20\\fs20 {\\super\\chftn} ";
                bool first = true;
                for (const ir::Block& b : note->blocks) {
                  if (!first) body_ += "\\par ";
                  first = false;
                  if (const auto* para = b.get_if<ir::Paragraph>()) {
                    inlines(para->content, depth + 4);
                  } else {
                    text(ir::plain_text(ir::Blocks{b}));
                  }
                }
                body_ += "}";
                return;
              }
            }
            if (node.url.empty()) {
              inlines(node.children, depth + 1);
              return;
            }
            std::string target = node.url;
            body_ += "{\\field{\\*\\fldinst{HYPERLINK ";
            if (target.starts_with('#')) {
              body_ += "\\\\l \"";
              target.erase(0, 1);
            } else {
              body_ += "\"";
            }
            text(target);
            body_ += "\"}}{\\fldrslt{\\ul\\cf2 ";
            inlines(node.children, depth + 1);
            body_ += "}}}";
          }
        },
        item.node);
  }
}

void RtfWriter::para_start(std::string_view style_words, int quote) {
  body_ += "\\pard\\plain ";
  body_ += style_words;
  if (quote > 0) body_ += "\\li" + std::to_string(720 * quote) + "\\ri" + std::to_string(quote > 0 ? 720 : 0);
  body_ += "\\fs22 ";
}

void RtfWriter::list(const ir::List& list, int quote, int level, int depth) {
  // One list-table entry per list, used for this level.
  const int id = ++list_count_;
  const int nfc = !list.ordered ? 23
                  : list.numbering == ir::ListNumbering::UpperRoman ? 1
                  : list.numbering == ir::ListNumbering::LowerRoman ? 2
                  : list.numbering == ir::ListNumbering::UpperAlpha ? 3
                  : list.numbering == ir::ListNumbering::LowerAlpha ? 4
                                                                    : 0;
  list_table_ += "{\\list\\listtemplateid" + std::to_string(1000 + id) + "\\listsimple";
  const int lvl = std::clamp(level, 0, 8);
  for (int i = 0; i < 9; ++i) {
    const bool mine = i == lvl;
    const int this_nfc = mine ? nfc : (list.ordered ? 0 : 23);
    list_table_ += "{\\listlevel\\levelnfc" + std::to_string(this_nfc) + "\\levelnfcn" + std::to_string(this_nfc) +
                   "\\leveljc0\\leveljcn0\\levelfollow0\\levelstartat" + std::to_string(mine ? std::max(0, list.start) : 1);
    if (this_nfc == 23) {
      list_table_ += "{\\leveltext\\leveltemplateid" + std::to_string(2000 + id * 10 + i) + "\\'01\\u8226 ?;}{\\levelnumbers;}";
    } else {
      list_table_ += "{\\leveltext\\leveltemplateid" + std::to_string(2000 + id * 10 + i) + "\\'02\\'0" +
                     std::to_string(i) + ".;}{\\levelnumbers\\'01;}";
    }
    list_table_ += "\\fi-360\\li" + std::to_string(720 * (i + 1)) + "}";
  }
  list_table_ += "{\\listname ;}\\listid" + std::to_string(id) + "}";
  override_table_ += "{\\listoverride\\listid" + std::to_string(id) + "\\listoverridecount0\\ls" + std::to_string(id) + "}";

  int number = list.start;
  for (std::size_t i = 0; i < list.items.size(); ++i) {
    const ir::Blocks& item = list.items[i];
    bool first = true;
    for (std::size_t b = 0; b < item.size(); ++b) {
      const ir::Block& inner = item[b];
      const auto* para = inner.get_if<ir::Paragraph>();
      if (const auto* nested = inner.get_if<ir::List>()) {
        this->list(*nested, quote, level + 1, depth + 1);
        continue;
      }
      if (!para) {
        block(inner, quote + level + 1, depth + 1);
        continue;
      }
      const int indent = 720 * (lvl + 1) + 720 * quote;
      if (first && !list.definition) {
        body_ += "\\pard\\plain\\ls" + std::to_string(id) + "\\ilvl" + std::to_string(lvl) + "\\fi-360\\li" +
                 std::to_string(indent) + "\\fs22 {\\listtext\\tab ";
        if (list.ordered) {
          body_ += std::to_string(number) + ".";
        } else {
          body_ += "\\u8226 ?";
        }
        body_ += "\\tab}";
        if (list.task(i) == ir::TaskState::Open) text("\xE2\x98\x90 ");
        if (list.task(i) == ir::TaskState::Done) text("\xE2\x98\x92 ");
      } else {
        para_start("", 0);
        body_ += "\\li" + std::to_string(list.definition && b == 0 ? 720 * quote : indent) + " ";
        if (list.definition && b == 0) body_ += "\\b ";
      }
      inlines(para->content, depth + 1);
      body_ += "\\par\n";
      first = false;
    }
    ++number;
  }
}

void RtfWriter::table(const ir::TableBlock& table, int quote, int depth) {
  const document::TableGrid grid = document::table_grid(table);
  if (grid.columns == 0) return;
  if (!table.caption.empty()) {
    para_start("\\s21\\i", quote);
    inlines(table.caption, depth + 1);
    body_ += "\\par\n";
  }
  const int width = page_.width - 2 * page_.margin - 720 * quote;
  const int column = std::max(200, width / static_cast<int>(grid.columns));
  for (std::size_t r = 0; r < grid.cells.size(); ++r) {
    const bool header = r < grid.header_rows;
    body_ += "\\trowd\\trgaph108\\trleft" + std::to_string(720 * quote);
    if (header) body_ += "\\trhdr";
    std::size_t c = 0;
    std::vector<std::pair<const ir::TableCell*, bool>> emitted;
    while (c < grid.columns) {
      const ir::TableCell* cell = grid.cells[r][c];
      std::size_t span = 1;
      while (cell && c + span < grid.columns && grid.cells[r][c + span] == cell) ++span;
      const bool origin = grid.origin[r][c];
      const bool vstart = cell && origin && cell->rowspan > 1;
      const bool vcont = cell && !origin && r > 0 && grid.cells[r - 1][c] == cell;
      for (std::size_t k = 0; k < span; ++k) {
        body_ += "\\clbrdrt\\brdrs\\brdrw10\\clbrdrl\\brdrs\\brdrw10\\clbrdrb\\brdrs\\brdrw10\\clbrdrr\\brdrs\\brdrw10";
        if (span > 1) body_ += k == 0 ? "\\clmgf" : "\\clmrg";
        if (vstart) body_ += "\\clvmgf";
        if (vcont) body_ += "\\clvmrg";
        body_ += "\\cellx" + std::to_string(720 * quote + column * static_cast<int>(c + k + 1));
        emitted.emplace_back(k == 0 && origin ? cell : nullptr, header);
      }
      c += span;
    }
    body_ += "\n";
    for (const auto& [cell, is_header] : emitted) {
      body_ += "\\pard\\plain\\intbl\\fs22 ";
      if (cell) {
        if (is_header) body_ += "{\\b ";
        inlines(cell->content, depth + 1);
        if (is_header) body_ += "}";
      }
      body_ += "\\cell\n";
    }
    body_ += "\\row\n";
  }
  body_ += "\\pard\\plain\\fs22\\par\n";
}

void RtfWriter::block(const ir::Block& b, int quote, int depth) {
  if (depth > ir::max_document_depth) return;
  std::visit(
      [&](const auto& node) {
        using T = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<T, ir::Paragraph>) {
          para_start(quote > 0 ? "\\s15\\i" : (b.attr.has_class("caption") ? "\\s21\\i" : ""), quote);
          if (!b.attr.id.empty()) {
            body_ += "{\\*\\bkmkstart ";
            text(b.attr.id);
            body_ += "}{\\*\\bkmkend ";
            text(b.attr.id);
            body_ += "}";
          }
          inlines(node.content, depth + 1);
          body_ += "\\par\n";
        } else if constexpr (std::same_as<T, ir::Heading>) {
          const int level = std::clamp(node.level, 1, 6);
          static constexpr int sizes[6] = {36, 30, 26, 24, 22, 22};
          body_ += "\\pard\\plain\\s" + std::to_string(level) + "\\outlinelevel" + std::to_string(level - 1) +
                   "\\keepn\\sb240\\sa120\\b\\fs" + std::to_string(sizes[level - 1]) + " ";
          if (!b.attr.id.empty()) {
            body_ += "{\\*\\bkmkstart ";
            text(b.attr.id);
            body_ += "}{\\*\\bkmkend ";
            text(b.attr.id);
            body_ += "}";
          }
          inlines(node.content, depth + 1);
          body_ += "\\par\n";
        } else if constexpr (std::same_as<T, ir::List>) {
          list(node, quote, 0, depth + 1);
        } else if constexpr (std::same_as<T, ir::CodeBlock>) {
          std::string_view code = node.code;
          while (code.ends_with('\n')) code.remove_suffix(1);
          for (std::string_view line : util::split(code, '\n')) {
            if (line.ends_with('\r')) line.remove_suffix(1);
            body_ += "\\pard\\plain\\s16\\f2\\fs20";
            if (quote > 0) body_ += "\\li" + std::to_string(720 * quote);
            body_ += " ";
            text(line);
            body_ += "\\par\n";
          }
        } else if constexpr (std::same_as<T, ir::BlockQuote>) {
          blocks(node.blocks, quote + 1, depth + 1);
        } else if constexpr (std::same_as<T, ir::TableBlock>) {
          table(node, quote, depth + 1);
        } else if constexpr (std::same_as<T, ir::ImageBlock>) {
          para_start("\\qc", quote);
          picture(node.source, node.alt);
          body_ += "\\par\n";
          if (!node.caption.empty()) {
            para_start("\\s21\\qc\\i", quote);
            text(node.caption);
            body_ += "\\par\n";
          }
        } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
          body_ += "\\pard\\plain\\brdrb\\brdrs\\brdrw10\\brsp20\\fs22\\par\n";
        } else if constexpr (std::same_as<T, ir::PageBreak>) {
          body_ += "\\pard\\plain\\fs22\\page\n";
        }
      },
      b.node);
}

void RtfWriter::blocks(const ir::Blocks& items, int quote, int depth) {
  for (const ir::Block& b : items) block(b, quote, depth);
}

Result<Bytes> RtfWriter::write() {
  const std::size_t total = doc_.blocks.size();
  for (std::size_t i = 0; i < total; ++i) {
    if ((i & 63) == 0) {
      CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(total, 1)), "Writing"));
    }
    block(doc_.blocks[i], 0, 0);
  }
  if (dropped_ > 0) ctx_.warn(std::to_string(dropped_) + " picture(s) were left out as requested.");
  std::string language = ctx_.options.get_string("language", "");
  if (language.empty()) language = doc_.metadata.language;
  std::string out =
      "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1\n"
      "{\\fonttbl{\\f0\\froman\\fcharset0 Times New Roman;}{\\f1\\fswiss\\fcharset0 Arial;}"
      "{\\f2\\fmodern\\fcharset0 Courier New;}}\n"
      "{\\colortbl;\\red0\\green0\\blue0;\\red5\\green99\\blue193;}\n"
      "{\\stylesheet{\\s0\\snext0\\fs22 Normal;}";
  static constexpr int sizes[6] = {36, 30, 26, 24, 22, 22};
  for (int level = 1; level <= 6; ++level) {
    out += "{\\s" + std::to_string(level) + "\\outlinelevel" + std::to_string(level - 1) + "\\keepn\\sb240\\sa120\\b\\fs" +
           std::to_string(sizes[level - 1]) + "\\sbasedon0\\snext0 heading " + std::to_string(level) + ";}";
  }
  out += "{\\s15\\li720\\ri720\\i\\fs22\\sbasedon0\\snext0 Quote;}{\\s16\\f2\\fs20\\sbasedon0\\snext16 Source Code;}"
         "{\\s20\\fs20\\sbasedon0\\snext20 footnote text;}{\\s21\\i\\fs18\\sbasedon0\\snext0 caption;}}\n";
  if (!list_table_.empty()) {
    out += "{\\*\\listtable" + list_table_ + "}\n{\\*\\listoverridetable" + override_table_ + "}\n";
  }
  const bool strip = options::resolved_strip_metadata(ctx_.options);
  if (!doc_.metadata.title.empty() || (!strip && !doc_.metadata.author.empty())) {
    std::string saved = std::move(body_);
    body_.clear();
    body_ += "{\\info";
    if (!doc_.metadata.title.empty()) {
      body_ += "{\\title ";
      text(doc_.metadata.title);
      body_ += "}";
    }
    if (!strip && !doc_.metadata.author.empty()) {
      body_ += "{\\author ";
      text(doc_.metadata.author);
      body_ += "}";
    }
    body_ += "}\n";
    out += body_;
    body_ = std::move(saved);
  }
  out += "\\paperw" + std::to_string(page_.width) + "\\paperh" + std::to_string(page_.height) + "\\margl" +
         std::to_string(page_.margin) + "\\margr" + std::to_string(page_.margin) + "\\margt" + std::to_string(page_.margin) +
         "\\margb" + std::to_string(page_.margin);
  if (page_.width > page_.height) out += "\\landscape";
  out += "\\fet0\\ftnbj\\sectd\n";
  out += body_;
  out += "}\n";
  (void)language;
  return to_bytes(out);
}

Result<Bytes> write_rtf(const AnyIR& value, WriteContext& ctx) {
  RtfWriter writer(value.get<ir::Document>(), ctx);
  return writer.write();
}

}  // namespace

Result<ir::Document> rtf_to_document(std::string_view rtf, const ReadContext& ctx) {
  std::size_t start = rtf.find("{\\rtf");
  if (start == std::string_view::npos) return fail(Status::Decode, "not RTF text");
  RtfReader reader(ctx, read_settings(ctx.options));
  return reader.read(rtf.substr(start));
}

void register_rtf(Registry& registry) {
  FormatInfo info;
  info.id = "rtf";
  info.name = "Rich Text Format";
  info.description = "Word-processing document in Microsoft's Rich Text Format.";
  info.category = Category::Document;
  info.extensions = {"rtf"};
  info.mime = "application/rtf";
  info.popularity = 60;
  info.signatures = {Signature::at(0, "'{\\rtf'"), Signature::at(0, "EF BB BF '{\\rtf'")};
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "rtf";
  codec.kind = Kind::Document;
  codec.read = read_rtf;
  codec.write = write_rtf;
  codec.read_options = document_read_options();
  codec.write_options = document_write_options(true);
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::office
