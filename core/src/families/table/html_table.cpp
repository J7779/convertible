// Convertible core - table family: HTML tables.
//
// Reading: a forgiving tokenizer (HTML is not XML) finds every <table> of a
// page, including nested ones, and turns each into a sheet: rows from <tr>,
// cells from <td>/<th> with colspan/rowspan expanded into merged ranges, a
// header when the first row is all <th> or sits in <thead>, the sheet name
// from <caption> (or the id). Cell text has entities decoded, white space
// collapsed, <br> and block boundaries kept as line breaks; script and style
// content is ignored.
//
// Writing: a standalone page with a small, readable style sheet (or just
// the table markup with the `html_fragment` option); merged cells become
// colspan/rowspan, numbers are right-aligned, every visible sheet gets its
// own table under a heading.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/table/common.hpp"
#include "families/table/table.hpp"
#include "families/table/xml_pull.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

// --------------------------------------------------------------------------- entities

struct Entity {
  std::string_view name;
  char32_t code;
};

constexpr Entity named_entities[] = {
    {"amp", '&'},       {"lt", '<'},        {"gt", '>'},        {"quot", '"'},      {"apos", '\''},
    {"nbsp", 0xA0},     {"ensp", 0x2002},   {"emsp", 0x2003},   {"thinsp", 0x2009}, {"ndash", 0x2013},
    {"mdash", 0x2014},  {"lsquo", 0x2018},  {"rsquo", 0x2019},  {"sbquo", 0x201A},  {"ldquo", 0x201C},
    {"rdquo", 0x201D},  {"bdquo", 0x201E},  {"bull", 0x2022},   {"hellip", 0x2026}, {"prime", 0x2032},
    {"euro", 0x20AC},   {"trade", 0x2122},  {"larr", 0x2190},   {"uarr", 0x2191},   {"rarr", 0x2192},
    {"darr", 0x2193},   {"harr", 0x2194},   {"minus", 0x2212},  {"le", 0x2264},     {"ge", 0x2265},
    {"ne", 0x2260},     {"infin", 0x221E},  {"asymp", 0x2248},  {"times", 0xD7},    {"divide", 0xF7},
    {"alpha", 0x3B1},   {"beta", 0x3B2},    {"gamma", 0x3B3},   {"delta", 0x3B4},   {"pi", 0x3C0},
    {"mu", 0x3BC},      {"sigma", 0x3C3},   {"omega", 0x3C9},   {"zwj", 0x200D},    {"zwnj", 0x200C},
    {"lrm", 0x200E},    {"rlm", 0x200F},    {"shy", 0xAD},      {"dagger", 0x2020}, {"permil", 0x2030},
    {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"check", 0x2713},  {"hearts", 0x2665}, {"star", 0x2606},
};

// Latin-1 names in code point order (160..255).
constexpr std::string_view latin1_names[] = {
    "nbsp",   "iexcl",  "cent",   "pound",  "curren", "yen",    "brvbar", "sect",   "uml",    "copy",   "ordf",
    "laquo",  "not",    "shy",    "reg",    "macr",   "deg",    "plusmn", "sup2",   "sup3",   "acute",  "micro",
    "para",   "middot", "cedil",  "sup1",   "ordm",   "raquo",  "frac14", "frac12", "frac34", "iquest", "Agrave",
    "Aacute", "Acirc",  "Atilde", "Auml",   "Aring",  "AElig",  "Ccedil", "Egrave", "Eacute", "Ecirc",  "Euml",
    "Igrave", "Iacute", "Icirc",  "Iuml",   "ETH",    "Ntilde", "Ograve", "Oacute", "Ocirc",  "Otilde", "Ouml",
    "times",  "Oslash", "Ugrave", "Uacute", "Ucirc",  "Uuml",   "Yacute", "THORN",  "szlig",  "agrave", "aacute",
    "acirc",  "atilde", "auml",   "aring",  "aelig",  "ccedil", "egrave", "eacute", "ecirc",  "euml",   "igrave",
    "iacute", "icirc",  "iuml",   "eth",    "ntilde", "ograve", "oacute", "ocirc",  "otilde", "ouml",   "divide",
    "oslash", "ugrave", "uacute", "ucirc",  "uuml",   "yacute", "thorn",  "yuml",
};

