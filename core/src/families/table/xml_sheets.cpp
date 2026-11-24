// Convertible core - table family: XML spreadsheet formats.
//
//   * SpreadsheetML 2003 ("XML Spreadsheet", Excel 2002/2003): read and
//     write. Typed Data elements, styles with number formats (named and
//     custom) for dates, R1C1 formulas (translated to and from A1), merged
//     cells, column widths, hidden sheets and frozen panes.
//   * Gnumeric (.gnumeric, gzip-compressed or plain XML): read. Value types,
//     dates from style regions, both date conventions, formulas (Gnumeric
//     stores no results for them), merged regions, hidden sheets.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/table/common.hpp"
#include "families/table/spreadsheet.hpp"
#include "families/table/table.hpp"
#include "families/table/xml_pull.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

// --------------------------------------------------------------------------- SpreadsheetML 2003

FormatKind named_format_kind(std::string_view format) {
  if (format == "Short Date" || format == "Medium Date" || format == "Long Date") return FormatKind::Date;
  if (format == "General Date") return FormatKind::DateTime;
  if (format == "Short Time" || format == "Medium Time" || format == "Long Time") return FormatKind::Time;
  if (format == "General" || format.empty()) return FormatKind::General;
  if (format == "General Number" || format == "Fixed" || format == "Standard" || format == "Percent" ||
      format == "Scientific" || format == "Currency" || format == "Euro Currency" || format == "Yes/No" ||
      format == "True/False" || format == "On/Off") {
    return FormatKind::Number;
  }
  return classify_number_format(format);
}

struct XmlStyle {
  std::string format;
  FormatKind kind = FormatKind::General;
};

