// Convertible core - table family: OpenDocument spreadsheets (ODS, OTS and
// flat FODS).
//
// The reader walks content.xml (or the single flat document) with the
// streaming XmlPull reader. Repeated rows and cells are expanded only where
// they hold content, so the customary "1,048,000 empty rows repeated" tails
// cost nothing. Value types float, percentage, currency, date, time,
// boolean and string are mapped to typed cells, formulas are translated
// from OpenFormula to Excel notation, spans become merged ranges, and hidden
// tables, column widths and header rows are kept.
//
// The writer emits typed cells with date/time styles, a styled header row,
// column widths, merges, formulas with cached values, and no metadata.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/table/common.hpp"
#include "families/table/spreadsheet.hpp"
#include "families/table/table.hpp"
#include "families/table/xml_pull.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::table {

namespace {

constexpr std::string_view ods_mime = "application/vnd.oasis.opendocument.spreadsheet";
constexpr std::string_view ots_mime = "application/vnd.oasis.opendocument.spreadsheet-template";

// --------------------------------------------------------------------------- reading

// "2.258cm", "0.889in", "64pt" ... -> Excel character widths.
double length_to_characters(std::string_view text) {
  std::size_t split = 0;
  while (split < text.size() && (util::is_ascii_digit(text[split]) || text[split] == '.' || text[split] == '-')) ++split;
  const auto number = util::parse_double(text.substr(0, split));
  if (!number || *number <= 0) return 0.0;
  const std::string_view unit = text.substr(split);
  double inches = 0.0;
  if (unit == "cm") {
    inches = *number / 2.54;
  } else if (unit == "mm") {
    inches = *number / 25.4;
  } else if (unit == "in" || unit == "inch") {
    inches = *number;
  } else if (unit == "pt") {
    inches = *number / 72.0;
  } else if (unit == "pc") {
    inches = *number / 6.0;
  } else if (unit == "px") {
    inches = *number / 96.0;
  } else {
    return 0.0;
  }
  const double characters = (inches * 96.0 - 5.0) / 7.0;
  return characters > 0 ? std::round(characters * 100.0) / 100.0 : 0.0;
}

std::string characters_to_length(double characters) {
  const double cm = (characters * 7.0 + 5.0) / 96.0 * 2.54;
  return std::format("{:.3f}cm", cm);
}

// "PT12H30M15.5S" (ISO 8601 duration) -> seconds.
std::optional<double> parse_duration(std::string_view text) {
  bool negative = false;
  if (text.starts_with('-')) {
    negative = true;
    text.remove_prefix(1);
  }
  if (!text.starts_with('P')) return std::nullopt;
  text.remove_prefix(1);
  double total = 0.0;
  bool time_part = false;
  while (!text.empty()) {
    if (text.front() == 'T') {
      time_part = true;
      text.remove_prefix(1);
      continue;
    }
    std::size_t n = 0;
    while (n < text.size() && (util::is_ascii_digit(text[n]) || text[n] == '.' || text[n] == ',')) ++n;
    if (n == 0 || n >= text.size()) return std::nullopt;
    std::string number(text.substr(0, n));
    std::ranges::replace(number, ',', '.');
    const auto value = util::parse_double(number);
    if (!value) return std::nullopt;
    const char unit = text[n];
    if (unit == 'D' && !time_part) {
      total += *value * 86400.0;
    } else if (unit == 'H' && time_part) {
      total += *value * 3600.0;
    } else if (unit == 'M' && time_part) {
      total += *value * 60.0;
    } else if (unit == 'S' && time_part) {
      total += *value;
    } else {
      return std::nullopt;  // years and months have no fixed length
    }
    text.remove_prefix(n + 1);
  }
  return negative ? -total : total;
}

struct CellData {
  ir::Cell cell;
  std::string formula;
  std::uint32_t column_span = 1;
  std::uint32_t row_span = 1;
};

// Read the content of a table:table-cell (the start tag has been consumed).
Result<CellData> read_cell(XmlPull& xml, bool covered) {
  CellData data;
  const std::string type = xml.attribute("office:value-type").value_or("");
  const std::string value = xml.attribute("office:value").value_or("");
  const std::string date_value = xml.attribute("office:date-value").value_or("");
  const std::string time_value = xml.attribute("office:time-value").value_or("");
  const std::string bool_value = xml.attribute("office:boolean-value").value_or("");
  const auto string_value = xml.attribute("office:string-value");
  const std::string formula = xml.attribute("table:formula").value_or("");
  data.column_span = static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(util::parse_int(xml.attribute("table:number-columns-spanned").value_or("1")).value_or(1), 1, 16384));
  data.row_span = static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(util::parse_int(xml.attribute("table:number-rows-spanned").value_or("1")).value_or(1), 1, 1048576));
  if (!formula.empty()) data.formula = odf_to_excel_formula(formula);

  // Text content: paragraphs separated by line breaks.
  std::string text;
  bool any_paragraph = false;
  const std::size_t cell_depth = xml.depth();
  while (xml.depth() >= cell_depth) {
    CVT_TRY_ASSIGN(const XmlPull::Event event, xml.next());
    if (event == XmlPull::Event::End) return fail(Status::Decode, "the document ends inside a cell");
    if (event == XmlPull::Event::StartElement) {
      const std::string_view name = xml.name();
      if (name == "text:p" || name == "text:h") {
        if (any_paragraph) text.push_back('\n');
        any_paragraph = true;
      } else if (name == "text:s") {
        const auto count = std::clamp<std::int64_t>(util::parse_int(xml.attribute("text:c").value_or("1")).value_or(1), 1, 1000);
        text.append(static_cast<std::size_t>(count), ' ');
      } else if (name == "text:tab") {
        text.push_back('\t');
      } else if (name == "text:line-break") {
        text.push_back('\n');
      } else if (name == "office:annotation" || name == "draw:frame" || name == "text:note") {
        CVT_TRY(xml.skip_element());
      }
    } else if (event == XmlPull::Event::Text && xml.depth() > cell_depth) {
      text.append(xml.text());
    }
  }
  if (covered) return data;
  if (type == "float" || type == "percentage" || type == "currency") {
    if (const auto number = util::parse_double(util::trim(value))) {
      data.cell = number_cell(*number);
    } else if (!text.empty()) {
      data.cell = ir::Cell::string(std::move(text));
    }
  } else if (type == "date") {
    if (const auto date = parse_iso_datetime(date_value)) {
      data.cell = datetime_cell(date->unix_seconds, date->style);
    } else if (!text.empty()) {
      data.cell = ir::Cell::string(std::move(text));
    }
  } else if (type == "time") {
    if (const auto seconds = parse_duration(time_value); seconds && *seconds >= 0 && *seconds < 86400.0) {
      data.cell = datetime_cell(excel_1900_epoch_unix + *seconds, DateStyle::Time);
    } else if (seconds) {
      data.cell = number_cell(*seconds / 86400.0);
    } else if (!text.empty()) {
      data.cell = ir::Cell::string(std::move(text));
    }
  } else if (type == "boolean") {
    data.cell = ir::Cell::boolean(bool_value == "true" || bool_value == "1");
  } else if (type == "string" && string_value) {
    data.cell = ir::Cell::string(*string_value);
  } else if (type == "string" || (type.empty() && any_paragraph && !text.empty())) {
    data.cell = ir::Cell::string(std::move(text));
  }
  return data;
}