std::optional<char32_t> lookup_entity(std::string_view name) {
  for (const Entity& e : named_entities) {
    if (e.name == name) return e.code;
  }
  for (std::size_t i = 0; i < std::size(latin1_names); ++i) {
    if (latin1_names[i] == name) return static_cast<char32_t>(0xA0 + i);
  }
  return std::nullopt;
}

void decode_html_append(std::string& out, std::string_view raw) {
  std::size_t i = 0;
  while (i < raw.size()) {
    const char c = raw[i];
    if (c != '&') {
      out.push_back(c);
      ++i;
      continue;
    }
    std::size_t end = i + 1;
    while (end < raw.size() && end - i <= 32 && (util::is_ascii_alpha(raw[end]) || util::is_ascii_digit(raw[end]) || raw[end] == '#')) ++end;
    const std::string_view name = raw.substr(i + 1, end - i - 1);
    const bool terminated = end < raw.size() && raw[end] == ';';
    std::optional<char32_t> code;
    if (name.size() > 1 && name[0] == '#') {
      std::optional<std::int64_t> value;
      if (name[1] == 'x' || name[1] == 'X') {
        value = util::parse_int(name.substr(2), 16);
      } else {
        value = util::parse_int(name.substr(1), 10);
      }
      if (value && *value > 0 && *value <= 0x10FFFF) code = static_cast<char32_t>(*value);
    } else if (!name.empty()) {
      code = lookup_entity(name);
    }
    if (code) {
      util::append_utf8(out, *code == 0xA0 ? U' ' : *code);
      i = end + (terminated ? 1 : 0);
    } else {
      out.push_back('&');
      ++i;
    }
  }
}

// --------------------------------------------------------------------------- tokenizer

struct Tag {
  std::string name;  // lower case
  bool closing = false;
  bool self_closing = false;
  std::map<std::string, std::string> attributes;  // lower-case names, decoded values
};