Result<AnyIR> read_spreadsheetml(ReadContext& ctx) {
  const std::string text = xml_text(ctx.input);
  XmlPull xml(text);
  std::map<std::string, XmlStyle> styles;
  std::string current_style;
  ir::Table table;
  CellBudget budget(ctx.limits);
  ir::Sheet* sheet = nullptr;
  std::uint64_t row = 0;
  std::uint64_t column = 0;
  std::uint64_t column_index = 0;
  bool in_row = false;
  bool in_options = false;
  std::string option_element;
  bool freeze = false;
  std::uint32_t split_rows = 0;
  std::uint32_t split_columns = 0;
  while (true) {
    CVT_TRY_ASSIGN(const XmlPull::Event event, xml.next());
    if (event == XmlPull::Event::End) break;
    const std::string_view name = xml.local_name();
    if (event == XmlPull::Event::EndElement) {
      if (name == "Row") {
        in_row = false;
        ++row;
      } else if (name == "WorksheetOptions") {
        in_options = false;
      } else if (name == "Worksheet" && sheet != nullptr) {
        if (freeze) {
          sheet->frozen_rows = split_rows;
          sheet->frozen_columns = split_columns;
        }
        trim_sheet(*sheet);
        sheet = nullptr;
      }
      option_element.clear();
      continue;
    }
    if (event == XmlPull::Event::Text) {
      if (in_options && sheet != nullptr) {
        const std::string_view value = util::trim(xml.text());
        if (option_element == "Visible") {
          if (value == "SheetHidden") sheet->visibility = ir::SheetVisibility::Hidden;
          if (value == "SheetVeryHidden") sheet->visibility = ir::SheetVisibility::VeryHidden;
        } else if (option_element == "SplitHorizontal") {
          split_rows = static_cast<std::uint32_t>(std::clamp<std::int64_t>(util::parse_int(value).value_or(0), 0, 1048575));
        } else if (option_element == "SplitVertical") {
          split_columns = static_cast<std::uint32_t>(std::clamp<std::int64_t>(util::parse_int(value).value_or(0), 0, 16383));
        }
      }
      continue;
    }
    // Start elements.
    if (name == "Style") {
      current_style = xml.attribute("ss:ID").value_or("");
      styles[current_style];
    } else if (name == "NumberFormat" && !current_style.empty()) {
      XmlStyle& style = styles[current_style];
      style.format = xml.attribute("ss:Format").value_or("");
      style.kind = named_format_kind(style.format);
    } else if (name == "Worksheet") {
      CVT_TRY(ctx.limits.check_archive_entries(table.sheets.size() + 1));
      table.sheets.emplace_back();
      sheet = &table.sheets.back();
      sheet->name = xml.attribute("ss:Name").value_or("");
      row = 0;
      column_index = 0;
      freeze = false;
      split_rows = 0;
      split_columns = 0;
    } else if (sheet == nullptr) {
      continue;
    } else if (name == "WorksheetOptions") {
      in_options = true;
    } else if (in_options) {
      option_element = std::string(name);
      if (name == "FreezePanes") freeze = true;
    } else if (name == "Column") {
      if (const auto index = util::parse_int(xml.attribute("ss:Index").value_or(""))) {
        if (*index >= 1) column_index = static_cast<std::uint64_t>(*index - 1);
      }
      const std::uint64_t span = static_cast<std::uint64_t>(std::clamp<std::int64_t>(util::parse_int(xml.attribute("ss:Span").value_or("0")).value_or(0), 0, 16384)) + 1;
      if (const auto width = util::parse_double(xml.attribute("ss:Width").value_or("")); width && *width > 0) {
        const double characters = std::max(0.0, std::round((*width * 96.0 / 72.0 - 5.0) / 7.0 * 100.0) / 100.0);
        const std::uint64_t end = std::min<std::uint64_t>(column_index + span, 16384);
        if (sheet->column_widths.size() < end) sheet->column_widths.resize(static_cast<std::size_t>(end), 0.0);
        for (std::uint64_t c = column_index; c < end; ++c) sheet->column_widths[static_cast<std::size_t>(c)] = characters;
      }
      column_index += span;
    } else if (name == "Row") {
      if (const auto index = util::parse_int(xml.attribute("ss:Index").value_or("")); index && *index >= 1) {
        row = static_cast<std::uint64_t>(*index - 1);
      }
      in_row = true;
      column = 0;
    } else if (name == "Cell" && in_row) {
      if (const auto index = util::parse_int(xml.attribute("ss:Index").value_or("")); index && *index >= 1) {
        column = static_cast<std::uint64_t>(*index - 1);
      }
      const auto across = static_cast<std::uint32_t>(std::clamp<std::int64_t>(util::parse_int(xml.attribute("ss:MergeAcross").value_or("0")).value_or(0), 0, 16383));
      const auto down = static_cast<std::uint32_t>(std::clamp<std::int64_t>(util::parse_int(xml.attribute("ss:MergeDown").value_or("0")).value_or(0), 0, 1048575));
      const std::string style_id = xml.attribute("ss:StyleID").value_or("");
      const std::string formula = xml.attribute("ss:Formula").value_or("");
      std::string type;
      std::string value;
      bool has_data = false;
      const std::size_t depth = xml.depth();
      while (xml.depth() >= depth) {
        CVT_TRY_ASSIGN(const XmlPull::Event inner, xml.next());
        if (inner == XmlPull::Event::End) return fail(Status::Decode, "the document ends inside a cell");
        if (inner == XmlPull::Event::StartElement && xml.local_name() == "Data") {
          type = xml.attribute("ss:Type").value_or("String");
          CVT_TRY_ASSIGN(value, xml.element_text());
          has_data = true;
        } else if (inner == XmlPull::Event::StartElement) {
          CVT_TRY(xml.skip_element());  // comments, smart tags
        }
      }
      ir::Cell cell;
      const auto style = styles.find(style_id);
      const FormatKind kind = style != styles.end() ? style->second.kind : FormatKind::General;
      if (has_data) {
        if (type == "Number") {
          if (const auto number = util::parse_double(util::trim(value))) {
            cell = number_cell(*number);
          } else {
            cell = ir::Cell::string(value);
          }
        } else if (type == "DateTime") {
          if (const auto date = parse_iso_datetime(value)) {
            double when = date->unix_seconds;
            DateStyle date_style = is_date_kind(kind) ? date_style_for(kind) : DateStyle::Auto;
            // Excel keeps pure times of day on 1899-12-31.
            if (when >= excel_1900_epoch_unix + 86400.0 && when < excel_1900_epoch_unix + 2 * 86400.0) {
              when -= 86400.0;
              date_style = DateStyle::Time;
            }
            cell = datetime_cell(when, date_style);
          } else {
            cell = ir::Cell::string(value);
          }
        } else if (type == "Boolean") {
          cell = ir::Cell::boolean(util::trim(value) == "1" || util::iequals(util::trim(value), "true"));
        } else if (!value.empty() || type == "String") {
          cell = ir::Cell::string(value);
        }
      }
      const auto r = static_cast<std::uint32_t>(std::min<std::uint64_t>(row, UINT32_MAX - 1));
      const auto c = static_cast<std::uint32_t>(std::min<std::uint64_t>(column, UINT32_MAX - 1));
      if (!cell.empty() || !formula.empty()) {
        const bool numeric = cell.type == ir::CellType::Number || cell.type == ir::CellType::DateTime;
        CVT_TRY(put_cell(*sheet, row, column, std::move(cell), budget));
        if (!formula.empty()) {
          std::string_view f = formula;
          if (f.starts_with("of:")) {
            // LibreOffice's export writes OpenFormula here instead of R1C1.
            sheet->formulas[{r, c}] = odf_to_excel_formula(f);
          } else {
            if (f.starts_with('=')) f.remove_prefix(1);
            sheet->formulas[{r, c}] = r1c1_to_a1_formula(f, r, c);
          }
        }
        if (numeric && style != styles.end() && !style->second.format.empty() && kind != FormatKind::General &&
            !style->second.format.starts_with("Short") && !style->second.format.starts_with("Long") &&
            !style->second.format.starts_with("Medium") && style->second.format != "General Date") {
          sheet->number_formats[{r, c}] = style->second.format;
        }
      }
      if (across > 0 || down > 0) {
        if (sheet->merged.size() < 100'000) sheet->merged.push_back(ir::CellRange{r, c, r + down, c + across});
      }
      column += 1 + across;
    }
  }
  if (table.sheets.empty()) return fail(Status::Decode, "the document holds no worksheets");
  guess_headers(table);
  return table;
}

Result<Bytes> write_spreadsheetml(const AnyIR& value, WriteContext& ctx) {
  const ir::Table& input = value.get<ir::Table>();
  ir::Table blank;
  blank.sheets.emplace_back();
  const ir::Table& table = input.sheets.empty() ? blank : input;
  const bool header_style = ctx.options.get_bool("header_style", true);
  const bool freeze_header = ctx.options.get_bool("freeze_header", false);
  const std::vector<std::string> names = unique_sheet_names(table, 31, "[]:*?/\\");
  std::map<std::string, std::string> format_styles;  // code -> style id

  std::string sheets;
  for (std::size_t s = 0; s < table.sheets.size(); ++s) {
    const ir::Sheet& sheet = table.sheets[s];
    sheets += "<Worksheet ss:Name=\"";
    xml_escape_append(sheets, names[s], true);
    sheets += "\"><Table>";
    for (std::size_t c = 0; c < sheet.column_widths.size(); ++c) {
      if (sheet.column_widths[c] <= 0) continue;
      sheets += std::format(R"(<Column ss:Index="{}" ss:Width="{}"/>)", c + 1,
                            format_number(std::round((sheet.column_widths[c] * 7.0 + 5.0) * 72.0 / 96.0 * 100.0) / 100.0));
    }
    const auto covered = [&](std::size_t r, std::size_t c) {
      for (const ir::CellRange& m : sheet.merged) {
        if (m.contains(r, c) && !(m.first_row == r && m.first_column == c)) return true;
      }
      return false;
    };
    for (std::size_t r = 0; r < sheet.rows.size(); ++r) {
      if ((r & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(sheet.rows.size())));
      sheets += std::format("<Row ss:Index=\"{}\">", r + 1);
      const auto& row = sheet.rows[r];
      bool contiguous = false;
      for (std::size_t c = 0; c < row.size(); ++c) {
        const ir::Cell& cell = row[c];
        const std::string_view formula = sheet.formula_at(r, c);
        if ((cell.empty() && formula.empty()) || covered(r, c)) {
          contiguous = false;
          continue;
        }
        sheets += "<Cell";
        if (!contiguous) sheets += std::format(" ss:Index=\"{}\"", c + 1);
        contiguous = true;
        for (const ir::CellRange& m : sheet.merged) {
          if (m.first_row == r && m.first_column == c) {
            if (m.last_column > m.first_column) sheets += std::format(" ss:MergeAcross=\"{}\"", m.last_column - m.first_column);
            if (m.last_row > m.first_row) sheets += std::format(" ss:MergeDown=\"{}\"", m.last_row - m.first_row);
            contiguous = false;
          }
        }
        if (header_style && sheet.has_header && r == 0) {
          sheets += " ss:StyleID=\"sHeader\"";
        } else if (cell.type == ir::CellType::String && cell.text.find('\n') != std::string::npos) {
          sheets += " ss:StyleID=\"sWrap\"";
        } else if (const std::string code = cell_number_format(sheet, r, c); !code.empty()) {
          auto [it, inserted] = format_styles.emplace(code, std::format("f{}", format_styles.size() + 1));
          sheets += std::format(" ss:StyleID=\"{}\"", it->second);
        }
        if (!formula.empty()) {
          sheets += " ss:Formula=\"=";
          xml_escape_append(sheets, a1_to_r1c1_formula(formula, static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(c)), true);
          sheets += '"';
        }
        sheets += '>';
        switch (cell.type) {
          case ir::CellType::Empty: break;
          case ir::CellType::String:
            sheets += "<Data ss:Type=\"String\">";
            xml_escape_append(sheets, cell.text, true);
            sheets += "</Data>";
            break;
          case ir::CellType::Number:
            sheets += std::isfinite(cell.number) ? "<Data ss:Type=\"Number\">" + util::format_double(cell.number) + "</Data>"
                                                 : std::string("<Data ss:Type=\"Error\">#NUM!</Data>");
            break;
          case ir::CellType::Bool:
            sheets += std::format("<Data ss:Type=\"Boolean\">{}</Data>", cell.number != 0 ? 1 : 0);
            break;
          case ir::CellType::DateTime: {
            double when = cell.number;
            if (is_time_only(when)) when += 86400.0;  // Excel keeps times on 1899-12-31
            if (when < excel_1900_epoch_unix) {
              sheets += "<Data ss:Type=\"String\">";
              xml_escape_append(sheets, cell.text);
              sheets += "</Data>";
            } else {
              std::string iso = format_datetime(when, DateStyle::DateTime);
              if (iso.find('.') == std::string::npos) iso += ".000";
              sheets += "<Data ss:Type=\"DateTime\">" + iso + "</Data>";
            }
            break;
          }
        }
        sheets += "</Cell>";
      }
      sheets += "</Row>";
      CVT_TRY(ctx.limits.check_output(sheets.size()));
    }
    sheets += "</Table><WorksheetOptions xmlns=\"urn:schemas-microsoft-com:office:excel\">";
    if (sheet.hidden() && s != 0) {
      sheets += sheet.visibility == ir::SheetVisibility::VeryHidden ? "<Visible>SheetVeryHidden</Visible>"
                                                                     : "<Visible>SheetHidden</Visible>";
    }
    std::uint32_t frozen_rows = sheet.frozen_rows;
    if (freeze_header && !sheet.rows.empty()) frozen_rows = std::max<std::uint32_t>(frozen_rows, 1);
    if (frozen_rows > 0 || sheet.frozen_columns > 0) {
      sheets += "<FreezePanes/><FrozenNoSplit/>";
      if (frozen_rows > 0) sheets += std::format("<SplitHorizontal>{0}</SplitHorizontal><TopRowBottomPane>{0}</TopRowBottomPane>", frozen_rows);
      if (sheet.frozen_columns > 0) {
        sheets += std::format("<SplitVertical>{0}</SplitVertical><LeftColumnRightPane>{0}</LeftColumnRightPane>", sheet.frozen_columns);
      }
      sheets += std::format("<ActivePane>{}</ActivePane>", frozen_rows > 0 && sheet.frozen_columns > 0 ? 0 : frozen_rows > 0 ? 2 : 1);
    }
    sheets += "</WorksheetOptions></Worksheet>";
  }

  std::string out = R"(<?xml version="1.0" encoding="UTF-8"?>
<?mso-application progid="Excel.Sheet"?>
<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet" xmlns:o="urn:schemas-microsoft-com:office:office" xmlns:x="urn:schemas-microsoft-com:office:excel" xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet" xmlns:html="http://www.w3.org/TR/REC-html40">
<Styles><Style ss:ID="Default" ss:Name="Normal"><Alignment ss:Vertical="Bottom"/></Style>)";
  out += header_style
             ? R"(<Style ss:ID="sHeader"><Font ss:Bold="1"/><Interior ss:Color="#E8EEF6" ss:Pattern="Solid"/><Borders><Border ss:Position="Bottom" ss:LineStyle="Continuous" ss:Weight="1" ss:Color="#7F93B2"/></Borders></Style>)"
             : R"(<Style ss:ID="sHeader"/>)";
  out += R"(<Style ss:ID="sWrap"><Alignment ss:Vertical="Top" ss:WrapText="1"/></Style>)";
  for (const auto& [code, id] : format_styles) {
    out += std::format(R"(<Style ss:ID="{}"><NumberFormat ss:Format=")", id);
    xml_escape_append(out, code, true);
    out += "\"/></Style>";
  }
  out += "</Styles>\n";
  out += sheets;
  out += "</Workbook>\n";
  return to_bytes(out);
}

