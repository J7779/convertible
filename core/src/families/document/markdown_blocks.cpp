// Convertible core - document family: Markdown block structure.
//
// The block parser follows the CommonMark specification's two-phase
// algorithm (container matching per line, then inline parsing of leaf
// content), including tab stops, lazy continuation lines, the seven HTML
// block kinds, link reference definitions and tight/loose list detection.
// Extensions (GFM and the "extended" flavour): pipe tables, task list items,
// footnote definitions, $$ math blocks, definition lists, heading attributes,
// GitHub alerts ("> [!NOTE]") and YAML/TOML front matter.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <memory>

#include "families/document/doc_common.hpp"
#include "families/document/document_api.hpp"
#include "families/document/markdown_internal.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace markdown {

namespace {

enum class BT : std::uint8_t {
  Document, BlockQuote, List, Item, Paragraph, Heading, ThematicBreak, CodeBlock, HtmlBlock, Table, FootnoteDef,
  DefList, DefItem, DefDetails, MathBlock,
};

struct ListData {
  bool ordered = false;
  char bullet = 0;
  char delimiter = 0;
  int start = 1;
  int marker_offset = 0;
  int padding = 0;
};

struct MdNode {
  BT type = BT::Document;
  MdNode* parent = nullptr;
  std::vector<std::unique_ptr<MdNode>> children;
  bool open = true;
  bool last_line_blank = false;
  bool last_line_checked = false;
  std::size_t start_line = 0;
  std::string content;
  ListData list;
  bool tight = true;
  bool fenced = false;
  char fence_char = 0;
  std::size_t fence_length = 0;
  std::size_t fence_offset = 0;
  std::string info;
  int level = 0;
  int html_type = 0;
  std::string label;
  std::vector<std::string> table_lines;  // header, then body rows (raw)
  std::vector<ir::Alignment> aligns;
  int depth = 0;

  [[nodiscard]] MdNode* last_child() const { return children.empty() ? nullptr : children.back().get(); }
};

bool can_contain(BT parent, BT child) {
  switch (parent) {
    case BT::Document:
    case BT::BlockQuote:
    case BT::Item:
    case BT::FootnoteDef:
    case BT::DefDetails: return child != BT::Item && child != BT::DefItem && child != BT::DefDetails;
    case BT::List: return child == BT::Item;
    case BT::DefList: return child == BT::DefItem;
    case BT::DefItem: return child == BT::DefDetails;
    default: return false;
  }
}

bool accepts_lines(BT t) { return t == BT::Paragraph || t == BT::CodeBlock || t == BT::HtmlBlock || t == BT::MathBlock; }

bool is_space_or_tab(char c) { return c == ' ' || c == '\t'; }

// ---- HTML block starts ----------------------------------------------------------

bool html_block_name(std::string_view name) {
  static constexpr std::array<std::string_view, 62> names = {
      "address", "article", "aside",   "base",     "basefont", "blockquote", "body",     "caption",  "center",
      "col",     "colgroup", "dd",     "details",  "dialog",   "dir",        "div",      "dl",       "dt",
      "fieldset", "figcaption", "figure", "footer", "form",    "frame",      "frameset", "h1",       "h2",
      "h3",      "h4",      "h5",      "h6",       "head",     "header",     "hr",       "html",     "iframe",
      "legend",  "li",      "link",    "main",     "menu",     "menuitem",   "nav",      "noframes", "ol",
      "optgroup", "option", "p",       "param",    "search",   "section",    "summary",  "table",    "tbody",
      "td",      "tfoot",   "th",      "thead",    "title",    "tr",         "track",    "ul"};
  return std::ranges::find(names, name) != names.end();
}

int html_block_start(std::string_view s, bool can_interrupt_paragraph) {
  if (!s.starts_with("<")) return 0;
  const std::string lowered = util::to_lower(s.substr(0, 16));
  for (const std::string_view tag : {"script", "pre", "textarea", "style"}) {
    if (std::string_view(lowered).substr(1).starts_with(tag)) {
      const std::size_t after = 1 + tag.size();
      if (after >= s.size() || util::is_ascii_space(s[after]) || s[after] == '>') return 1;
    }
  }
  if (s.starts_with("<!--")) return 2;
  if (s.starts_with("<?")) return 3;
  if (s.size() >= 3 && s[1] == '!' && util::is_ascii_alpha(s[2])) return 4;
  if (s.starts_with("<![CDATA[")) return 5;
  {
    std::size_t i = 1;
    if (i < s.size() && s[i] == '/') ++i;
    const std::size_t name_start = i;
    while (i < s.size() && (util::is_ascii_alpha(s[i]) || util::is_ascii_digit(s[i]))) ++i;
    const std::string name = util::to_lower(s.substr(name_start, i - name_start));
    if (!name.empty() && html_block_name(name)) {
      if (i >= s.size() || util::is_ascii_space(s[i]) || s[i] == '>' || s.substr(i).starts_with("/>")) return 6;
    }
  }
  if (!can_interrupt_paragraph) {
    std::size_t length = match_open_tag(s, 0);
    if (length == 0) length = match_close_tag(s, 0);
    if (length > 0 && util::trim(s.substr(length)).empty()) {
      const std::string name = util::to_lower(s.substr(s[1] == '/' ? 2 : 1, 8));
      if (!name.starts_with("script") && !name.starts_with("style") && !name.starts_with("pre") &&
          !name.starts_with("textarea")) {
        return 7;
      }
    }
  }
  return 0;
}

bool html_block_ends(int type, std::string_view line) {
  const std::string lowered = util::to_lower(line);
  switch (type) {
    case 1:
      return lowered.find("</script>") != std::string::npos || lowered.find("</pre>") != std::string::npos ||
             lowered.find("</style>") != std::string::npos || lowered.find("</textarea>") != std::string::npos;
    case 2: return line.find("-->") != std::string_view::npos;
    case 3: return line.find("?>") != std::string_view::npos;
    case 4: return line.find('>') != std::string_view::npos;
    case 5: return line.find("]]>") != std::string_view::npos;
    default: return false;
  }
}

// ---- tables ------------------------------------------------------------------------

std::vector<std::string> split_table_row(std::string_view line) {
  std::string_view t = util::trim(line);
  if (t.starts_with("|")) t.remove_prefix(1);
  if (t.ends_with("|") && !(t.size() >= 2 && t[t.size() - 2] == '\\')) t.remove_suffix(1);
  std::vector<std::string> cells;
  std::string cell;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
      cell.push_back('|');
      ++i;
      continue;
    }
    if (t[i] == '\\' && i + 1 < t.size()) {
      cell.push_back(t[i]);
      cell.push_back(t[i + 1]);
      ++i;
      continue;
    }
    if (t[i] == '|') {
      cells.push_back(std::string(util::trim(cell)));
      cell.clear();
      continue;
    }
    cell.push_back(t[i]);
  }
  cells.push_back(std::string(util::trim(cell)));
  return cells;
}