class HtmlTokenizer {
 public:
  explicit HtmlTokenizer(std::string_view html) : html_(html) {}
  [[nodiscard]] bool at_end() const noexcept { return pos_ >= html_.size(); }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }

  // Next token: text (into `text`) or a tag (into `tag`). Returns false at the end.
  bool next(bool& is_tag, std::string& text, Tag& tag) {
    while (pos_ < html_.size()) {
      if (html_[pos_] != '<') {
        const std::size_t lt = html_.find('<', pos_);
        const std::size_t end = lt == std::string_view::npos ? html_.size() : lt;
        text.clear();
        decode_html_append(text, html_.substr(pos_, end - pos_));
        pos_ = end;
        is_tag = false;
        return true;
      }
      const std::string_view rest = html_.substr(pos_);
      if (rest.starts_with("<!--")) {
        const std::size_t close = html_.find("-->", pos_ + 4);
        pos_ = close == std::string_view::npos ? html_.size() : close + 3;
        continue;
      }
      if (rest.starts_with("<!") || rest.starts_with("<?")) {
        const std::size_t close = html_.find('>', pos_);
        pos_ = close == std::string_view::npos ? html_.size() : close + 1;
        continue;
      }
      std::size_t i = pos_ + 1;
      tag = Tag{};
      if (i < html_.size() && html_[i] == '/') {
        tag.closing = true;
        ++i;
      }
      const std::size_t name_start = i;
      while (i < html_.size() && (util::is_ascii_alpha(html_[i]) || util::is_ascii_digit(html_[i]) || html_[i] == ':' || html_[i] == '-')) ++i;
      if (i == name_start) {
        // A lone '<' is text.
        text = "<";
        ++pos_;
        is_tag = false;
        return true;
      }
      tag.name = util::to_lower(html_.substr(name_start, i - name_start));
      // Attributes.
      while (i < html_.size() && html_[i] != '>') {
        if (util::is_ascii_space(html_[i])) {
          ++i;
          continue;
        }
        if (html_[i] == '/') {
          tag.self_closing = true;
          ++i;
          continue;
        }
        const std::size_t key_start = i;
        while (i < html_.size() && !util::is_ascii_space(html_[i]) && html_[i] != '=' && html_[i] != '>' && html_[i] != '/') ++i;
        std::string key = util::to_lower(html_.substr(key_start, i - key_start));
        while (i < html_.size() && util::is_ascii_space(html_[i])) ++i;
        std::string value;
        if (i < html_.size() && html_[i] == '=') {
          ++i;
          while (i < html_.size() && util::is_ascii_space(html_[i])) ++i;
          if (i < html_.size() && (html_[i] == '"' || html_[i] == '\'')) {
            const char quote = html_[i];
            const std::size_t close = html_.find(quote, i + 1);
            const std::size_t end = close == std::string_view::npos ? html_.size() : close;
            decode_html_append(value, html_.substr(i + 1, end - i - 1));
            i = close == std::string_view::npos ? html_.size() : close + 1;
          } else {
            const std::size_t value_start = i;
            while (i < html_.size() && !util::is_ascii_space(html_[i]) && html_[i] != '>') ++i;
            decode_html_append(value, html_.substr(value_start, i - value_start));
          }
        }
        if (!key.empty() && tag.attributes.size() < 256) tag.attributes.emplace(std::move(key), std::move(value));
        if (key_start == i) ++i;
      }
      pos_ = i < html_.size() ? i + 1 : html_.size();
      // Raw text elements: skip their content entirely.
      if (!tag.closing && (tag.name == "script" || tag.name == "style" || tag.name == "textarea" || tag.name == "title")) {
        const std::string close = "</" + tag.name;
        std::size_t k = pos_;
        while (k < html_.size()) {
          const std::size_t found = html_.find("</", k);
          if (found == std::string_view::npos) {
            k = html_.size();
            break;
          }
          if (util::istarts_with(html_.substr(found), close)) {
            k = found;
            break;
          }
          k = found + 2;
        }
        pos_ = k;
      }
      is_tag = true;
      return true;
    }
    return false;
  }

 private:
  std::string_view html_;
  std::size_t pos_ = 0;
};

// --------------------------------------------------------------------------- reading

struct CellBuild {
  std::string text;
  std::uint32_t colspan = 1;
  std::uint32_t rowspan = 1;
  bool header = false;
};

struct TableBuild {
  std::size_t order = 0;  // position of the opening tag: sheets follow the page order
  std::string name;
  std::vector<std::vector<CellBuild>> rows;
  bool in_thead = false;
  bool thead_rows = false;
  bool in_caption = false;
  bool cell_open = false;
  bool pre = false;
};