int sniff_spreadsheetml(ByteView data, std::string_view) {
  std::string_view head = as_chars(data).substr(0, 2048);
  if (head.starts_with("\xEF\xBB\xBF")) head.remove_prefix(3);
  if (!util::trim_left(head).starts_with("<")) return 0;
  if (head.find("progid=\"Excel.Sheet\"") != std::string_view::npos) return 92;
  if (head.find("urn:schemas-microsoft-com:office:spreadsheet") != std::string_view::npos &&
      head.find("<Workbook") != std::string_view::npos) {
    return 90;
  }
  return 0;
}

// --------------------------------------------------------------------------- Gnumeric

struct StyleRegion {
  ir::CellRange range;
  std::string format;
};

Result<AnyIR> read_gnumeric(ReadContext& ctx) {
  Bytes inflated;
  ByteView source = ctx.input;
  if (source.size() >= 2 && std::to_integer<std::uint8_t>(source[0]) == 0x1F && std::to_integer<std::uint8_t>(source[1]) == 0x8B) {
    CVT_TRY_ASSIGN(inflated, util::gzip_decompress(source, ctx.limits.max_decompressed_bytes));
    source = inflated;
  }
  const std::string text = xml_text(source);
  XmlPull xml(text);
  ir::Table table;
  CellBudget budget(ctx.limits);
  ir::Sheet* sheet = nullptr;
  std::size_t sheet_depth = 0;
  bool date1904 = false;
  std::vector<StyleRegion> regions;
  std::map<std::string, std::pair<ir::CellPosition, std::string>> expressions;

  const auto finish_sheet = [&] {
    if (sheet == nullptr) return;
    // Numbers formatted as dates become DateTime cells.
    for (const StyleRegion& region : regions) {
      const FormatKind kind = classify_number_format(region.format);
      if (kind == FormatKind::General) continue;
      const std::size_t last_row = std::min<std::size_t>(region.range.last_row, sheet->rows.size() == 0 ? 0 : sheet->rows.size() - 1);
      for (std::size_t r = region.range.first_row; r <= last_row && r < sheet->rows.size(); ++r) {
        auto& row = sheet->rows[r];
        const std::size_t last_column = std::min<std::size_t>(region.range.last_column, row.empty() ? 0 : row.size() - 1);
        for (std::size_t c = region.range.first_column; c <= last_column && c < row.size(); ++c) {
          ir::Cell& cell = row[c];
          if (cell.type != ir::CellType::Number) continue;
          if (is_date_kind(kind)) cell = spreadsheet_number(cell.number, kind, date1904);
          sheet->number_formats[{static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(c)}] = region.format;
        }
      }
    }
    regions.clear();
    trim_sheet(*sheet);
    sheet = nullptr;
  };

  while (true) {
    CVT_TRY_ASSIGN(const XmlPull::Event event, xml.next());
    if (event == XmlPull::Event::End) break;
    const std::string_view name = xml.local_name();
    if (event == XmlPull::Event::EndElement) {
      if (name == "Sheet" && sheet != nullptr && xml.depth() + 1 == sheet_depth) finish_sheet();
      continue;
    }
    if (event != XmlPull::Event::StartElement) continue;
    if (name == "DateConvention") {
      CVT_TRY_ASSIGN(const std::string convention, xml.element_text());
      date1904 = convention.find("1904") != std::string::npos;
    } else if (name == "Sheet" && sheet == nullptr) {
      CVT_TRY(ctx.limits.check_archive_entries(table.sheets.size() + 1));
      table.sheets.emplace_back();
      sheet = &table.sheets.back();
      sheet_depth = xml.depth();
      const std::string visibility = xml.attribute("Visibility").value_or("");
      if (visibility == "GNM_SHEET_VISIBILITY_HIDDEN") sheet->visibility = ir::SheetVisibility::Hidden;
      if (visibility == "GNM_SHEET_VISIBILITY_VERY_HIDDEN") sheet->visibility = ir::SheetVisibility::VeryHidden;
    } else if (sheet == nullptr) {
      continue;
    } else if (name == "Name" && xml.depth() == sheet_depth + 1) {
      CVT_TRY_ASSIGN(sheet->name, xml.element_text());
    } else if (name == "StyleRegion") {
      StyleRegion region;
      const auto get = [&](std::string_view key, std::int64_t max) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(util::parse_int(xml.attribute(key).value_or("0")).value_or(0), 0, max));
      };
      region.range.first_column = get("startCol", 16383);
      region.range.first_row = get("startRow", 1048575);
      region.range.last_column = get("endCol", 16383);
      region.range.last_row = get("endRow", 1048575);
      // The format sits on the child Style element.
      const std::size_t depth = xml.depth();
      while (xml.depth() >= depth) {
        CVT_TRY_ASSIGN(const XmlPull::Event inner, xml.next());
        if (inner == XmlPull::Event::End) return fail(Status::Decode, "the document ends inside a style region");
        if (inner == XmlPull::Event::StartElement && xml.local_name() == "Style") {
          region.format = xml.attribute("Format").value_or("");
        }
      }
      if (!region.format.empty() && region.format != "General" && regions.size() < 100'000) regions.push_back(std::move(region));
    } else if (name == "Cell") {
      const auto row = util::parse_int(xml.attribute("Row").value_or(""));
      const auto column = util::parse_int(xml.attribute("Col").value_or(""));
      const auto type = util::parse_int(xml.attribute("ValueType").value_or("0")).value_or(0);
      const std::string expr_id = xml.attribute("ExprID").value_or("");
      CVT_TRY_ASSIGN(std::string value, xml.element_text());
      if (!row || !column || *row < 0 || *column < 0 || *row >= 16'777'216 || *column >= 65'536) continue;
      const auto r = static_cast<std::uint32_t>(*row);
      const auto c = static_cast<std::uint32_t>(*column);
      ir::Cell cell;
      std::string formula;
      if (!expr_id.empty()) {
        if (value.starts_with('=')) {
          expressions[expr_id] = {{r, c}, value.substr(1)};
          formula = value.substr(1);
        } else if (const auto it = expressions.find(expr_id); it != expressions.end()) {
          formula = shift_formula(it->second.second, std::int64_t{r} - it->second.first.first,
                                  std::int64_t{c} - it->second.first.second);
        }
      } else if (type == 0 && value.starts_with('=')) {
        formula = value.substr(1);
      }
      if (formula.empty()) {
        switch (type) {
          case 20: cell = ir::Cell::boolean(util::iequals(util::trim(value), "TRUE")); break;
          case 30:
          case 40:
            if (const auto number = util::parse_double(util::trim(value))) cell = number_cell(*number);
            break;
          case 50:
          case 60: cell = ir::Cell::string(std::move(value)); break;
          default: break;
        }
      }
      if (!cell.empty() || !formula.empty()) {
        CVT_TRY(put_cell(*sheet, r, c, std::move(cell), budget));
        if (!formula.empty()) sheet->formulas[{r, c}] = formula;
      }
    } else if (name == "Merge") {
      CVT_TRY_ASSIGN(const std::string ref, xml.element_text());
      if (const auto range = parse_a1_range(util::trim(ref)); range && sheet->merged.size() < 100'000) sheet->merged.push_back(*range);
    } else if (name == "ColInfo") {
      const auto first = util::parse_int(xml.attribute("No").value_or(""));
      const auto points = util::parse_double(xml.attribute("Unit").value_or(""));
      const auto count = util::parse_int(xml.attribute("Count").value_or("1")).value_or(1);
      if (first && points && *first >= 0 && *first < 16384 && *points > 0 && xml.attribute("HardSize").value_or("0") == "1") {
        const auto end = static_cast<std::size_t>(std::min<std::int64_t>(*first + std::max<std::int64_t>(count, 1), 16384));
        if (sheet->column_widths.size() < end) sheet->column_widths.resize(end, 0.0);
        for (auto k = static_cast<std::size_t>(*first); k < end; ++k) {
          sheet->column_widths[k] = std::max(0.0, std::round((*points * 96.0 / 72.0 - 5.0) / 7.0 * 100.0) / 100.0);
        }
      }
    }
  }
  finish_sheet();
  if (table.sheets.empty()) return fail(Status::Decode, "the document holds no Gnumeric sheets");
  guess_headers(table);
  return table;
}

int sniff_gnumeric(ByteView data, std::string_view) {
  if (data.size() >= 2 && std::to_integer<std::uint8_t>(data[0]) == 0x1F && std::to_integer<std::uint8_t>(data[1]) == 0x8B) {
    // Peek into the gzip member for the Gnumeric namespace.
    util::Inflater inflater(util::DeflateFormat::Gzip);
    Bytes head;
    (void)inflater.feed(data.first(std::min<std::size_t>(data.size(), 4096)), head, 64 * 1024);
    return as_chars(head).find("http://www.gnumeric.org/v10.dtd") != std::string_view::npos ? 95 : 0;
  }
  const std::string_view head = as_chars(data).substr(0, 2048);
  return head.find("<gnm:Workbook") != std::string_view::npos ? 92 : 0;
}

}  // namespace