std::optional<std::vector<ir::Alignment>> parse_delimiter_row(std::string_view line) {
  const std::string_view t = util::trim(line);
  if (t.empty() || t.find('-') == std::string_view::npos) return std::nullopt;
  if (t.find('|') == std::string_view::npos) {
    // A single-column delimiter row needs a pipe.
    return std::nullopt;
  }
  std::vector<ir::Alignment> aligns;
  for (const std::string& raw : split_table_row(t)) {
    const std::string_view c = util::trim(raw);
    if (c.empty()) return std::nullopt;
    const bool left = c.front() == ':';
    const bool right = c.back() == ':';
    const std::string_view dashes = c.substr(left ? 1 : 0, c.size() - (left ? 1 : 0) - (right && c.size() > 1 ? 1 : 0));
    if (dashes.empty() || !std::ranges::all_of(dashes, [](char x) { return x == '-'; })) return std::nullopt;
    aligns.push_back(left && right ? ir::Alignment::Center
                     : right       ? ir::Alignment::Right
                     : left        ? ir::Alignment::Left
                                   : ir::Alignment::Default);
  }
  return aligns;
}

// ---- the parser ----------------------------------------------------------------------

class BlockParser {
 public:
  BlockParser(ParseState& state, int max_nesting) : state_(state), max_nesting_(std::clamp(max_nesting, 2, 64)) {
    root_ = std::make_unique<MdNode>();
    tip_ = root_.get();
  }

  std::unique_ptr<MdNode> parse(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size()) {
      std::size_t end = text.find_first_of("\r\n", start);
      if (end == std::string_view::npos) end = text.size();
      incorporate_line(text.substr(start, end - start));
      if (end < text.size() && text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n') ++end;
      start = end + 1;
    }
    while (tip_ != nullptr) finalize(tip_);
    return std::move(root_);
  }

 private:
  // ---- line state ----
  void find_next_nonspace() {
    std::size_t i = offset_;
    std::size_t cols = column_;
    char c = 0;
    while (i < line_.size()) {
      c = line_[i];
      if (c == ' ') {
        ++i;
        ++cols;
      } else if (c == '\t') {
        ++i;
        cols += 4 - cols % 4;
      } else {
        break;
      }
    }
    blank_ = i >= line_.size();
    next_nonspace_ = i;
    next_nonspace_column_ = cols;
    indent_ = next_nonspace_column_ - column_;
    indented_ = indent_ >= 4;
  }

  void advance_offset(std::size_t count, bool columns) {
    while (count > 0 && offset_ < line_.size()) {
      const char c = line_[offset_];
      if (c == '\t') {
        const std::size_t to_tab = 4 - column_ % 4;
        if (columns) {
          partially_consumed_tab_ = to_tab > count;
          const std::size_t advance = std::min(to_tab, count);
          column_ += advance;
          offset_ += partially_consumed_tab_ ? 0 : 1;
          count -= advance;
        } else {
          partially_consumed_tab_ = false;
          column_ += to_tab;
          offset_ += 1;
          count -= 1;
        }
      } else {
        partially_consumed_tab_ = false;
        offset_ += 1;
        column_ += 1;
        count -= 1;
      }
    }
  }

  void advance_next_nonspace() {
    offset_ = next_nonspace_;
    column_ = next_nonspace_column_;
    partially_consumed_tab_ = false;
  }

  [[nodiscard]] char peek(std::size_t at) const { return at < line_.size() ? line_[at] : '\0'; }
  [[nodiscard]] std::string_view rest() const { return std::string_view(line_).substr(std::min(next_nonspace_, line_.size())); }

  void add_line() {
    if (partially_consumed_tab_) {
      offset_ += 1;
      const std::size_t to_tab = 4 - column_ % 4;
      tip_->content.append(to_tab, ' ');
    }
    tip_->content.append(std::string_view(line_).substr(std::min(offset_, line_.size())));
    tip_->content.push_back('\n');
  }

  MdNode* add_child(BT type, std::size_t /*offset*/) {
    while (!can_contain(tip_->type, type)) finalize(tip_);
    auto node = std::make_unique<MdNode>();
    node->type = type;
    node->parent = tip_;
    node->start_line = line_number_;
    node->depth = tip_->depth + 1;
    MdNode* raw = node.get();
    tip_->children.push_back(std::move(node));
    tip_ = raw;
    return raw;
  }

  void close_unmatched() {
    if (!all_closed_) {
      while (old_tip_ != last_matched_) {
        MdNode* parent = old_tip_->parent;
        finalize(old_tip_);
        old_tip_ = parent;
      }
      all_closed_ = true;
    }
  }