void append_text(std::string& out, std::string_view text, bool pre) {
  if (pre) {
    out.append(text);
    return;
  }
  for (const char c : text) {
    if (util::is_ascii_space(c)) {
      if (!out.empty() && out.back() != ' ' && out.back() != '\n') out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
}

void line_break(std::string& out) {
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (!out.empty() && out.back() != '\n') out.push_back('\n');
}

std::string finish_cell_text(std::string text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
  std::size_t start = 0;
  while (start < text.size() && (text[start] == ' ' || text[start] == '\n')) ++start;
  text.erase(0, start);
  return util::replace_all(text, " \n", "\n");
}

Result<AnyIR> read_html_tables(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string html, decode_source_text(ctx.input, ctx.options.get_string("source_encoding", "auto"), ctx));
  HtmlTokenizer tokenizer(html);
  std::vector<TableBuild> stack;
  std::vector<TableBuild> done;
  bool is_tag = false;
  std::string text;
  Tag tag;
  std::size_t tokens = 0;
  std::string heading;          // text of the last heading outside tables, names the next table
  bool in_heading = false;
  const auto current_cell = [&]() -> CellBuild* {
    if (stack.empty() || !stack.back().cell_open || stack.back().rows.empty() || stack.back().rows.back().empty()) return nullptr;
    return &stack.back().rows.back().back();
  };
  const auto close_table = [&] {
    TableBuild table = std::move(stack.back());
    stack.pop_back();
    done.push_back(std::move(table));
    // A nested table's text does not belong to the enclosing cell.
  };
  while (tokenizer.next(is_tag, text, tag)) {
    if ((++tokens & 8191) == 0) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(tokenizer.position()) / static_cast<double>(html.size())));
    }
    if (!is_tag) {
      if (stack.empty()) {
        if (in_heading) append_text(heading, text, false);
        continue;
      }
      TableBuild& t = stack.back();
      if (t.in_caption) {
        append_text(t.name, text, false);
      } else if (CellBuild* cell = current_cell()) {
        append_text(cell->text, text, t.pre);
      }
      continue;
    }
    const std::string& name = tag.name;
    if (stack.empty() && name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
      in_heading = !tag.closing;
      if (!tag.closing) heading.clear();
      continue;
    }
    if (name == "table") {
      if (tag.closing) {
        if (!stack.empty()) close_table();
      } else {
        if (stack.size() >= 64) return fail(Status::Limit, "HTML tables are nested too deeply");
        CVT_TRY(ctx.limits.check_archive_entries(done.size() + stack.size() + 1));
        TableBuild table;
        table.order = done.size() + stack.size();
        if (const auto it = tag.attributes.find("id"); it != tag.attributes.end()) table.name = it->second;
        if (stack.empty() && !heading.empty()) table.name = heading;
        heading.clear();
        in_heading = false;
        stack.push_back(std::move(table));
      }
      continue;
    }
    if (stack.empty()) continue;
    TableBuild& t = stack.back();
    if (name == "caption") {
      t.in_caption = !tag.closing;
      if (!tag.closing) t.name.clear();
    } else if (name == "thead") {
      t.in_thead = !tag.closing;
    } else if (name == "tr") {
      t.cell_open = false;
      if (!tag.closing) {
        t.rows.emplace_back();
        if (t.in_thead) t.thead_rows = true;
      }
    } else if (name == "td" || name == "th") {
      if (tag.closing) {
        t.cell_open = false;
        continue;
      }
      if (t.rows.empty()) t.rows.emplace_back();
      CellBuild cell;
      cell.header = name == "th" || t.in_thead;
      const auto span = [&](const char* key, std::uint32_t max) {
        const auto it = tag.attributes.find(key);
        if (it == tag.attributes.end()) return std::uint32_t{1};
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(util::parse_int(util::trim(it->second)).value_or(1), 1, max));
      };
      cell.colspan = span("colspan", 1000);
      cell.rowspan = span("rowspan", 65534);
      t.rows.back().push_back(std::move(cell));
      t.cell_open = true;
    } else if (CellBuild* cell = current_cell()) {
      if (name == "br" || ((name == "p" || name == "div" || name == "li" || name == "tr") && !cell->text.empty())) {
        line_break(cell->text);
      } else if (name == "pre") {
        t.pre = !tag.closing;
      }
    }
  }
  while (!stack.empty()) close_table();
  std::ranges::sort(done, [](const TableBuild& a, const TableBuild& b) { return a.order < b.order; });

  // Lay the cells out on a grid, honouring row and column spans.
  const InferOptions infer = infer_options_from(ctx.options);
  CellBudget budget(ctx.limits);
  ir::Table table;
  for (std::size_t index = 0; index < done.size(); ++index) {
    TableBuild& build = done[index];
    if (build.rows.empty()) continue;
    ir::Sheet sheet;
    sheet.name = finish_cell_text(build.name);
    if (sheet.name.empty()) sheet.name = std::format("Table {}", table.sheets.size() + 1);
    std::vector<std::vector<bool>> occupied;
    bool first_row_headers = !build.rows.front().empty();
    for (const CellBuild& cell : build.rows.front()) first_row_headers = first_row_headers && cell.header;
    for (std::size_t r = 0; r < build.rows.size(); ++r) {
      std::size_t column = 0;
      for (CellBuild& cell : build.rows[r]) {
        while (r < occupied.size() && column < occupied[r].size() && occupied[r][column]) ++column;
        const std::uint64_t last_row = r + cell.rowspan - 1;
        const std::uint64_t last_column = column + cell.colspan - 1;
        CVT_TRY(budget.take(std::uint64_t{cell.colspan} * cell.rowspan));
        if (occupied.size() <= last_row) occupied.resize(static_cast<std::size_t>(last_row + 1));
        for (std::uint64_t rr = r; rr <= last_row; ++rr) {
          auto& line = occupied[static_cast<std::size_t>(rr)];
          if (line.size() <= last_column) line.resize(static_cast<std::size_t>(last_column + 1), false);
          for (std::uint64_t cc = column; cc <= last_column; ++cc) line[static_cast<std::size_t>(cc)] = true;
        }
        std::string value = finish_cell_text(std::move(cell.text));
        ir::Cell ir_cell = (r == 0 && first_row_headers) || cell.header ? (value.empty() ? ir::Cell{} : ir::Cell::string(std::move(value)))
                                                                         : infer_cell(std::move(value), infer);
        if (!ir_cell.empty()) CVT_TRY(put_cell(sheet, r, column, std::move(ir_cell), budget));
        if (cell.colspan > 1 || cell.rowspan > 1) {
          sheet.merged.push_back(ir::CellRange{static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(column),
                                               static_cast<std::uint32_t>(last_row), static_cast<std::uint32_t>(last_column)});
        }
        column = static_cast<std::size_t>(last_column + 1);
      }
    }
    sheet.has_header = first_row_headers || build.thead_rows;
    trim_sheet(sheet);
    table.sheets.push_back(std::move(sheet));
  }
  if (table.sheets.empty()) return fail(Status::Decode, "the page contains no tables");
  return table;
}