void register_spreadsheetml(Registry& registry) {
  FormatInfo info;
  info.id = "spreadsheetml";
  info.name = "Excel 2003 XML spreadsheet";
  info.description = "SpreadsheetML 2003 (\"XML Spreadsheet\"), Excel's XML workbook format before XLSX.";
  info.category = Category::Spreadsheet;
  info.extensions = {"xml"};
  info.mime = "application/xml";
  info.popularity = 15;
  info.sniff = sniff_spreadsheetml;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "spreadsheetml";
  codec.kind = Kind::Table;
  codec.read = read_spreadsheetml;
  codec.write = write_spreadsheetml;
  codec.write_options = {
      OptionSpec::boolean("header_style", "Style the header row", true,
                          "Make the header row bold with a light fill and a rule below it."),
      OptionSpec::boolean("freeze_header", "Freeze the header row", false, "Keep the first row visible while scrolling."),
      options::strip_metadata(),
  };
  registry.add_codec(std::move(codec));
}

void register_gnumeric(Registry& registry) {
  FormatInfo info;
  info.id = "gnumeric";
  info.name = "Gnumeric spreadsheet";
  info.description = "Workbook of the Gnumeric spreadsheet (gzip-compressed XML).";
  info.category = Category::Spreadsheet;
  info.extensions = {"gnumeric", "gnm"};
  info.mime = "application/x-gnumeric";
  info.popularity = 10;
  info.sniff = sniff_gnumeric;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "gnumeric";
  codec.kind = Kind::Table;
  codec.read = read_gnumeric;
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::table