  // ---- continuation -------------------------------------------------------------
  // 0 = matched, 1 = not matched, 2 = line consumed
  int continue_block(MdNode* c) {
    switch (c->type) {
      case BT::Document:
      case BT::List: return 0;
      case BT::DefList:
      case BT::DefItem:
        if (blank_ || indent_ >= 2) return 0;
        return peek(next_nonspace_) == ':' ? 0 : 1;
      case BT::BlockQuote:
        if (!indented_ && peek(next_nonspace_) == '>') {
          advance_next_nonspace();
          advance_offset(1, false);
          if (is_space_or_tab(peek(offset_))) advance_offset(1, true);
          return 0;
        }
        return 1;
      case BT::Item:
      case BT::DefDetails:
      case BT::FootnoteDef: {
        const std::size_t need = c->type == BT::FootnoteDef ? 4 : static_cast<std::size_t>(c->list.marker_offset + c->list.padding);
        if (blank_) {
          if (c->children.empty() && c->type != BT::FootnoteDef) return 1;
          advance_next_nonspace();
          return 0;
        }
        if (indent_ >= need) {
          advance_offset(need, true);
          return 0;
        }
        return 1;
      }
      case BT::Heading:
      case BT::ThematicBreak: return 1;
      case BT::CodeBlock:
        if (c->fenced) {
          const std::string_view r = rest();
          if (indent_ <= 3 && !r.empty() && r.front() == c->fence_char) {
            std::size_t n = 0;
            while (n < r.size() && r[n] == c->fence_char) ++n;
            if (n >= c->fence_length && util::trim(r.substr(n)).empty()) {
              finalize(c);
              return 2;
            }
          }
          std::size_t i = c->fence_offset;
          while (i > 0 && is_space_or_tab(peek(offset_))) {
            advance_offset(1, true);
            --i;
          }
          return 0;
        }
        if (indent_ >= 4) {
          advance_offset(4, true);
          return 0;
        }
        if (blank_) {
          advance_next_nonspace();
          return 0;
        }
        return 1;
      case BT::MathBlock: {
        const std::string_view r = util::trim(rest());
        if (r.ends_with("$$")) {
          const std::string_view before = r.substr(0, r.size() - 2);
          if (!before.empty()) {
            c->content.append(before);
            c->content.push_back('\n');
          }
          finalize(c);
          return 2;
        }
        return 0;
      }
      case BT::HtmlBlock: return blank_ && (c->html_type == 6 || c->html_type == 7) ? 1 : 0;
      case BT::Paragraph: return blank_ ? 1 : 0;
      case BT::Table: return blank_ ? 1 : 0;
    }
    return 1;
  }

