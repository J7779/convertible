// Convertible core - table family: Excel workbook (XLSX/XLSM/XLTX) writer.
//
// Produces the smallest package Excel, Numbers and LibreOffice accept without
// complaint: content types, relationships, workbook, styles, shared strings
// and one part per worksheet. No document properties are written at all, so
// nothing identifies the author, the machine or the time of conversion.
// Cells are typed (numbers, booleans, shared strings, dates as serial numbers
// with a date format), formulas keep their cached values, and a header row
// gets a restrained style (bold, light fill, thin rule below).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/table/common.hpp"
#include "families/table/spreadsheet.hpp"
#include "families/table/xlsx.hpp"
#include "families/table/xml_pull.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::table {

namespace {

constexpr std::uint32_t max_rows = 1'048'576;
constexpr std::uint32_t max_columns = 16'384;
constexpr std::size_t max_cell_text = 32'767;

constexpr std::string_view main_ns = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr std::string_view rel_ns = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

// Number formats and the cell styles (XF records) that use them.
class StyleTable {
 public:
  static constexpr std::uint32_t xf_default = 0;
  static constexpr std::uint32_t xf_header = 1;
  static constexpr std::uint32_t xf_wrap = 2;

  std::uint32_t xf_for_format(const std::string& code) {
    if (code.empty()) return xf_default;
    const auto it = xf_by_code_.find(code);
    if (it != xf_by_code_.end()) return it->second;
    std::uint32_t id = 0;
    if (const auto builtin = builtin_number_format_id(code)) {
      id = *builtin;
    } else {
      id = next_custom_id_++;
      custom_.emplace_back(id, code);
    }
    const auto xf = static_cast<std::uint32_t>(3 + format_ids_.size());
    format_ids_.push_back(id);
    xf_by_code_.emplace(code, xf);
    return xf;
  }

  [[nodiscard]] std::string xml() const {
    std::string out = std::format(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="{}">)",
                                  main_ns);
    if (!custom_.empty()) {
      out += std::format(R"(<numFmts count="{}">)", custom_.size());
      for (const auto& [id, code] : custom_) {
        out += std::format(R"(<numFmt numFmtId="{}" formatCode=")", id);
        xml_escape_append(out, code, true);
        out += "\"/>";
      }
      out += "</numFmts>";
    }
    out +=
        R"(<fonts count="2"><font><sz val="11"/><color theme="1"/><name val="Calibri"/><family val="2"/><scheme val="minor"/></font>)"
        R"(<font><b/><sz val="11"/><color theme="1"/><name val="Calibri"/><family val="2"/><scheme val="minor"/></font></fonts>)"
        R"(<fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill>)"
        R"(<fill><patternFill patternType="solid"><fgColor rgb="FFE8EEF6"/><bgColor indexed="64"/></patternFill></fill></fills>)"
        R"(<borders count="2"><border><left/><right/><top/><bottom/><diagonal/></border>)"
        R"(<border><left/><right/><top/><bottom style="thin"><color rgb="FF7F93B2"/></bottom><diagonal/></border></borders>)"
        R"(<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>)";
    out += std::format(R"(<cellXfs count="{}">)", 3 + format_ids_.size());
    out += R"(<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>)";
    out += R"(<xf numFmtId="0" fontId="1" fillId="2" borderId="1" xfId="0" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1"><alignment vertical="center"/></xf>)";
    out += R"(<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0" applyAlignment="1"><alignment vertical="top" wrapText="1"/></xf>)";
    for (const std::uint32_t id : format_ids_) {
      out += std::format(R"(<xf numFmtId="{}" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>)", id);
    }
    out += "</cellXfs>";
    out += R"(<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>)";
    out += R"(<dxfs count="0"/><tableStyles count="0" defaultTableStyle="TableStyleMedium2" defaultPivotStyle="PivotStyleLight16"/>)";
    out += "</styleSheet>";
    return out;
  }

 private:
  std::map<std::string, std::uint32_t> xf_by_code_;
  std::vector<std::uint32_t> format_ids_;  // numFmtId of XF 3, 4, ...
  std::vector<std::pair<std::uint32_t, std::string>> custom_;
  std::uint32_t next_custom_id_ = 164;
};