struct OdsStyles {
  std::set<std::string> hidden_tables;
  std::map<std::string, double> column_widths;  // characters
};

Result<void> read_automatic_styles(XmlPull& xml, OdsStyles& styles) {
  // Positioned on <office:automatic-styles>.
  const std::size_t depth = xml.depth();
  std::string current;
  std::string family;
  while (xml.depth() >= depth) {
    CVT_TRY_ASSIGN(const XmlPull::Event event, xml.next());
    if (event == XmlPull::Event::End) return fail(Status::Decode, "the document ends inside its styles");
    if (event != XmlPull::Event::StartElement) continue;
    const std::string_view name = xml.name();
    if (name == "style:style") {
      current = xml.attribute("style:name").value_or("");
      family = xml.attribute("style:family").value_or("");
    } else if (name == "style:table-properties" && family == "table") {
      if (xml.attribute("table:display").value_or("true") == "false") styles.hidden_tables.insert(current);
    } else if (name == "style:table-column-properties" && family == "table-column") {
      if (const auto width = xml.attribute("style:column-width")) {
        const double characters = length_to_characters(*width);
        if (characters > 0) styles.column_widths[current] = characters;
      }
    }
  }
  return {};
}

Result<ir::Table> read_ods_content(std::string_view content, const ReadContext& ctx) {
  XmlPull xml(content);
  ir::Table table;
  OdsStyles styles;
  CellBudget budget(ctx.limits);
  ir::Sheet* sheet = nullptr;
  std::uint64_t row = 0;
  std::uint64_t column = 0;
  std::uint64_t row_repeat = 1;
  std::uint64_t column_index = 0;  // for table:table-column
  std::size_t rows_seen = 0;
  // Cells of the current row, collected so repeated rows can be copied.
  struct RowCell {
    std::uint64_t column;
    std::uint64_t repeat;
    CellData data;
  };
  std::vector<RowCell> row_cells;
  bool row_open = false;

  const auto flush_row = [&]() -> Result<void> {
    if (sheet == nullptr || !row_open) return {};
    row_open = false;
    bool has_content = false;
    for (const RowCell& c : row_cells) {
      if (!c.data.cell.empty() || !c.data.formula.empty() || c.data.column_span > 1 || c.data.row_span > 1) has_content = true;
    }
    if (!has_content) {
      row += row_repeat;
      row_cells.clear();
      return {};
    }
    // Repeating a row with content: copy it (bounded by the cell budget).
    for (std::uint64_t k = 0; k < row_repeat; ++k) {
      const std::uint64_t r = row + k;
      for (const RowCell& c : row_cells) {
        const bool content = !c.data.cell.empty() || !c.data.formula.empty();
        for (std::uint64_t j = 0; j < c.repeat; ++j) {
          const std::uint64_t col = c.column + j;
          if (content) {
            CVT_TRY(put_cell(*sheet, r, col, c.data.cell, budget));
            if (!c.data.formula.empty() && r < UINT32_MAX && col < UINT32_MAX) {
              sheet->formulas[{static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(col)}] = c.data.formula;
            }
          }
          if ((c.data.column_span > 1 || c.data.row_span > 1) && sheet->merged.size() < 100'000 && r < UINT32_MAX &&
              col + c.data.column_span < UINT32_MAX && r + c.data.row_span < UINT32_MAX) {
            ir::CellRange range;
            range.first_row = static_cast<std::uint32_t>(r);
            range.first_column = static_cast<std::uint32_t>(col);
            range.last_row = static_cast<std::uint32_t>(r + c.data.row_span - 1);
            range.last_column = static_cast<std::uint32_t>(col + c.data.column_span - 1);
            sheet->merged.push_back(range);
          }
        }
      }
    }
    row += row_repeat;
    row_cells.clear();
    return {};
  };

  while (true) {
    CVT_TRY_ASSIGN(const XmlPull::Event event, xml.next());
    if (event == XmlPull::Event::End) break;
    if (event == XmlPull::Event::EndElement) {
      const std::string_view name = xml.name();
      if (name == "table:table-row") {
        CVT_TRY(flush_row());
      } else if (name == "table:table") {
        if (sheet != nullptr) trim_sheet(*sheet);
        sheet = nullptr;
      }
      continue;
    }
    if (event != XmlPull::Event::StartElement) continue;
    const std::string_view name = xml.name();
    if (name == "office:automatic-styles") {
      CVT_TRY(read_automatic_styles(xml, styles));
    } else if (name == "table:table" && sheet == nullptr) {
      CVT_TRY(ctx.limits.check_archive_entries(table.sheets.size() + 1));
      table.sheets.emplace_back();
      sheet = &table.sheets.back();
      sheet->name = xml.attribute("table:name").value_or("");
      if (styles.hidden_tables.contains(xml.attribute("table:style-name").value_or(""))) {
        sheet->visibility = ir::SheetVisibility::Hidden;
      }
      row = 0;
      column_index = 0;
    } else if (name == "table:table" && sheet != nullptr) {
      CVT_TRY(xml.skip_element());  // nested table inside a cell (not representable)
    } else if (sheet == nullptr) {
      continue;
    } else if (name == "table:table-column") {
      const auto repeat = static_cast<std::uint64_t>(
          std::clamp<std::int64_t>(util::parse_int(xml.attribute("table:number-columns-repeated").value_or("1")).value_or(1), 1, 1 << 20));
      const auto it = styles.column_widths.find(xml.attribute("table:style-name").value_or(""));
      if (it != styles.column_widths.end() && column_index < 16384) {
        const std::uint64_t end = std::min<std::uint64_t>(column_index + repeat, 16384);
        if (sheet->column_widths.size() < end) sheet->column_widths.resize(static_cast<std::size_t>(end), 0.0);
        for (std::uint64_t c = column_index; c < end; ++c) sheet->column_widths[static_cast<std::size_t>(c)] = it->second;
      }
      column_index += repeat;
    } else if (name == "table:table-header-rows") {
      if (row == 0) sheet->has_header = true;
    } else if (name == "table:table-row") {
      CVT_TRY(flush_row());
      row_open = true;
      row_repeat = static_cast<std::uint64_t>(
          std::clamp<std::int64_t>(util::parse_int(xml.attribute("table:number-rows-repeated").value_or("1")).value_or(1), 1, 1 << 24));
      column = 0;
      if ((++rows_seen & 1023) == 0) {
        CVT_TRY(ctx.checkpoint(static_cast<double>(xml.position()) / static_cast<double>(std::max<std::size_t>(1, content.size()))));
      }
    } else if (name == "table:table-cell" || name == "table:covered-table-cell") {
      const auto repeat = static_cast<std::uint64_t>(
          std::clamp<std::int64_t>(util::parse_int(xml.attribute("table:number-columns-repeated").value_or("1")).value_or(1), 1, 1 << 20));
      CVT_TRY_ASSIGN(CellData data, read_cell(xml, name == "table:covered-table-cell"));
      if (!row_open) {
        row_open = true;
        row_repeat = 1;
      }
      if (!data.cell.empty() || !data.formula.empty() || data.column_span > 1 || data.row_span > 1) {
        CVT_TRY(budget.take(repeat));
        row_cells.push_back(RowCell{column, repeat, std::move(data)});
      }
      column += repeat;
    }
  }
  if (table.sheets.empty()) return fail(Status::Decode, "the document holds no spreadsheet tables");
  for (ir::Sheet& s : table.sheets) trim_sheet(s);
  guess_headers(table);
  return table;
}