  // ---- block starts --------------------------------------------------------------
  // 0 = no match, 1 = container started, 2 = leaf started
  int try_starts(MdNode*& container) {
    const std::string_view r = rest();
    const bool nesting_ok = container->depth < max_nesting_;
    // Block quote.
    if (!indented_ && peek(next_nonspace_) == '>' && nesting_ok) {
      advance_next_nonspace();
      advance_offset(1, false);
      if (is_space_or_tab(peek(offset_))) advance_offset(1, true);
      close_unmatched();
      container = add_child(BT::BlockQuote, next_nonspace_);
      return 1;
    }
    // ATX heading.
    if (!indented_ && r.starts_with("#")) {
      std::size_t level = 0;
      while (level < r.size() && r[level] == '#') ++level;
      if (level <= 6 && (level == r.size() || is_space_or_tab(r[level]))) {
        advance_next_nonspace();
        advance_offset(level, false);
        close_unmatched();
        MdNode* heading = add_child(BT::Heading, next_nonspace_);
        heading->level = static_cast<int>(level);
        std::string_view content = util::trim(std::string_view(line_).substr(std::min(offset_, line_.size())));
        // Closing sequence: spaces then #s at the end (or only #s).
        std::size_t end = content.size();
        while (end > 0 && content[end - 1] == '#') --end;
        if (end == 0) {
          content = {};
        } else if (end < content.size() && is_space_or_tab(content[end - 1])) {
          content = util::trim_right(content.substr(0, end));
        }
        heading->content = std::string(content);
        advance_offset(line_.size() - offset_, false);
        return 3;
      }
    }
    // Fenced code.
    if (!indented_ && (r.starts_with("```") || r.starts_with("~~~"))) {
      const char fence = r.front();
      std::size_t n = 0;
      while (n < r.size() && r[n] == fence) ++n;
      const std::string_view info = r.substr(n);
      if (fence != '`' || info.find('`') == std::string_view::npos) {
        close_unmatched();
        MdNode* code = add_child(BT::CodeBlock, next_nonspace_);
        code->fenced = true;
        code->fence_char = fence;
        code->fence_length = n;
        code->fence_offset = indent_;
        code->info = unescape_string(util::trim(info));
        advance_next_nonspace();
        advance_offset(line_.size() - offset_, false);
        return 2;
      }
    }
    // Math block ($$ on its own line, or $$ ... $$ on one line).
    if (!indented_ && state_.gfm() && r.starts_with("$$")) {
      const std::string_view after = util::trim(r.substr(2));
      close_unmatched();
      MdNode* math = add_child(BT::MathBlock, next_nonspace_);
      if (after.size() >= 2 && after.ends_with("$$")) {
        math->content = std::string(util::trim(after.substr(0, after.size() - 2)));
        finalize(math);
      } else if (!after.empty()) {
        math->content = std::string(after) + "\n";
      }
      advance_offset(line_.size() - offset_, false);
      return 3;
    }
    // HTML block.
    if (!indented_ && r.starts_with("<")) {
      const bool paragraph_tip = container->type == BT::Paragraph || (!all_closed_ && !blank_ && tip_->type == BT::Paragraph);
      const int type = html_block_start(r, paragraph_tip);
      if (type > 0) {
        close_unmatched();
        MdNode* block = add_child(BT::HtmlBlock, offset_);
        block->html_type = type;
        return 2;
      }
    }
    // Setext heading underline.
    if (!indented_ && container->type == BT::Paragraph && (r.starts_with("=") || r.starts_with("-"))) {
      const char c = r.front();
      std::size_t n = 0;
      while (n < r.size() && r[n] == c) ++n;
      if (util::trim(r.substr(n)).empty()) {
        close_unmatched();
        strip_references(container);
        if (!util::trim(container->content).empty()) {
          container->type = BT::Heading;
          container->level = c == '=' ? 1 : 2;
          std::string content(util::trim(container->content));
          container->content = std::move(content);
          container->open = false;
          tip_ = container->parent;
          advance_offset(line_.size() - offset_, false);
          return 3;
        }
      }
    }
    // GFM table: a delimiter row right after a one-line header paragraph.
    if (state_.gfm() && container->type == BT::Paragraph && !indented_) {
      if (auto aligns = parse_delimiter_row(r)) {
        std::string_view content = container->content;
        while (content.ends_with('\n')) content.remove_suffix(1);
        const std::size_t last_nl = content.rfind('\n');
        const std::string_view header = last_nl == std::string_view::npos ? content : content.substr(last_nl + 1);
        if (split_table_row(header).size() == aligns->size() && !header.empty()) {
          close_unmatched();
          std::string remaining(last_nl == std::string_view::npos ? std::string_view{} : content.substr(0, last_nl + 1));
          MdNode* paragraph = container;
          MdNode* parent = paragraph->parent;
          std::string header_line(header);
          if (util::trim(remaining).empty()) {
            // The whole paragraph becomes the table.
            parent->children.pop_back();
            tip_ = parent;
          } else {
            paragraph->content = remaining;
            finalize(paragraph);
          }
          MdNode* table = add_child(BT::Table, next_nonspace_);
          table->aligns = std::move(*aligns);
          table->table_lines.push_back(std::move(header_line));
          advance_offset(line_.size() - offset_, false);
          container = table;
          return 3;
        }
      }
    }
    // Thematic break.
    if (!indented_ && (r.starts_with("*") || r.starts_with("-") || r.starts_with("_"))) {
      const char c = r.front();
      std::size_t count = 0;
      bool ok = true;
      for (const char x : r) {
        if (x == c) {
          ++count;
        } else if (!is_space_or_tab(x)) {
          ok = false;
          break;
        }
      }
      if (ok && count >= 3) {
        close_unmatched();
        add_child(BT::ThematicBreak, next_nonspace_);
        advance_offset(line_.size() - offset_, false);
        return 3;
      }
    }
    // Footnote definition "[^label]: text".
    if (!indented_ && state_.gfm() && r.starts_with("[^") && nesting_ok) {
      const std::size_t close = r.find("]:");
      if (close != std::string_view::npos && close > 2) {
        const std::string_view label = r.substr(2, close - 2);
        if (std::ranges::none_of(label, [](char x) { return util::is_ascii_space(x) || x == ']' || x == '['; })) {
          close_unmatched();
          MdNode* note = add_child(BT::FootnoteDef, next_nonspace_);
          note->label = std::string(label);
          advance_next_nonspace();
          advance_offset(close + 2, false);
          if (is_space_or_tab(peek(offset_))) advance_offset(1, true);
          container = note;
          return 1;
        }
      }
    }
    // Definition list ": definition" after a term paragraph (or another
    // definition of the current term).
    if (!indented_ && state_.extended() && nesting_ok && (r.starts_with(": ") || r.starts_with(":\t") || r == ":")) {
      MdNode* item = nullptr;
      if (container->type == BT::Paragraph && all_closed_ && single_line(container->content)) {
        MdNode* term = container;
        MdNode* parent = term->parent;
        term->open = false;
        std::unique_ptr<MdNode> owned = std::move(parent->children.back());
        parent->children.pop_back();
        tip_ = parent;
        MdNode* list = parent->last_child();
        if (list != nullptr && list->type == BT::DefList) {
          list->open = true;
          tip_ = list;
        } else {
          list = add_child(BT::DefList, next_nonspace_);
        }
        item = add_child(BT::DefItem, next_nonspace_);
        owned->parent = item;
        owned->depth = item->depth + 1;
        item->children.push_back(std::move(owned));
      } else if (container->type == BT::DefItem) {
        close_unmatched();
        item = container;
      } else if (container->type == BT::DefList && container->last_child() != nullptr) {
        close_unmatched();
        item = container->last_child();
        item->open = true;
      }
      if (item != nullptr) {
        tip_ = item;
        MdNode* details = add_child(BT::DefDetails, next_nonspace_);
        advance_next_nonspace();
        const std::size_t marker_column = column_;
        advance_offset(1, true);
        std::size_t spaces = 0;
        while (spaces < 4 && is_space_or_tab(peek(offset_))) {
          advance_offset(1, true);
          ++spaces;
        }
        details->list.marker_offset = 0;
        details->list.padding = static_cast<int>(std::max<std::size_t>(column_ - marker_column, 2));
        container = details;
        return 1;
      }
    }
    // List item.
    if ((!indented_ || container->type == BT::List) && nesting_ok) {
      if (auto data = parse_list_marker(container)) {
        close_unmatched();
        if (tip_->type != BT::List || !lists_match(tip_->list, *data)) {
          MdNode* list = add_child(BT::List, next_nonspace_);
          list->list = *data;
        }
        MdNode* item = add_child(BT::Item, next_nonspace_);
        item->list = *data;
        container = item;
        return 1;
      }
    }
    // Indented code.
    if (indented_ && tip_->type != BT::Paragraph && !blank_) {
      advance_offset(4, true);
      close_unmatched();
      add_child(BT::CodeBlock, offset_);
      return 2;
    }
    return 0;
  }

  static bool single_line(std::string_view content) {
    std::string_view t = content;
    while (t.ends_with('\n')) t.remove_suffix(1);
    return t.find('\n') == std::string_view::npos;
  }