// --------------------------------------------------------------------------- writing

std::string html_escape(std::string_view text) {
  std::string out;
  xml_escape_append(out, text);
  return out;
}

constexpr std::string_view page_style = R"(  <style>
    :root { color-scheme: light dark; }
    body { font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; margin: 2rem; color: #1f2933; background: #fff; }
    h2 { font-size: 1.1rem; margin: 2rem 0 .6rem; }
    table { border-collapse: collapse; margin-bottom: 1.5rem; }
    th, td { padding: .35rem .7rem; border-bottom: 1px solid #e1e5ea; text-align: left; vertical-align: top; }
    thead th { background: #eef2f7; border-bottom: 2px solid #7f93b2; font-weight: 600; }
    tbody tr:nth-child(even) { background: #f8fafc; }
    td.num { text-align: right; font-variant-numeric: tabular-nums; }
    @media (prefers-color-scheme: dark) {
      body { color: #e4e7eb; background: #1a1d21; }
      th, td { border-bottom-color: #3a3f45; }
      thead th { background: #262b31; border-bottom-color: #5f7391; }
      tbody tr:nth-child(even) { background: #20242a; }
    }
  </style>
)";

std::string table_markup(const ir::Sheet& sheet, const std::string& indent) {
  std::string out = indent + "<table>\n";
  const std::size_t columns = sheet.column_count();
  const auto merge_at = [&](std::size_t r, std::size_t c) -> const ir::CellRange* {
    for (const ir::CellRange& m : sheet.merged) {
      if (m.contains(r, c)) return &m;
    }
    return nullptr;
  };
  for (std::size_t r = 0; r < sheet.rows.size(); ++r) {
    const bool header = r == 0 && sheet.has_header;
    if (header) out += indent + "  <thead>\n";
    if (r == (sheet.has_header ? 1u : 0u)) out += indent + "  <tbody>\n";
    out += indent + "    <tr>";
    for (std::size_t c = 0; c < columns; ++c) {
      const ir::CellRange* merge = merge_at(r, c);
      if (merge != nullptr && (merge->first_row != r || merge->first_column != c)) continue;
      const ir::Cell& cell = sheet.at(r, c);
      const char* element = header ? "th" : "td";
      out += std::format("<{}", element);
      if (!header && cell.type == ir::CellType::Number) out += " class=\"num\"";
      if (merge != nullptr) {
        if (merge->last_column > merge->first_column) out += std::format(" colspan=\"{}\"", merge->last_column - merge->first_column + 1);
        if (merge->last_row > merge->first_row) out += std::format(" rowspan=\"{}\"", merge->last_row - merge->first_row + 1);
      }
      out += '>';
      out += util::replace_all(html_escape(cell.text), "\n", "<br>");
      out += std::format("</{}>", element);
    }
    out += "</tr>\n";
    if (header) out += indent + "  </thead>\n";
  }
  if (sheet.rows.size() > (sheet.has_header ? 1u : 0u)) out += indent + "  </tbody>\n";
  out += indent + "</table>\n";
  return out;
}

Result<Bytes> write_html_tables(const AnyIR& value, WriteContext& ctx) {
  const ir::Table& table = value.get<ir::Table>();
  std::vector<const ir::Sheet*> sheets;
  if (!ctx.options.get_string("sheet", "").empty()) {
    CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(table, ctx));
    sheets.push_back(sheet);
  } else {
    for (const ir::Sheet& sheet : table.sheets) {
      if (!sheet.hidden()) sheets.push_back(&sheet);
    }
    if (sheets.empty() && !table.sheets.empty()) sheets.push_back(&table.sheets.front());
  }
  const bool fragment = ctx.options.get_bool("html_fragment", false);
  const bool headings = sheets.size() > 1;
  std::string body;
  for (std::size_t i = 0; i < sheets.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(sheets.size())));
    if (headings) body += std::string(fragment ? "" : "  ") + "<h2>" + html_escape(sheets[i]->name) + "</h2>\n";
    body += table_markup(*sheets[i], fragment ? "" : "  ");
    CVT_TRY(ctx.limits.check_output(body.size()));
  }
  if (fragment) return to_bytes(body);
  std::string title = sheets.size() == 1 && !sheets[0]->name.empty() ? sheets[0]->name : std::string("Table");
  std::string out = "<!DOCTYPE html>\n<html>\n<head>\n  <meta charset=\"utf-8\">\n"
                    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n  <title>" +
                    html_escape(title) + "</title>\n";
  out += page_style;
  out += "</head>\n<body>\n" + body + "</body>\n</html>\n";
  return to_bytes(out);
}

}  // namespace

void register_html_table(Registry& registry) {
  FormatInfo info;
  info.id = "html";
  info.name = "HTML page";
  info.description = "Web page; its tables become sheets.";
  info.category = Category::Document;
  info.extensions = {"html", "htm", "xhtml"};
  info.mime = "text/html";
  info.popularity = 90;
  registry.add_known_format(std::move(info));  // the document family owns the format's description

  Codec reader;
  reader.format_id = "html";
  reader.kind = Kind::Table;
  reader.read = read_html_tables;
  reader.read_cost = 30;
  reader.lossy = true;
  reader.read_options = infer_option_specs();
  reader.read_options.push_back(source_encoding_option());
  registry.add_codec(std::move(reader));

  Codec writer;
  writer.format_id = "html";
  writer.kind = Kind::Table;
  writer.write = write_html_tables;
  writer.write_options = {
      OptionSpec::boolean("html_fragment", "Table markup only", false,
                          "Write only the <table> elements instead of a complete page with a style sheet."),
      sheet_option(),
  };
  registry.add_codec(std::move(writer));
}

}  // namespace cvt::table