Result<AnyIR> read_ods(ReadContext& ctx) {
  CVT_TRY_ASSIGN(auto zip, util::ZipReader::open(ctx.input, ctx.limits));
  const auto index = zip.find("content.xml");
  if (!index) return fail(Status::Decode, "the package has no content.xml");
  CVT_TRY_ASSIGN(const Bytes bytes, zip.read(*index));
  const std::string content = xml_text(bytes);
  CVT_TRY_ASSIGN(ir::Table table, read_ods_content(content, ctx));
  return table;
}

Result<AnyIR> read_fods(ReadContext& ctx) {
  const std::string content = xml_text(ctx.input);
  CVT_TRY_ASSIGN(ir::Table table, read_ods_content(content, ctx));
  return table;
}

// --------------------------------------------------------------------------- writing

constexpr std::string_view namespaces =
    R"( xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0")"
    R"( xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0")"
    R"( xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0")"
    R"( xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0")"
    R"( xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0")"
    R"( xmlns:number="urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0")"
    R"( xmlns:of="urn:oasis:names:tc:opendocument:xmlns:of:1.2")"
    R"( xmlns:config="urn:oasis:names:tc:opendocument:xmlns:config:1.0")";

// Paragraph text with ODF's whitespace elements.
void append_paragraph_text(std::string& out, std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '\t') {
      out += "<text:tab/>";
      ++i;
    } else if (c == ' ' && (i == 0 || text[i - 1] == ' ' || i + 1 == text.size())) {
      std::size_t n = 0;
      while (i < text.size() && text[i] == ' ') {
        ++n;
        ++i;
      }
      out += n == 1 ? "<text:s/>" : std::format("<text:s text:c=\"{}\"/>", n);
    } else if (c == ' ') {
      // A single space between words, unless a run of spaces starts here.
      if (i + 1 < text.size() && text[i + 1] == ' ') {
        out.push_back(' ');
        ++i;
        std::size_t n = 0;
        while (i < text.size() && text[i] == ' ') {
          ++n;
          ++i;
        }
        out += n == 1 ? "<text:s/>" : std::format("<text:s text:c=\"{}\"/>", n);
      } else {
        out.push_back(' ');
        ++i;
      }
    } else {
      std::size_t end = i;
      while (end < text.size() && text[end] != '\t' && text[end] != ' ') ++end;
      xml_escape_append(out, text.substr(i, end - i));
      i = end;
    }
  }
}