  static bool lists_match(const ListData& a, const ListData& b) {
    return a.ordered == b.ordered && a.delimiter == b.delimiter && a.bullet == b.bullet;
  }

  std::optional<ListData> parse_list_marker(MdNode* container) {
    if (indent_ >= 4) return std::nullopt;
    const std::string_view r = rest();
    ListData data;
    data.marker_offset = static_cast<int>(indent_);
    std::size_t marker_length = 0;
    if (!r.empty() && (r[0] == '*' || r[0] == '+' || r[0] == '-')) {
      data.bullet = r[0];
      marker_length = 1;
    } else {
      std::size_t digits = 0;
      while (digits < r.size() && digits < 10 && util::is_ascii_digit(r[digits])) ++digits;
      if (digits == 0 || digits > 9 || digits >= r.size() || (r[digits] != '.' && r[digits] != ')')) return std::nullopt;
      const int start = static_cast<int>(util::parse_int(r.substr(0, digits)).value_or(1));
      if (container->type == BT::Paragraph && start != 1) return std::nullopt;
      data.ordered = true;
      data.start = start;
      data.delimiter = r[digits];
      marker_length = digits + 1;
    }
    const char next = peek(next_nonspace_ + marker_length);
    if (!(next == '\0' || next == ' ' || next == '\t')) return std::nullopt;
    if (container->type == BT::Paragraph && util::trim(r.substr(marker_length)).empty()) return std::nullopt;
    advance_next_nonspace();
    advance_offset(marker_length, true);
    const std::size_t spaces_start_column = column_;
    const std::size_t spaces_start_offset = offset_;
    do {
      advance_offset(1, true);
    } while (column_ - spaces_start_column < 5 && is_space_or_tab(peek(offset_)));
    const bool blank_item = offset_ >= line_.size();
    const std::size_t spaces_after = column_ - spaces_start_column;
    if (spaces_after >= 5 || spaces_after < 1 || blank_item) {
      data.padding = static_cast<int>(marker_length + 1);
      column_ = spaces_start_column;
      offset_ = spaces_start_offset;
      partially_consumed_tab_ = false;
      if (is_space_or_tab(peek(offset_))) advance_offset(1, true);
    } else {
      data.padding = static_cast<int>(marker_length + spaces_after);
    }
    return data;
  }

  // ---- per line ----------------------------------------------------------------------

  void incorporate_line(std::string_view raw) {
    line_.assign(raw);
    // NUL is replaced for security (CommonMark 2.3).
    for (char& c : line_) {
      if (c == '\0') c = '?';
    }
    ++line_number_;
    offset_ = 0;
    column_ = 0;
    blank_ = false;
    partially_consumed_tab_ = false;
    old_tip_ = tip_;
    bool all_matched = true;
    MdNode* container = root_.get();
    while (MdNode* last = container->last_child()) {
      if (!last->open) break;
      container = last;
      find_next_nonspace();
      const int result = continue_block(container);
      if (result == 1) {
        container = container->parent;
        all_matched = false;
        break;
      }
      if (result == 2) {
        blank_before_ = false;
        return;
      }
    }
    (void)all_matched;
    all_closed_ = container == old_tip_;
    last_matched_ = container;
    bool matched_leaf = container->type != BT::Paragraph && container->type != BT::Table && accepts_lines(container->type);
    while (!matched_leaf) {
      find_next_nonspace();
      const int result = try_starts(container);
      if (result == 0) {
        advance_next_nonspace();
        break;
      }
      if (result == 3) {
        // The start consumed the whole line (heading, rule, table, math).
        blank_before_ = false;
        return;
      }
      container = tip_;
      if (result == 2) {
        matched_leaf = true;
        break;
      }
    }
    if (matched_leaf && container->type == BT::CodeBlock && container->fenced && container->start_line == line_number_) {
      blank_before_ = false;
      return;
    }
    // Remaining text.
    if (!all_closed_ && !blank_ && tip_->type == BT::Paragraph) {
      add_line();  // lazy continuation
    } else {
      close_unmatched();
      if (blank_ && container->last_child() != nullptr) container->last_child()->last_line_blank = true;
      const BT t = container->type;
      const bool last_line_blank =
          blank_ && !(t == BT::BlockQuote || (t == BT::CodeBlock && container->fenced) ||
                      (t == BT::Item && container->children.empty() && container->start_line == line_number_));
      for (MdNode* cont = container; cont != nullptr; cont = cont->parent) cont->last_line_blank = last_line_blank;
      if (t == BT::Table && !blank_) {
        if (offset_ < line_.size()) container->table_lines.emplace_back(std::string_view(line_).substr(offset_));
      } else if (accepts_lines(t)) {
        add_line();
        if (t == BT::HtmlBlock && container->html_type >= 1 && container->html_type <= 5 &&
            html_block_ends(container->html_type, std::string_view(line_).substr(std::min(offset_, line_.size())))) {
          finalize(container);
        }
      } else if (offset_ < line_.size() && !blank_) {
        add_child(BT::Paragraph, offset_);
        advance_next_nonspace();
        add_line();
      }
    }
    blank_before_ = blank_;
  }

  // ---- finalisation ----------------------------------------------------------------

  void strip_references(MdNode* paragraph) {
    while (!paragraph->content.empty() && util::trim_left(paragraph->content).starts_with("[")) {
      const std::string_view trimmed = util::trim_left(paragraph->content);
      const std::size_t lead = paragraph->content.size() - trimmed.size();
      const std::size_t used = parse_reference_definition(trimmed, state_);
      if (used == 0) break;
      paragraph->content.erase(0, lead + used);
    }
  }