class SharedStrings {
 public:
  std::uint32_t index(const std::string& text) {
    ++count_;
    const auto [it, inserted] = map_.emplace(text, static_cast<std::uint32_t>(order_.size()));
    if (inserted) order_.push_back(&it->first);
    return it->second;
  }
  [[nodiscard]] std::string xml() const {
    std::string out = std::format(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<sst xmlns="{}" count="{}" uniqueCount="{}">)",
                                  main_ns, count_, order_.size());
    for (const std::string* text : order_) {
      const bool preserve = !text->empty() && (util::is_ascii_space(text->front()) || util::is_ascii_space(text->back()));
      out += preserve ? R"(<si><t xml:space="preserve">)" : "<si><t>";
      ooxml_escape_append(out, *text);
      out += "</t></si>";
    }
    out += "</sst>";
    return out;
  }
  [[nodiscard]] bool empty() const noexcept { return order_.empty(); }

 private:
  std::unordered_map<std::string, std::uint32_t> map_;
  std::vector<const std::string*> order_;
  std::size_t count_ = 0;
};

struct WriteState {
  StyleTable styles;
  SharedStrings strings;
  bool date1904 = false;
  bool header_style = true;
  std::size_t truncated_texts = 0;
  std::size_t early_dates = 0;
  bool any_formula = false;
};

std::vector<ir::CellRange> clean_merges(const ir::Sheet& sheet, std::uint32_t rows, std::uint32_t columns) {
  std::vector<ir::CellRange> out;
  for (const ir::CellRange& range : sheet.merged) {
    if (range.first_row > range.last_row || range.first_column > range.last_column) continue;
    if (range.first_row == range.last_row && range.first_column == range.last_column) continue;
    if (range.last_row >= rows || range.last_column >= columns) continue;
    const bool overlaps = std::ranges::any_of(out, [&](const ir::CellRange& other) {
      return !(range.last_row < other.first_row || range.first_row > other.last_row ||
               range.last_column < other.first_column || range.first_column > other.last_column);
    });
    if (!overlaps) out.push_back(range);
  }
  return out;
}

std::vector<double> column_widths(const ir::Sheet& sheet, std::size_t columns, bool automatic) {
  std::vector<double> widths(columns, 0.0);
  for (std::size_t c = 0; c < columns && c < sheet.column_widths.size(); ++c) widths[c] = sheet.column_widths[c];
  if (!automatic || !sheet.column_widths.empty()) return widths;
  std::vector<std::size_t> longest(columns, 0);
  const std::size_t sample = std::min<std::size_t>(sheet.rows.size(), 2000);
  for (std::size_t r = 0; r < sample; ++r) {
    const auto& row = sheet.rows[r];
    for (std::size_t c = 0; c < row.size() && c < columns; ++c) {
      const ir::Cell& cell = row[c];
      if (cell.empty()) continue;
      std::size_t width = 0;
      // The longest line of multi-line text decides.
      for (const std::string_view line : util::split_lines(cell.text)) width = std::max(width, display_width(line));
      if (cell.type == ir::CellType::DateTime) width = std::max<std::size_t>(width, 10);
      if (r == 0 && sheet.has_header) width += width / 8 + 1;  // bold
      longest[c] = std::max(longest[c], width);
    }
  }
  for (std::size_t c = 0; c < columns; ++c) {
    if (longest[c] == 0) continue;
    widths[c] = std::clamp(static_cast<double>(longest[c]) + 2.0, 8.43, 80.0);
    widths[c] = std::round(widths[c] * 100.0) / 100.0;
  }
  return widths;
}

void append_cell_ref(std::string& out, std::uint32_t row, std::uint32_t column) {
  out += column_name(column);
  out += std::to_string(std::uint64_t{row} + 1);
}