void append_paragraphs(std::string& out, std::string_view text) {
  for (const std::string_view line : util::split(text, '\n')) {
    out += "<text:p>";
    append_paragraph_text(out, line);
    out += "</text:p>";
  }
}

std::string iso_value(double unix_seconds, bool with_time) {
  std::string text = format_datetime(unix_seconds, with_time ? DateStyle::DateTime : DateStyle::Date);
  return text;
}

std::string duration_value(double seconds) {
  const auto ms = static_cast<std::int64_t>(std::llround(seconds * 1000.0));
  const std::int64_t h = ms / 3'600'000;
  const std::int64_t m = ms / 60'000 % 60;
  const std::int64_t s = ms / 1000 % 60;
  const std::int64_t f = ms % 1000;
  if (f != 0) return std::format("PT{:02}H{:02}M{:02}.{:03}S", h, m, s, f);
  return std::format("PT{:02}H{:02}M{:02}S", h, m, s);
}

struct OdsWriteState {
  std::map<std::string, std::string> column_styles;  // width text -> style name
  bool header_style = true;
};

Result<std::string> table_xml(const ir::Sheet& sheet, const std::string& name, OdsWriteState& state,
                              const WriteContext& ctx, bool auto_widths) {
  std::string out;
  out += "<table:table table:name=\"";
  xml_escape_append(out, name, true);
  out += sheet.hidden() ? "\" table:style-name=\"ta2\">" : "\" table:style-name=\"ta1\">";
  const std::size_t columns = std::max<std::size_t>(sheet.column_count(), 1);
  // Columns, with widths from the sheet or the content.
  std::vector<double> widths(columns, 0.0);
  for (std::size_t c = 0; c < columns && c < sheet.column_widths.size(); ++c) widths[c] = sheet.column_widths[c];
  if (auto_widths && sheet.column_widths.empty()) {
    const std::size_t sample = std::min<std::size_t>(sheet.rows.size(), 2000);
    for (std::size_t r = 0; r < sample; ++r) {
      for (std::size_t c = 0; c < sheet.rows[r].size() && c < columns; ++c) {
        std::size_t width = 0;
        for (const std::string_view line : util::split_lines(sheet.rows[r][c].text)) width = std::max(width, display_width(line));
        if (r == 0 && sheet.has_header) width += width / 8 + 1;
        widths[c] = std::max(widths[c], std::clamp(static_cast<double>(width) + 2.0, 0.0, 80.0));
      }
    }
    for (double& w : widths) {
      if (w > 0 && w < 8.43) w = 8.43;
    }
  }
  std::size_t c = 0;
  while (c < columns) {
    std::size_t run = 1;
    while (c + run < columns && widths[c + run] == widths[c]) ++run;
    std::string style = "co0";
    if (widths[c] > 0) {
      const std::string length = characters_to_length(widths[c]);
      auto [it, inserted] = state.column_styles.emplace(length, std::format("co{}", state.column_styles.size() + 1));
      style = it->second;
    }
    out += std::format("<table:table-column table:style-name=\"{}\"", style);
    if (run > 1) out += std::format(" table:number-columns-repeated=\"{}\"", run);
    out += " table:default-cell-style-name=\"Default\"/>";
    c += run;
  }

  // Cells covered by a merge (other than its top-left cell).
  std::vector<ir::CellRange> merges;
  for (const ir::CellRange& m : sheet.merged) {
    if (m.first_row <= m.last_row && m.first_column <= m.last_column && m.last_row < 1'048'576 &&
        m.last_column < 16'384 && (m.first_row != m.last_row || m.first_column != m.last_column)) {
      merges.push_back(m);
    }
  }
  const auto merge_at = [&](std::size_t r, std::size_t col) -> const ir::CellRange* {
    for (const ir::CellRange& m : merges) {
      if (m.contains(r, col)) return &m;
    }
    return nullptr;
  };
  std::size_t last_row = sheet.rows.size();
  for (const ir::CellRange& m : merges) last_row = std::max<std::size_t>(last_row, std::size_t{m.last_row} + 1);
  if (last_row == 0) last_row = 1;
  for (std::size_t r = 0; r < last_row; ++r) {
    if ((r & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(last_row)));
    const bool header = r == 0 && sheet.has_header;
    if (header) out += "<table:table-header-rows>";
    out += "<table:table-row>";
    const std::size_t width = r < sheet.rows.size() ? sheet.rows[r].size() : 0;
    std::size_t row_end = width;
    for (const ir::CellRange& m : merges) {
      if (m.contains(r, m.first_column)) row_end = std::max<std::size_t>(row_end, std::size_t{m.last_column} + 1);
    }
    std::size_t empty_run = 0;
    const auto flush_empty = [&] {
      if (empty_run == 0) return;
      out += empty_run == 1 ? "<table:table-cell/>"
                            : std::format("<table:table-cell table:number-columns-repeated=\"{}\"/>", empty_run);
      empty_run = 0;
    };
    for (std::size_t col = 0; col < row_end; ++col) {
      const ir::Cell& cell = sheet.at(r, col);
      const std::string_view formula = sheet.formula_at(r, col);
      const ir::CellRange* merge = merge_at(r, col);
      const bool top_left = merge != nullptr && merge->first_row == r && merge->first_column == col;
      if (merge != nullptr && !top_left) {
        flush_empty();
        out += "<table:covered-table-cell/>";
        continue;
      }
      if (cell.empty() && formula.empty() && !top_left) {
        ++empty_run;
        continue;
      }
      flush_empty();
      out += "<table:table-cell";
      if (header) {
        out += " table:style-name=\"ce1\"";
      } else if (cell.type == ir::CellType::DateTime) {
        const std::string code = cell_number_format(sheet, r, col);
        const FormatKind kind = classify_number_format(code);
        out += kind == FormatKind::Time ? " table:style-name=\"ce4\""
               : kind == FormatKind::DateTime ? " table:style-name=\"ce3\""
                                              : " table:style-name=\"ce2\"";
      }
      if (top_left) {
        out += std::format(" table:number-columns-spanned=\"{}\" table:number-rows-spanned=\"{}\"",
                           merge->last_column - merge->first_column + 1, merge->last_row - merge->first_row + 1);
      }
      if (!formula.empty()) {
        out += " table:formula=\"";
        xml_escape_append(out, excel_to_odf_formula(formula), true);
        out += '"';
      }
      std::string display = cell.text;
      switch (cell.type) {
        case ir::CellType::Empty: break;
        case ir::CellType::String: out += " office:value-type=\"string\""; break;
        case ir::CellType::Number:
          if (std::isfinite(cell.number)) {
            out += std::format(" office:value-type=\"float\" office:value=\"{}\"", util::format_double(cell.number));
            display = format_number(cell.number);
          } else {
            out += " office:value-type=\"string\"";
          }
          break;
        case ir::CellType::Bool:
          out += std::format(" office:value-type=\"boolean\" office:boolean-value=\"{}\"", cell.number != 0 ? "true" : "false");
          display = cell.number != 0 ? "TRUE" : "FALSE";
          break;
        case ir::CellType::DateTime:
          if (is_time_only(cell.number)) {
            out += std::format(" office:value-type=\"time\" office:time-value=\"{}\"",
                               duration_value(cell.number - excel_1900_epoch_unix));
            display = format_datetime(cell.number, DateStyle::Time);
          } else {
            const double day_seconds = cell.number - std::floor(cell.number / 86400.0) * 86400.0;
            out += std::format(" office:value-type=\"date\" office:date-value=\"{}\"", iso_value(cell.number, day_seconds != 0.0));
            display = format_datetime(cell.number);
          }
          break;
      }
      out += '>';
      if (!cell.empty()) append_paragraphs(out, display);
      out += "</table:table-cell>";
    }
    out += "</table:table-row>";
    if (header) out += "</table:table-header-rows>";
    CVT_TRY(ctx.limits.check_output(out.size()));
  }
  out += "</table:table>";
  return out;
}