  bool ends_with_blank_line(MdNode* block) {
    while (block != nullptr) {
      if (block->last_line_blank) return true;
      if (!block->last_line_checked && (block->type == BT::List || block->type == BT::Item)) {
        block->last_line_checked = true;
        block = block->last_child();
      } else {
        block->last_line_checked = true;
        break;
      }
    }
    return false;
  }

  void finalize(MdNode* block) {
    MdNode* above = block->parent;
    block->open = false;
    switch (block->type) {
      case BT::Paragraph:
        strip_references(block);
        if (util::trim(block->content).empty()) {
          // Only reference definitions: remove the paragraph.
          if (above != nullptr) {
            auto& siblings = above->children;
            const auto it = std::ranges::find_if(siblings, [&](const auto& p) { return p.get() == block; });
            if (it != siblings.end()) siblings.erase(it);
          }
        }
        break;
      case BT::CodeBlock:
        if (!block->fenced) {
          // Trailing blank lines are not part of indented code.
          std::string& c = block->content;
          while (true) {
            if (c.empty()) break;
            std::size_t end = c.size();
            if (c.back() == '\n') --end;
            const std::size_t line_start = c.rfind('\n', end == 0 ? 0 : end - 1);
            const std::size_t from = line_start == std::string::npos ? 0 : line_start + 1;
            if (end > from && !util::trim(std::string_view(c).substr(from, end - from)).empty()) break;
            if (from == 0 && util::trim(c).empty()) {
              c.clear();
              break;
            }
            c.erase(from);
            if (from == 0) break;
          }
        }
        break;
      case BT::List: {
        bool tight = true;
        for (std::size_t i = 0; i < block->children.size() && tight; ++i) {
          MdNode* item = block->children[i].get();
          if (ends_with_blank_line(item) && i + 1 < block->children.size()) {
            tight = false;
            break;
          }
          for (std::size_t j = 0; j < item->children.size(); ++j) {
            if (ends_with_blank_line(item->children[j].get()) &&
                (i + 1 < block->children.size() || j + 1 < item->children.size())) {
              tight = false;
              break;
            }
          }
        }
        block->tight = tight;
        break;
      }
      default: break;
    }
    tip_ = above;
  }

  ParseState& state_;
  int max_nesting_;
  std::unique_ptr<MdNode> root_;
  MdNode* tip_ = nullptr;
  MdNode* old_tip_ = nullptr;
  MdNode* last_matched_ = nullptr;
  std::string line_;
  std::size_t line_number_ = 0;
  std::size_t offset_ = 0;
  std::size_t column_ = 0;
  std::size_t next_nonspace_ = 0;
  std::size_t next_nonspace_column_ = 0;
  std::size_t indent_ = 0;
  bool indented_ = false;
  bool blank_ = false;
  bool partially_consumed_tab_ = false;
  bool all_closed_ = true;
  bool blank_before_ = false;
};

// ---- conversion to the IR --------------------------------------------------------------

std::string strip_trailing_newline(std::string text) {
  while (text.ends_with('\n')) text.pop_back();
  return text;
}

// "{#id .class key=value}" at the end of a heading (extended flavour).
ir::Attributes take_heading_attributes(std::string& content) {
  ir::Attributes attr;
  std::string_view t = util::trim_right(content);
  if (!t.ends_with("}")) return attr;
  const std::size_t open = t.rfind('{');
  if (open == std::string_view::npos || (open > 0 && !util::is_ascii_space(t[open - 1]) && open != 0)) return attr;
  const std::string_view inner = t.substr(open + 1, t.size() - open - 2);
  ir::Attributes parsed;
  for (const std::string_view part : util::split(inner, ' ', false)) {
    if (part.starts_with("#") && part.size() > 1) {
      parsed.id = std::string(part.substr(1));
    } else if (part.starts_with(".") && part.size() > 1) {
      parsed.classes.emplace_back(part.substr(1));
    } else if (const std::size_t eq = part.find('='); eq != std::string_view::npos && eq > 0) {
      std::string value(part.substr(eq + 1));
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
      parsed.pairs.emplace_back(std::string(part.substr(0, eq)), value);
    } else if (part == "-") {
      parsed.classes.emplace_back("unnumbered");
    } else {
      return attr;  // not an attribute block
    }
  }
  content = std::string(util::trim_right(t.substr(0, open)));
  return parsed;
}

class Converter {
 public:
  explicit Converter(ParseState& state) : state_(state) {}

  void convert(MdNode& root, ir::Document& document) {
    // Footnote definitions anywhere in the tree are collected first so that
    // references can be resolved while parsing inlines.
    collect_footnote_labels(root, 0);
    document.blocks = blocks(root, 0);
    for (auto& [label, blocks] : notes_) {
      if (!state_.footnotes_used.contains(label)) continue;
      ir::Footnote note;
      note.label = state_.footnote_labels[label];
      note.blocks = std::move(blocks);
      document.footnotes.push_back(std::move(note));
    }
  }

 private:
  void collect_footnote_labels(MdNode& node, int depth) {
    if (depth > 128) return;
    for (auto& child : node.children) {
      if (child->type == BT::FootnoteDef) {
        const std::string key = normalize_label(child->label);
        state_.footnote_labels.emplace(key, child->label);
      }
      collect_footnote_labels(*child, depth + 1);
    }
  }

  ir::Blocks blocks(MdNode& node, int depth) {
    ir::Blocks out;
    for (auto& child : node.children) convert_block(*child, out, depth + 1);
    return out;
  }

  ir::Inlines inlines(std::string_view text) { return parse_inlines(util::trim(text), state_); }