Result<std::string> sheet_xml(const ir::Sheet& sheet, WriteState& state, const WriteContext& ctx, bool selected,
                              bool freeze_header, bool auto_widths) {
  const auto row_count = static_cast<std::uint32_t>(std::min<std::size_t>(sheet.rows.size(), max_rows));
  const auto column_count = static_cast<std::uint32_t>(std::min<std::size_t>(sheet.column_count(), max_columns));
  if (sheet.rows.size() > max_rows) {
    ctx.warn(std::format("Sheet \"{}\" has {} rows; Excel holds {}, the rest was dropped.", sheet.name, sheet.rows.size(), max_rows));
  }
  if (sheet.column_count() > max_columns) {
    ctx.warn(std::format("Sheet \"{}\" has {} columns; Excel holds {}, the rest was dropped.", sheet.name,
                         sheet.column_count(), max_columns));
  }
  std::string out = std::format(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="{}" xmlns:r="{}">)",
                                main_ns, rel_ns);
  out += "<dimension ref=\"";
  if (row_count == 0 || column_count == 0) {
    out += "A1";
  } else {
    out += "A1:" + a1(row_count - 1, column_count - 1);
  }
  out += "\"/>";

  std::uint32_t frozen_rows = sheet.frozen_rows;
  std::uint32_t frozen_columns = sheet.frozen_columns;
  if (freeze_header && row_count > 0) frozen_rows = std::max<std::uint32_t>(frozen_rows, 1);
  frozen_rows = std::min(frozen_rows, max_rows - 1);
  frozen_columns = std::min(frozen_columns, max_columns - 1);
  out += "<sheetViews><sheetView workbookViewId=\"0\"";
  if (selected) out += " tabSelected=\"1\"";
  if (frozen_rows > 0 || frozen_columns > 0) {
    const std::string pane = frozen_rows > 0 && frozen_columns > 0 ? "bottomRight" : frozen_rows > 0 ? "bottomLeft" : "topRight";
    out += "><pane";
    if (frozen_columns > 0) out += std::format(" xSplit=\"{}\"", frozen_columns);
    if (frozen_rows > 0) out += std::format(" ySplit=\"{}\"", frozen_rows);
    out += std::format(R"( topLeftCell="{}" activePane="{}" state="frozen"/><selection pane="{}"/></sheetView>)",
                       a1(frozen_rows, frozen_columns), pane, pane);
  } else {
    out += "/>";
  }
  out += "</sheetViews><sheetFormatPr defaultRowHeight=\"15\"/>";

  const std::vector<double> widths = column_widths(sheet, column_count, auto_widths);
  if (std::ranges::any_of(widths, [](double w) { return w > 0; })) {
    out += "<cols>";
    for (std::size_t c = 0; c < widths.size(); ++c) {
      if (widths[c] <= 0) continue;
      out += std::format(R"(<col min="{0}" max="{0}" width="{1}" customWidth="1"/>)", c + 1, format_number(widths[c]));
    }
    out += "</cols>";
  }

  out += "<sheetData>";
  for (std::uint32_t r = 0; r < row_count; ++r) {
    if ((r & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(row_count)));
    const auto& row = sheet.rows[r];
    bool open = false;
    const auto cells = static_cast<std::uint32_t>(std::min<std::size_t>(row.size(), max_columns));
    for (std::uint32_t c = 0; c < cells; ++c) {
      const ir::Cell& cell = row[c];
      const std::string_view formula = sheet.formula_at(r, c);
      if (cell.empty() && formula.empty()) continue;
      if (!open) {
        out += std::format("<row r=\"{}\">", std::uint64_t{r} + 1);
        open = true;
      }
      std::uint32_t xf = StyleTable::xf_default;
      const bool header = state.header_style && sheet.has_header && r == 0;
      if (header) {
        xf = StyleTable::xf_header;
      } else if (cell.type == ir::CellType::String && cell.text.find('\n') != std::string::npos) {
        xf = StyleTable::xf_wrap;
      } else if (const std::string code = cell_number_format(sheet, r, c); !code.empty()) {
        xf = state.styles.xf_for_format(code);
      }
      out += "<c r=\"";
      append_cell_ref(out, r, c);
      out += '"';
      if (xf != 0) out += std::format(" s=\"{}\"", xf);
      std::string value;
      std::string type;
      switch (cell.type) {
        case ir::CellType::Empty: break;
        case ir::CellType::String: {
          std::string text = cell.text;
          if (util::utf8_length(text) > max_cell_text) {
            text = truncate_utf8(text, max_cell_text);
            ++state.truncated_texts;
          }
          if (!formula.empty()) {
            type = "str";
            ooxml_escape_append(value, text);
          } else {
            type = "s";
            value = std::to_string(state.strings.index(text));
          }
          break;
        }
        case ir::CellType::Number:
          if (std::isfinite(cell.number)) {
            value = util::format_double(cell.number);
          } else {
            type = "e";
            value = "#NUM!";
          }
          break;
        case ir::CellType::Bool:
          type = "b";
          value = cell.number != 0.0 ? "1" : "0";
          break;
        case ir::CellType::DateTime: {
          const double serial = unix_to_excel_serial(cell.number, state.date1904);
          if (std::isfinite(serial) && serial >= 0.0 && serial < 2958466.0) {
            value = util::format_double(serial);
          } else {
            ++state.early_dates;
            type = "s";
            value = std::to_string(state.strings.index(cell.text));
          }
          break;
        }
      }
      if (!type.empty()) out += std::format(" t=\"{}\"", type);
      out += '>';
      if (!formula.empty()) {
        state.any_formula = true;
        out += "<f>";
        xml_escape_append(out, formula.starts_with('=') ? formula.substr(1) : formula);
        out += "</f>";
      }
      if (!value.empty() || cell.type == ir::CellType::String) out += "<v>" + value + "</v>";
      out += "</c>";
    }
    if (open) out += "</row>";
    CVT_TRY(ctx.limits.check_output(out.size()));
  }
  out += "</sheetData>";
  const auto merges = clean_merges(sheet, max_rows, max_columns);
  if (!merges.empty()) {
    out += std::format("<mergeCells count=\"{}\">", merges.size());
    for (const ir::CellRange& m : merges) {
      out += std::format(R"(<mergeCell ref="{}:{}"/>)", a1(m.first_row, m.first_column), a1(m.last_row, m.last_column));
    }
    out += "</mergeCells>";
  }
  out += R"(<pageMargins left="0.7" right="0.7" top="0.75" bottom="0.75" header="0.3" footer="0.3"/></worksheet>)";
  return out;
}