std::string automatic_styles(const OdsWriteState& state) {
  std::string out = "<office:automatic-styles>";
  out += R"(<style:style style:name="co0" style:family="table-column"><style:table-column-properties fo:break-before="auto" style:column-width="2.258cm"/></style:style>)";
  for (const auto& [length, name] : state.column_styles) {
    out += std::format(
        R"(<style:style style:name="{}" style:family="table-column"><style:table-column-properties fo:break-before="auto" style:column-width="{}"/></style:style>)",
        name, length);
  }
  out += R"(<style:style style:name="ta1" style:family="table"><style:table-properties table:display="true" style:writing-mode="lr-tb"/></style:style>)";
  out += R"(<style:style style:name="ta2" style:family="table"><style:table-properties table:display="false" style:writing-mode="lr-tb"/></style:style>)";
  out += R"(<number:date-style style:name="N1"><number:year number:style="long"/><number:text>-</number:text><number:month number:style="long"/><number:text>-</number:text><number:day number:style="long"/></number:date-style>)";
  out += R"(<number:date-style style:name="N2"><number:year number:style="long"/><number:text>-</number:text><number:month number:style="long"/><number:text>-</number:text><number:day number:style="long"/><number:text> </number:text><number:hours number:style="long"/><number:text>:</number:text><number:minutes number:style="long"/><number:text>:</number:text><number:seconds number:style="long"/></number:date-style>)";
  out += R"(<number:time-style style:name="N3"><number:hours number:style="long"/><number:text>:</number:text><number:minutes number:style="long"/><number:text>:</number:text><number:seconds number:style="long"/></number:time-style>)";
  if (state.header_style) {
    out += R"(<style:style style:name="ce1" style:family="table-cell" style:parent-style-name="Default"><style:table-cell-properties fo:background-color="#e8eef6" fo:border-bottom="0.75pt solid #7f93b2"/><style:text-properties fo:font-weight="bold" style:font-weight-asian="bold" style:font-weight-complex="bold"/></style:style>)";
  } else {
    out += R"(<style:style style:name="ce1" style:family="table-cell" style:parent-style-name="Default"/>)";
  }
  out += R"(<style:style style:name="ce2" style:family="table-cell" style:parent-style-name="Default" style:data-style-name="N1"/>)";
  out += R"(<style:style style:name="ce3" style:family="table-cell" style:parent-style-name="Default" style:data-style-name="N2"/>)";
  out += R"(<style:style style:name="ce4" style:family="table-cell" style:parent-style-name="Default" style:data-style-name="N3"/>)";
  out += "</office:automatic-styles>";
  return out;
}