  void convert_block(MdNode& node, ir::Blocks& out, int depth) {
    switch (node.type) {
      case BT::Paragraph: {
        ir::Inlines content = inlines(node.content);
        if (content.empty()) return;
        // A lone picture is a picture block.
        if (content.size() == 1) {
          if (auto* image = content.front().get_if<ir::InlineImage>()) {
            out.push_back(ir::ImageBlock{std::move(image->source), std::move(image->alt), {}, std::move(image->title)});
            return;
          }
        }
        // Pandoc table caption "Table: ..." right after a table.
        if (state_.extended() && !out.empty() && out.back().is<ir::TableBlock>()) {
          if (auto* first = content.front().get_if<ir::Text>(); first != nullptr && first->text.starts_with("Table: ")) {
            first->text.erase(0, 7);
            out.back().get_if<ir::TableBlock>()->caption = std::move(content);
            return;
          }
        }
        out.push_back(ir::Paragraph{std::move(content)});
        return;
      }
      case BT::Heading: {
        std::string content = node.content;
        ir::Attributes attr;
        if (state_.extended()) attr = take_heading_attributes(content);
        out.push_back(ir::Block(ir::Heading{node.level, inlines(content)}, std::move(attr)));
        return;
      }
      case BT::ThematicBreak: out.push_back(ir::HorizontalRule{}); return;
      case BT::CodeBlock: {
        ir::CodeBlock code;
        code.code = strip_trailing_newline(node.content);
        std::string info = node.info;
        std::string language = info.substr(0, info.find_first_of(" \t"));
        if (language.starts_with("{") && language.ends_with("}")) {
          // {=latex} raw block, {.python} class.
          const std::string inner = language.substr(1, language.size() - 2);
          if (inner.starts_with("=")) {
            code.raw_format = inner.substr(1);
            language.clear();
          } else if (inner.starts_with(".")) {
            language = inner.substr(1);
          }
        }
        if (language == "math" && state_.gfm()) {
          code.math = ir::MathMode::Display;
          language.clear();
        }
        code.language = std::move(language);
        out.push_back(std::move(code));
        return;
      }
      case BT::MathBlock: {
        ir::CodeBlock math;
        math.code = std::string(util::trim(node.content));
        math.math = ir::MathMode::Display;
        out.push_back(std::move(math));
        return;
      }
      case BT::HtmlBlock: {
        ir::CodeBlock raw;
        raw.code = strip_trailing_newline(node.content);
        raw.raw_format = "html";
        // Script and style blocks stay raw; other HTML becomes structure.
        const std::string lowered = util::to_lower(std::string_view(raw.code).substr(0, 16));
        const bool code_like = lowered.starts_with("<script") || lowered.starts_with("<style") ||
                               lowered.starts_with("<?") || (lowered.starts_with("<!") && !lowered.starts_with("<!--"));
        if (state_.options != nullptr && state_.options->parse_html_blocks && !code_like && depth < 16) {
          HtmlReadOptions html;
          html.read_metadata = false;
          html.resolve = state_.options->resolve;
          if (auto parsed = parse_html_text(raw.code, html)) {
            for (ir::Block& b : parsed->blocks) out.push_back(std::move(b));
            return;
          }
        }
        out.push_back(std::move(raw));
        return;
      }
      case BT::BlockQuote: {
        ir::BlockQuote quote{blocks(node, depth)};
        ir::Attributes attr;
        // GitHub alert: the first paragraph starts with "[!NOTE]".
        if (state_.gfm() && !quote.blocks.empty()) {
          if (auto* p = quote.blocks.front().get_if<ir::Paragraph>(); p != nullptr && !p->content.empty()) {
            if (auto* t = p->content.front().get_if<ir::Text>(); t != nullptr && t->text.starts_with("[!")) {
              const std::size_t close = t->text.find(']');
              if (close != std::string::npos && close > 2 && close < 16) {
                const std::string kind = util::to_lower(t->text.substr(2, close - 2));
                if (std::ranges::all_of(kind, [](char c) { return util::is_ascii_alpha(c); })) {
                  attr.add_class(ir::conventions::admonition);
                  attr.add_class(kind);
                  t->text.erase(0, close + 1);
                  while (!t->text.empty() && (t->text.front() == '\n' || t->text.front() == ' ')) t->text.erase(0, 1);
                  if (t->text.empty()) p->content.erase(p->content.begin());
                  if (p->content.empty()) quote.blocks.erase(quote.blocks.begin());
                }
              }
            }
          }
        }
        out.push_back(ir::Block(std::move(quote), std::move(attr)));
        return;
      }
      case BT::List: {
        ir::List list;
        list.ordered = node.list.ordered;
        list.start = node.list.start;
        list.loose = !node.tight;
        bool any_task = false;
        std::vector<ir::TaskState> tasks;
        for (auto& item : node.children) {
          ir::TaskState state = ir::TaskState::None;
          if (state_.gfm()) state = take_task_marker(*item);
          if (state != ir::TaskState::None) any_task = true;
          tasks.push_back(state);
          list.items.push_back(blocks(*item, depth));
        }
        if (any_task) list.tasks = std::move(tasks);
        out.push_back(std::move(list));
        return;
      }
      case BT::DefList: {
        ir::List list;
        list.definition = true;
        for (auto& item : node.children) {
          ir::Blocks entry;
          bool first = true;
          for (auto& part : item->children) {
            if (first && part->type == BT::Paragraph) {
              entry.push_back(ir::Paragraph{inlines(part->content)});
            } else if (part->type == BT::DefDetails) {
              if (first) entry.push_back(ir::Paragraph{});
              ir::Blocks details = blocks(*part, depth);
              for (ir::Block& b : details) entry.push_back(std::move(b));
            }
            first = false;
          }
          list.items.push_back(std::move(entry));
        }
        out.push_back(std::move(list));
        return;
      }
      case BT::Table: {
        ir::TableBlock table;
        table.alignments = node.aligns;
        const std::size_t columns = node.aligns.size();
        for (std::size_t r = 0; r < node.table_lines.size(); ++r) {
          ir::TableRow row;
          row.header = r == 0;
          std::vector<std::string> cells = split_table_row(node.table_lines[r]);
          cells.resize(columns);
          for (const std::string& cell : cells) row.cells.push_back(ir::TableCell{inlines(cell), 1, 1});
          table.rows.push_back(std::move(row));
        }
        // An all-empty header row stands for "no header".
        if (!table.rows.empty() && std::ranges::all_of(table.rows.front().cells, [](const ir::TableCell& c) {
              return c.content.empty();
            })) {
          table.rows.erase(table.rows.begin());
        }
        out.push_back(std::move(table));
        return;
      }
      case BT::FootnoteDef: {
        const std::string key = normalize_label(node.label);
        if (!notes_.contains(key)) notes_[key] = blocks(node, depth);
        return;
      }
      default: return;
    }
  }