std::string_view main_content_type(std::string_view format_id) {
  if (format_id == "xlsm") return "application/vnd.ms-excel.sheet.macroEnabled.main+xml";
  if (format_id == "xltx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.template.main+xml";
  return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml";
}

}  // namespace

std::vector<OptionSpec> xlsx_write_options() {
  return {
      OptionSpec::boolean("header_style", "Style the header row", true,
                          "Make the header row bold with a light fill and a thin rule below it."),
      OptionSpec::boolean("freeze_header", "Freeze the header row", false,
                          "Keep the first row visible while scrolling."),
      OptionSpec::boolean("auto_column_widths", "Fit column widths", true,
                          "Size columns to their content when the source does not specify widths."),
      OptionSpec::boolean("date1904", "1904 date system", false,
                          "Store dates in the 1904 date system used by old Mac versions of Excel.")
          .advanced(),
      options::compression_level(),
      options::preset(),
      options::strip_metadata(),
  };
}

Result<Bytes> write_xlsx(const AnyIR& value, WriteContext& ctx) {
  const ir::Table& table = value.get<ir::Table>();
  WriteState state;
  state.date1904 = ctx.options.get_bool("date1904", false);
  state.header_style = ctx.options.get_bool("header_style", true);
  const bool freeze_header = ctx.options.get_bool("freeze_header", false);
  const bool auto_widths = ctx.options.get_bool("auto_column_widths", true);
  util::ZipEntryOptions entry;
  entry.level = options::resolved_compression_level(ctx.options);

  ir::Table blank;
  const ir::Table& source = table.sheets.empty() ? blank : table;
  if (table.sheets.empty()) blank.sheets.emplace_back();
  const std::vector<std::string> names = unique_sheet_names(source, 31, "[]:*?/\\");

  // Excel needs a visible, selected sheet.
  std::size_t active = source.sheets.size();
  for (std::size_t i = 0; i < source.sheets.size(); ++i) {
    if (!source.sheets[i].hidden()) {
      active = i;
      break;
    }
  }
  if (active == source.sheets.size()) {
    active = 0;
    ctx.warn("Every sheet was hidden; the first one was made visible.");
  }

  std::vector<std::string> sheet_parts;
  for (std::size_t i = 0; i < source.sheets.size(); ++i) {
    CVT_TRY_ASSIGN(std::string xml, sheet_xml(source.sheets[i], state, ctx, i == active, freeze_header, auto_widths));
    sheet_parts.push_back(std::move(xml));
  }

  std::string workbook = std::format(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="{}" xmlns:r="{}">)",
                                     main_ns, rel_ns);
  workbook += state.date1904 ? R"(<workbookPr date1904="1"/>)" : "<workbookPr/>";
  workbook += std::format(R"(<bookViews><workbookView activeTab="{}"/></bookViews><sheets>)", active);
  for (std::size_t i = 0; i < source.sheets.size(); ++i) {
    workbook += "<sheet name=\"";
    xml_escape_append(workbook, names[i], true);
    workbook += std::format("\" sheetId=\"{}\"", i + 1);
    const ir::SheetVisibility visibility = i == active ? ir::SheetVisibility::Visible : source.sheets[i].visibility;
    if (visibility == ir::SheetVisibility::Hidden) workbook += " state=\"hidden\"";
    if (visibility == ir::SheetVisibility::VeryHidden) workbook += " state=\"veryHidden\"";
    workbook += std::format(" r:id=\"rId{}\"/>", i + 1);
  }
  workbook += "</sheets>";
  if (state.any_formula) workbook += R"(<calcPr calcId="0" fullCalcOnLoad="1"/>)";
  workbook += "</workbook>";

  std::string workbook_rels = std::format(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)");
  for (std::size_t i = 0; i < source.sheets.size(); ++i) {
    workbook_rels += std::format(
        R"(<Relationship Id="rId{}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet{}.xml"/>)",
        i + 1, i + 1);
  }
  const std::size_t n = source.sheets.size();
  workbook_rels += std::format(
      R"(<Relationship Id="rId{}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>)",
      n + 1);
  if (!state.strings.empty()) {
    workbook_rels += std::format(
        R"(<Relationship Id="rId{}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>)",
        n + 2);
  }
  workbook_rels += "</Relationships>";

  std::string types = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
                      R"(<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>)"
                      R"(<Default Extension="xml" ContentType="application/xml"/>)";
  types += std::format(R"(<Override PartName="/xl/workbook.xml" ContentType="{}"/>)", main_content_type(ctx.format_id));
  for (std::size_t i = 0; i < n; ++i) {
    types += std::format(
        R"(<Override PartName="/xl/worksheets/sheet{}.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>)",
        i + 1);
  }
  types += R"(<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>)";
  if (!state.strings.empty()) {
    types += R"(<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>)";
  }
  types += "</Types>";

  const std::string package_rels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)";

  util::ZipWriter zip;
  CVT_TRY(zip.add("[Content_Types].xml", types, entry));
  CVT_TRY(zip.add("_rels/.rels", package_rels, entry));
  CVT_TRY(zip.add("xl/workbook.xml", workbook, entry));
  CVT_TRY(zip.add("xl/_rels/workbook.xml.rels", workbook_rels, entry));
  CVT_TRY(zip.add("xl/styles.xml", state.styles.xml(), entry));
  if (!state.strings.empty()) CVT_TRY(zip.add("xl/sharedStrings.xml", state.strings.xml(), entry));
  for (std::size_t i = 0; i < n; ++i) {
    CVT_TRY(ctx.checkpoint(0.9 + 0.1 * static_cast<double>(i) / static_cast<double>(n)));
    CVT_TRY(zip.add(std::format("xl/worksheets/sheet{}.xml", i + 1), sheet_parts[i], entry));
  }
  if (state.truncated_texts > 0) {
    ctx.warn(std::format("{} text cell(s) were longer than Excel's limit of {} characters and were cut.",
                         state.truncated_texts, max_cell_text));
  }
  if (state.early_dates > 0) {
    ctx.warn(std::format("{} date(s) before 1900 cannot be Excel dates and were written as text.", state.early_dates));
  }
  return zip.finish();
}

}  // namespace cvt::table