constexpr std::string_view common_styles =
    R"(<office:styles><style:default-style style:family="table-cell"><style:paragraph-properties style:tab-stop-distance="1.25cm"/></style:default-style>)"
    R"(<style:style style:name="Default" style:family="table-cell"/></office:styles>)";

// Frozen panes live in settings.xml.
std::string settings_xml(const ir::Table& table, const std::vector<std::string>& names, bool freeze_header) {
  std::string tables;
  bool any = false;
  for (std::size_t i = 0; i < table.sheets.size(); ++i) {
    const ir::Sheet& sheet = table.sheets[i];
    std::uint32_t rows = sheet.frozen_rows;
    const std::uint32_t columns = sheet.frozen_columns;
    if (freeze_header && !sheet.rows.empty()) rows = std::max<std::uint32_t>(rows, 1);
    if (rows == 0 && columns == 0) continue;
    any = true;
    tables += "<config:config-item-map-entry config:name=\"";
    xml_escape_append(tables, names[i], true);
    tables += "\">";
    tables += std::format(
        R"(<config:config-item config:name="HorizontalSplitMode" config:type="short">{}</config:config-item>)"
        R"(<config:config-item config:name="VerticalSplitMode" config:type="short">{}</config:config-item>)"
        R"(<config:config-item config:name="HorizontalSplitPosition" config:type="int">{}</config:config-item>)"
        R"(<config:config-item config:name="VerticalSplitPosition" config:type="int">{}</config:config-item>)"
        R"(<config:config-item config:name="ActiveSplitRange" config:type="short">2</config:config-item>)"
        R"(<config:config-item config:name="PositionLeft" config:type="int">0</config:config-item>)"
        R"(<config:config-item config:name="PositionRight" config:type="int">{}</config:config-item>)"
        R"(<config:config-item config:name="PositionTop" config:type="int">0</config:config-item>)"
        R"(<config:config-item config:name="PositionBottom" config:type="int">{}</config:config-item>)",
        columns > 0 ? 2 : 0, rows > 0 ? 2 : 0, columns, rows, columns, rows);
    tables += "</config:config-item-map-entry>";
  }
  if (!any) return {};
  return std::format(
      R"(<?xml version="1.0" encoding="UTF-8"?>
<office:document-settings{} office:version="1.3"><office:settings><config:config-item-set config:name="ooo:view-settings"><config:config-item-map-indexed config:name="Views"><config:config-item-map-entry><config:config-item config:name="ViewId" config:type="string">view1</config:config-item><config:config-item-map-named config:name="Tables">{}</config:config-item-map-named></config:config-item-map-entry></config:config-item-map-indexed></config:config-item-set></office:settings></office:document-settings>)",
      namespaces, tables);
}