  // GFM task list item: the first paragraph starts with "[ ]", "[x]" or "[X]".
  static ir::TaskState take_task_marker(MdNode& item) {
    if (item.children.empty() || item.children.front()->type != BT::Paragraph) return ir::TaskState::None;
    std::string& content = item.children.front()->content;
    const std::string_view t = util::trim_left(content);
    if (t.size() < 3 || t[0] != '[' || t[2] != ']') return ir::TaskState::None;
    if (t.size() > 3 && !util::is_ascii_space(t[3])) return ir::TaskState::None;
    if (t.size() == 3) return ir::TaskState::None;
    const char mark = t[1];
    if (mark != ' ' && mark != 'x' && mark != 'X') return ir::TaskState::None;
    content.erase(0, content.size() - t.size() + 4);
    return mark == ' ' ? ir::TaskState::Open : ir::TaskState::Done;
  }

  ParseState& state_;
  std::map<std::string, ir::Blocks> notes_;
};

// ---- front matter ---------------------------------------------------------------------------

std::string yaml_scalar(std::string_view value) {
  std::string_view v = util::trim(value);
  if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
    v = v.substr(1, v.size() - 2);
  }
  return std::string(v);
}

// Reads the common keys of a YAML or TOML front matter block (flat keys,
// scalars and simple lists); everything else stays in the raw text.
void read_front_matter(std::string_view block, bool toml, ir::DocumentMetadata& metadata) {
  std::string current_key;
  std::vector<std::string> list_values;
  const auto commit = [&] {
    if (current_key.empty()) return;
    std::string value = util::join(list_values, current_key == "keywords" || current_key == "tags" ? ", " : "; ");
    if (value.empty()) return;
    if (current_key == "title") {
      metadata.title = value;
    } else if (current_key == "author" || current_key == "authors") {
      metadata.author = value;
    } else if (current_key == "date") {
      metadata.date = value;
    } else if (current_key == "lang" || current_key == "language") {
      metadata.language = value;
    } else {
      metadata.extra[current_key] = value;
    }
  };
  for (const std::string_view line : util::split_lines(block)) {
    if (util::trim(line).empty() || util::trim(line).starts_with("#")) continue;
    const std::string_view t = util::trim(line);
    if (!toml && t.starts_with("- ") && !current_key.empty()) {
      list_values.push_back(yaml_scalar(t.substr(2)));
      continue;
    }
    if (util::is_ascii_space(line.front())) continue;  // nested structure: kept only in the raw text
    const std::size_t sep = t.find(toml ? '=' : ':');
    if (sep == std::string_view::npos) continue;
    commit();
    list_values.clear();
    current_key = util::to_lower(util::trim(t.substr(0, sep)));
    std::string_view value = util::trim(t.substr(sep + 1));
    if (value.starts_with("[") && value.ends_with("]")) {
      for (const std::string_view item : util::split(value.substr(1, value.size() - 2), ',', false)) {
        list_values.push_back(yaml_scalar(item));
      }
    } else if (!value.empty() && value != "|" && value != ">") {
      list_values.push_back(yaml_scalar(value));
    }
  }
  commit();
}

}  // namespace

}  // namespace markdown

Result<ir::Document> parse_markdown(std::string_view text, const MarkdownReadOptions& options, WarningSink warnings) {
  (void)warnings;
  markdown::ParseState state;
  state.flavor = options.flavor;
  state.options = &options;
  ir::Document document;
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  // Front matter.
  if (state.extended() || state.gfm()) {
    for (const std::string_view fence : {"---", "+++"}) {
      if (!text.starts_with(fence)) continue;
      const std::size_t first_end = text.find_first_of("\r\n");
      if (first_end == std::string_view::npos || util::trim(text.substr(3, first_end - 3)) != "") continue;
      std::size_t pos = first_end;
      if (text[pos] == '\r' && pos + 1 < text.size() && text[pos + 1] == '\n') ++pos;
      ++pos;
      const std::size_t body_start = pos;
      bool closed = false;
      while (pos <= text.size()) {
        std::size_t end = text.find_first_of("\r\n", pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = util::trim_right(text.substr(pos, end - pos));
        if (line == fence || (fence == "---" && line == "...")) {
          const std::string_view body = text.substr(body_start, pos - body_start);
          markdown::read_front_matter(body, fence == "+++", document.metadata);
          document.metadata.extra["front_matter"] = std::string(body);
          document.metadata.extra["front_matter_format"] = fence == "+++" ? "toml" : "yaml";
          std::size_t next = end;
          if (next < text.size() && text[next] == '\r') ++next;
          if (next < text.size() && text[next] == '\n') ++next;
          text.remove_prefix(std::min(next, text.size()));
          closed = true;
          break;
        }
        if (end >= text.size()) break;
        pos = end + 1;
        if (text[end] == '\r' && pos < text.size() && text[pos] == '\n') ++pos;
      }
      if (closed) break;
    }
  }
  markdown::BlockParser parser(state, options.max_nesting);
  std::unique_ptr<markdown::MdNode> root = parser.parse(text);
  markdown::Converter converter(state);
  converter.convert(*root, document);
  if (!document.footnotes.empty()) number_footnotes(document);
  enforce_depth(document);
  return document;
}

}  // namespace cvt::document