Result<std::string> spreadsheet_body(const ir::Table& table, const std::vector<std::string>& names, OdsWriteState& state,
                                     const WriteContext& ctx) {
  const bool auto_widths = ctx.options.get_bool("auto_column_widths", true);
  std::string body = "<office:body><office:spreadsheet>";
  for (std::size_t i = 0; i < table.sheets.size(); ++i) {
    CVT_TRY_ASSIGN(const std::string xml, table_xml(table.sheets[i], names[i], state, ctx, auto_widths));
    body += xml;
  }
  body += "</office:spreadsheet></office:body>";
  return body;
}

Result<Bytes> write_ods(const AnyIR& value, WriteContext& ctx) {
  const ir::Table& input = value.get<ir::Table>();
  ir::Table blank;
  blank.sheets.emplace_back();
  const ir::Table& table = input.sheets.empty() ? blank : input;
  const std::vector<std::string> names = unique_sheet_names(table, 255, "[]*?:/\\'");
  OdsWriteState state;
  state.header_style = ctx.options.get_bool("header_style", true);
  CVT_TRY_ASSIGN(const std::string body, spreadsheet_body(table, names, state, ctx));
  const bool flat = ctx.format_id == "fods";
  const std::string_view mime = ctx.format_id == "ots" ? ots_mime : ods_mime;
  if (flat) {
    std::string doc = std::format(R"(<?xml version="1.0" encoding="UTF-8"?>
<office:document{} office:version="1.3" office:mimetype="{}">)",
                                  namespaces, mime);
    doc += common_styles;
    doc += automatic_styles(state);
    doc += body;
    doc += "</office:document>\n";
    return to_bytes(doc);
  }
  std::string content = std::format(R"(<?xml version="1.0" encoding="UTF-8"?>
<office:document-content{} office:version="1.3">)",
                                    namespaces);
  content += automatic_styles(state);
  content += body;
  content += "</office:document-content>";
  const std::string styles = std::format(R"(<?xml version="1.0" encoding="UTF-8"?>
<office:document-styles{} office:version="1.3">{}</office:document-styles>)",
                                         namespaces, common_styles);
  const std::string settings = settings_xml(table, names, ctx.options.get_bool("freeze_header", false));
  std::string manifest = std::format(R"(<?xml version="1.0" encoding="UTF-8"?>
<manifest:manifest xmlns:manifest="urn:oasis:names:tc:opendocument:xmlns:manifest:1.0" manifest:version="1.3"><manifest:file-entry manifest:full-path="/" manifest:version="1.3" manifest:media-type="{}"/><manifest:file-entry manifest:full-path="content.xml" manifest:media-type="text/xml"/><manifest:file-entry manifest:full-path="styles.xml" manifest:media-type="text/xml"/>)",
                                     mime);
  if (!settings.empty()) manifest += R"(<manifest:file-entry manifest:full-path="settings.xml" manifest:media-type="text/xml"/>)";
  manifest += "</manifest:manifest>";

  util::ZipEntryOptions stored;
  stored.level = 0;
  util::ZipEntryOptions entry;
  entry.level = options::resolved_compression_level(ctx.options);
  util::ZipWriter zip;
  CVT_TRY(zip.add("mimetype", mime, stored));
  CVT_TRY(zip.add("content.xml", content, entry));
  CVT_TRY(zip.add("styles.xml", styles, entry));
  if (!settings.empty()) CVT_TRY(zip.add("settings.xml", settings, entry));
  CVT_TRY(zip.add("META-INF/manifest.xml", manifest, entry));
  return zip.finish();
}

// An ODF package starts with a stored "mimetype" member holding the media type.
int sniff_odf_package(ByteView data, std::string_view mime) {
  util::ByteReader reader(data);
  if (reader.u32le() != 0x04034B50u) return 0;
  reader.skip(4);
  const std::uint16_t method = reader.u16le();
  reader.skip(8);
  const std::uint32_t compressed = reader.u32le();
  reader.skip(4);
  const std::uint16_t name_length = reader.u16le();
  const std::uint16_t extra_length = reader.u16le();
  if (!reader.ok() || method != 0 || name_length != 8 || reader.chars(8) != "mimetype") return 0;
  reader.skip(extra_length);
  if (compressed != mime.size()) return 0;
  return reader.ok() && reader.chars(compressed) == mime ? 95 : 0;
}

int sniff_fods(ByteView data, std::string_view) {
  std::string_view head = as_chars(data).substr(0, 4096);
  if (head.starts_with("\xEF\xBB\xBF")) head.remove_prefix(3);
  head = util::trim_left(head);
  if (!head.starts_with("<")) return 0;
  if (head.find("office:document") == std::string_view::npos) return 0;
  return head.find("application/vnd.oasis.opendocument.spreadsheet\"") != std::string_view::npos ? 92 : 0;
}

}  // namespace

void register_ods(Registry& registry) {
  std::vector<OptionSpec> write_options = {
      OptionSpec::boolean("header_style", "Style the header row", true,
                          "Make the header row bold with a light fill and a thin rule below it."),
      OptionSpec::boolean("freeze_header", "Freeze the header row", false, "Keep the first row visible while scrolling."),
      OptionSpec::boolean("auto_column_widths", "Fit column widths", true,
                          "Size columns to their content when the source does not specify widths."),
      options::compression_level(),
      options::preset(),
      options::strip_metadata(),
  };
  {
    FormatInfo info;
    info.id = "ods";
    info.name = "OpenDocument spreadsheet";
    info.description = "Spreadsheet of LibreOffice, OpenOffice and other OpenDocument applications.";
    info.category = Category::Spreadsheet;
    info.extensions = {"ods"};
    info.mime = std::string(ods_mime);
    info.popularity = 50;
    info.sniff = [](ByteView data, std::string_view) { return sniff_odf_package(data, ods_mime); };
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = "ods";
    codec.kind = Kind::Table;
    codec.read = read_ods;
    codec.write = write_ods;
    codec.write_options = write_options;
    registry.add_codec(std::move(codec));
  }
  {
    FormatInfo info;
    info.id = "ots";
    info.name = "OpenDocument spreadsheet template";
    info.description = "Template for OpenDocument spreadsheets.";
    info.category = Category::Spreadsheet;
    info.extensions = {"ots"};
    info.mime = std::string(ots_mime);
    info.popularity = 10;
    info.sniff = [](ByteView data, std::string_view) { return sniff_odf_package(data, ots_mime); };
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = "ots";
    codec.kind = Kind::Table;
    codec.read = read_ods;
    codec.write = write_ods;
    codec.write_options = write_options;
    registry.add_codec(std::move(codec));
  }
  {
    FormatInfo info;
    info.id = "fods";
    info.name = "Flat OpenDocument spreadsheet";
    info.description = "OpenDocument spreadsheet as a single uncompressed XML file.";
    info.category = Category::Spreadsheet;
    info.extensions = {"fods"};
    info.mime = "application/vnd.oasis.opendocument.spreadsheet";
    info.popularity = 15;
    info.sniff = sniff_fods;
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = "fods";
    codec.kind = Kind::Table;
    codec.read = read_fods;
    codec.write = write_ods;
    codec.write_options = {write_options[0], write_options[2], options::strip_metadata()};
    registry.add_codec(std::move(codec));
  }
}

}  // namespace cvt::table
