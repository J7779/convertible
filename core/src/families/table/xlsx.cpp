// Convertible core - table family: Excel workbooks (XLSX, XLSM, XLTX, XLTM)
// reader, and the Office Open XML package helpers.
//
// Worksheets are walked with the streaming XmlPull reader so that sheets
// with millions of cells need memory for the cells only. Shared and inline
// strings (rich text flattened, phonetic runs dropped), numbers, booleans,
// errors, ISO dates, dates and times recognised from number formats in both
// date systems, formulas with their cached values (shared formulas are
// expanded), hidden sheets, merged cells, column widths and frozen panes
// are read.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/table/common.hpp"
#include "families/table/ooxml.hpp"
#include "families/table/spreadsheet.hpp"
#include "families/table/table.hpp"
#include "families/table/xlsx.hpp"
#include "families/table/xml_pull.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::table {

// --------------------------------------------------------------------------- package helpers

std::string rels_path_for(std::string_view part) {
  const std::size_t slash = part.rfind('/');
  if (slash == std::string_view::npos) return "_rels/" + std::string(part) + ".rels";
  return std::string(part.substr(0, slash + 1)) + "_rels/" + std::string(part.substr(slash + 1)) + ".rels";
}

std::string resolve_part_name(std::string_view source_part, std::string_view target) {
  std::string joined;
  if (target.starts_with('/')) {
    joined = std::string(target.substr(1));
  } else {
    const std::size_t slash = source_part.rfind('/');
    joined = (slash == std::string_view::npos ? std::string() : std::string(source_part.substr(0, slash + 1))) +
             std::string(target);
  }
  std::vector<std::string> segments;
  for (const std::string_view piece : util::split(joined, '/', false)) {
    if (piece == ".") continue;
    if (piece == "..") {
      if (!segments.empty()) segments.pop_back();
      continue;
    }
    segments.emplace_back(piece);
  }
  return util::join(segments, "/");
}

Result<std::string> read_xml_part(util::ZipReader& zip, std::string_view name) {
  auto index = zip.find(name);
  if (!index) {
    // Part names are case-insensitive in OPC.
    for (std::size_t i = 0; i < zip.entries().size(); ++i) {
      if (util::iequals(zip.entries()[i].name, name)) {
        index = i;
        break;
      }
    }
  }
  if (!index) return fail(Status::Decode, "the package has no part '{}'", name);
  CVT_TRY_ASSIGN(const Bytes bytes, zip.read(*index));
  return xml_text(bytes);
}

Result<std::vector<Relationship>> read_relationships(util::ZipReader& zip, std::string_view source_part) {
  std::vector<Relationship> out;
  const std::string path = rels_path_for(source_part);
  auto text = read_xml_part(zip, path);
  if (!text) return out;  // a part without relationships
  XmlPull xml(*text);
  while (true) {
    CVT_TRY_ASSIGN(const XmlPull::Event event, xml.next());
    if (event == XmlPull::Event::End) break;
    if (event != XmlPull::Event::StartElement || xml.local_name() != "Relationship") continue;
    Relationship rel;
    rel.id = xml.attribute("Id").value_or("");
    rel.type = xml.attribute("Type").value_or("");
    rel.external = xml.attribute("TargetMode").value_or("") == "External";
    const std::string target = xml.attribute("Target").value_or("");
    rel.target = rel.external ? target : resolve_part_name(source_part, target);
    out.push_back(std::move(rel));
  }
  return out;
}

Result<std::string> main_part(util::ZipReader& zip, std::string_view fallback) {
  CVT_TRY_ASSIGN(const auto rels, read_relationships(zip, ""));
  for (const Relationship& rel : rels) {
    if (rel.type.ends_with("/officeDocument") && !rel.external) return rel.target;
  }
  return std::string(fallback);
}

std::string excel_package_kind(ByteView data) {
  if (!as_chars(data).starts_with("PK\x03\x04")) return {};
  Limits limits;
  limits.max_decompressed_bytes = 4 * 1024 * 1024;
  auto zip = util::ZipReader::open(data, limits);
  if (!zip) {
    // Truncated or partial archive (no central directory): the tell-tale
    // member names in the local headers still identify a workbook.
    const std::string_view text = as_chars(data).substr(0, 64 * 1024);
    if (text.find("xl/workbook.bin") != std::string_view::npos) return "xlsb";
    if (text.find("xl/workbook") != std::string_view::npos) return "xlsx";
    return {};
  }
  const auto types = zip->find("[Content_Types].xml");
  if (types && zip->entries()[*types].uncompressed_size < 2 * 1024 * 1024) {
    if (auto bytes = zip->read(*types)) {
      const std::string_view text = as_chars(*bytes);
      if (text.find("spreadsheetml.sheet.binary.macroEnabled.main") != std::string_view::npos ||
          text.find("ms-excel.sheet.binary.macroEnabled.main") != std::string_view::npos) {
        return "xlsb";
      }
      if (text.find("ms-excel.sheet.macroEnabled.main+xml") != std::string_view::npos) return "xlsm";
      if (text.find("ms-excel.template.macroEnabled.main+xml") != std::string_view::npos) return "xltm";
      if (text.find("spreadsheetml.template.main+xml") != std::string_view::npos) return "xltx";
      if (text.find("spreadsheetml.sheet.main+xml") != std::string_view::npos) return "xlsx";
    }
  }
  if (zip->contains("xl/workbook.bin")) return "xlsb";
  if (zip->contains("xl/workbook.xml")) return "xlsx";
  return {};
}

// --------------------------------------------------------------------------- reading

namespace {

struct Styles {
  std::vector<FormatKind> kinds;   // by cell XF index
  std::vector<std::string> codes;  // by cell XF index ("" = General)
};

Result<Styles> read_styles(util::ZipReader& zip, std::string_view part) {
  Styles styles;
  if (part.empty()) return styles;
  auto text = read_xml_part(zip, part);
  if (!text) return styles;  // no styles: every cell is General
  std::map<std::uint32_t, std::string> custom;
  XmlPull xml(*text);
  bool in_cell_xfs = false;
  while (true) {
    CVT_TRY_ASSIGN(const XmlPull::Event event, xml.next());
    if (event == XmlPull::Event::End) break;
    if (event == XmlPull::Event::EndElement) {
      if (xml.local_name() == "cellXfs") in_cell_xfs = false;
      continue;
    }
    if (event != XmlPull::Event::StartElement) continue;
    const std::string_view name = xml.local_name();
    if (name == "numFmt") {
      const auto id = util::parse_int(xml.attribute("numFmtId").value_or(""));
      if (id && *id >= 0 && *id < 100000) custom[static_cast<std::uint32_t>(*id)] = xml.attribute("formatCode").value_or("");
    } else if (name == "cellXfs") {
      in_cell_xfs = true;
    } else if (name == "xf" && in_cell_xfs) {
      if (styles.codes.size() >= 1'000'000) return fail(Status::Limit, "the workbook defines too many cell styles");
      const auto id = util::parse_int(xml.attribute("numFmtId").value_or("0")).value_or(0);
      std::string code;
      if (id >= 0) {
        const auto it = custom.find(static_cast<std::uint32_t>(id));
        code = it != custom.end() ? it->second : std::string(builtin_number_format(static_cast<std::uint32_t>(id)));
      }
      styles.kinds.push_back(classify_number_format(code));
      styles.codes.push_back(std::move(code));
    }
  }
  return styles;
}

Result<std::vector<std::string>> read_shared_strings(util::ZipReader& zip, std::string_view part, CellBudget& budget,
                                                     const ReadContext& ctx) {
  std::vector<std::string> strings;
  if (part.empty()) return strings;
  auto text = read_xml_part(zip, part);
  if (!text) return strings;
  XmlPull xml(*text);
  std::string current;
  bool in_item = false;
  int in_t = 0;
  while (true) {
    CVT_TRY_ASSIGN(const XmlPull::Event event, xml.next());
    if (event == XmlPull::Event::End) break;
    if (event == XmlPull::Event::StartElement) {
      const std::string_view name = xml.local_name();
      if (name == "si") {
        in_item = true;
        current.clear();
      } else if (name == "rPh" || name == "phoneticPr") {
        CVT_TRY(xml.skip_element());
      } else if (name == "t" && in_item) {
        ++in_t;
      }
    } else if (event == XmlPull::Event::EndElement) {
      const std::string_view name = xml.local_name();
      if (name == "t" && in_t > 0) {
        --in_t;
      } else if (name == "si" && in_item) {
        in_item = false;
        if ((strings.size() & 4095) == 0) {
          CVT_TRY(budget.take(4096));
          CVT_TRY(ctx.checkpoint(0.05 * static_cast<double>(xml.position()) / static_cast<double>(text->size())));
        }
        strings.push_back(ooxml_unescape(current));
      }
    } else if (event == XmlPull::Event::Text && in_t > 0) {
      current.append(xml.text());
    }
  }
  return strings;
}

struct SharedFormula {
  std::string formula;
  std::uint32_t row = 0;
  std::uint32_t column = 0;
};

struct SheetInfo {
  std::string name;
  std::string part;
  ir::SheetVisibility visibility = ir::SheetVisibility::Visible;
};

Result<void> read_worksheet(std::string_view xml_text_value, ir::Sheet& sheet, const Styles& styles,
                            const std::vector<std::string>& strings, bool date1904, CellBudget& budget,
                            const ReadContext& ctx, double progress_base, double progress_span) {
  XmlPull xml(xml_text_value);
  std::int64_t row = -1;
  std::int64_t column = -1;
  std::map<std::string, SharedFormula> shared;
  std::size_t bad_strings = 0;
  std::size_t rows_seen = 0;
  bool in_sheet_data = false;
  while (true) {
    CVT_TRY_ASSIGN(const XmlPull::Event event, xml.next());
    if (event == XmlPull::Event::End) break;
    if (event == XmlPull::Event::EndElement) {
      if (xml.local_name() == "sheetData") in_sheet_data = false;
      continue;
    }
    if (event != XmlPull::Event::StartElement) continue;
    const std::string_view name = xml.local_name();
    if (name == "sheetData") {
      in_sheet_data = true;
    } else if (name == "row" && in_sheet_data) {
      const auto r = util::parse_int(xml.attribute("r").value_or(""));
      row = r && *r >= 1 && *r <= 1'048'576 * 16 ? *r - 1 : row + 1;
      column = -1;
      if ((++rows_seen & 1023) == 0) {
        CVT_TRY(ctx.checkpoint(progress_base + progress_span * static_cast<double>(xml.position()) /
                                                   static_cast<double>(std::max<std::size_t>(1, xml_text_value.size()))));
      }
    } else if (name == "c" && in_sheet_data) {
      if (row < 0) row = 0;
      if (const auto ref = xml.attribute("r")) {
        if (const auto pos = parse_a1(*ref)) {
          row = pos->first;
          column = pos->second;
        } else {
          ++column;
        }
      } else {
        ++column;
      }
      const std::string type = xml.attribute("t").value_or("n");
      const auto style_index = util::parse_int(xml.attribute("s").value_or("0")).value_or(0);
      FormatKind kind = FormatKind::General;
      std::string_view code;
      if (style_index >= 0 && static_cast<std::size_t>(style_index) < styles.kinds.size()) {
        kind = styles.kinds[static_cast<std::size_t>(style_index)];
        code = styles.codes[static_cast<std::size_t>(style_index)];
      }
      // Children: v, f, is.
      std::string value;
      bool has_value = false;
      std::string inline_text;
      bool has_inline = false;
      std::string formula;
      bool has_formula = false;
      const std::size_t cell_depth = xml.depth();
      while (xml.depth() >= cell_depth) {
        CVT_TRY_ASSIGN(const XmlPull::Event inner, xml.next());
        if (inner == XmlPull::Event::End) return fail(Status::Decode, "the worksheet ends inside a cell");
        if (inner != XmlPull::Event::StartElement) continue;
        const std::string_view child = xml.local_name();
        if (child == "v") {
          CVT_TRY_ASSIGN(value, xml.element_text());
          has_value = !value.empty() || type == "s" || type == "str" || type == "inlineStr";
        } else if (child == "f") {
          const std::string formula_type = xml.attribute("t").value_or("");
          const std::string si = xml.attribute("si").value_or("");
          const bool master = xml.attribute("ref").has_value();
          CVT_TRY_ASSIGN(formula, xml.element_text());
          if (formula_type == "shared" && !si.empty()) {
            if (master || !formula.empty()) {
              if (shared.size() < 100'000) {
                shared[si] = SharedFormula{formula, static_cast<std::uint32_t>(row), static_cast<std::uint32_t>(column)};
              }
            } else if (const auto it = shared.find(si); it != shared.end()) {
              formula = shift_formula(it->second.formula, row - it->second.row, column - it->second.column);
            }
          }
          has_formula = !formula.empty();
        } else if (child == "is") {
          has_inline = true;
          const std::size_t is_depth = xml.depth();
          int in_t = 0;
          while (xml.depth() >= is_depth) {
            CVT_TRY_ASSIGN(const XmlPull::Event e, xml.next());
            if (e == XmlPull::Event::End) return fail(Status::Decode, "the worksheet ends inside a cell");
            if (e == XmlPull::Event::StartElement) {
              if (xml.local_name() == "rPh") {
                CVT_TRY(xml.skip_element());
              } else if (xml.local_name() == "t") {
                ++in_t;
              }
            } else if (e == XmlPull::Event::EndElement && xml.local_name() == "t" && in_t > 0) {
              --in_t;
            } else if (e == XmlPull::Event::Text && in_t > 0) {
              inline_text.append(xml.text());
            }
          }
        } else {
          CVT_TRY(xml.skip_element());
        }
      }
      ir::Cell cell;
      if (type == "s") {
        if (has_value) {
          const auto index = util::parse_int(util::trim(value));
          if (index && *index >= 0 && static_cast<std::size_t>(*index) < strings.size()) {
            cell = ir::Cell::string(strings[static_cast<std::size_t>(*index)]);
          } else {
            ++bad_strings;
          }
        }
      } else if (type == "inlineStr") {
        if (has_inline) {
          cell = ir::Cell::string(ooxml_unescape(inline_text));
        } else if (has_value) {
          cell = ir::Cell::string(ooxml_unescape(value));
        }
      } else if (type == "str") {
        if (has_value) cell = ir::Cell::string(ooxml_unescape(value));
      } else if (type == "b") {
        if (has_value) cell = ir::Cell::boolean(util::trim(value) == "1" || util::iequals(util::trim(value), "true"));
      } else if (type == "e") {
        if (has_value) cell = ir::Cell::string(std::string(util::trim(value)));
      } else if (type == "d") {
        if (has_value) {
          if (const auto date = parse_iso_datetime(value)) {
            cell = datetime_cell(date->unix_seconds, is_date_kind(kind) ? date_style_for(kind) : date->style);
          } else {
            cell = ir::Cell::string(value);
          }
        }
      } else if (has_value) {
        if (const auto number = util::parse_double(util::trim(value))) {
          cell = spreadsheet_number(*number, kind, date1904);
        } else {
          cell = ir::Cell::string(value);
        }
      }
      if (row >= 0 && column >= 0 && (!cell.empty() || has_formula)) {
        const auto r = static_cast<std::uint32_t>(row);
        const auto c = static_cast<std::uint32_t>(column);
        const bool numeric = cell.type == ir::CellType::Number || cell.type == ir::CellType::DateTime;
        CVT_TRY(put_cell(sheet, static_cast<std::uint64_t>(row), static_cast<std::uint64_t>(column), std::move(cell),
                         budget));
        if (has_formula) sheet.formulas[{r, c}] = formula.starts_with('=') ? formula.substr(1) : formula;
        if (numeric && !code.empty() && kind != FormatKind::General) sheet.number_formats[{r, c}] = std::string(code);
      }
    } else if (name == "mergeCell") {
      if (const auto range = parse_a1_range(xml.attribute("ref").value_or(""))) {
        if (sheet.merged.size() < 100'000 && (range->first_row != range->last_row || range->first_column != range->last_column)) {
          sheet.merged.push_back(*range);
        }
      }
    } else if (name == "col") {
      const auto first = util::parse_int(xml.attribute("min").value_or(""));
      const auto last = util::parse_int(xml.attribute("max").value_or(""));
      const auto width = util::parse_double(xml.attribute("width").value_or(""));
      if (first && last && width && *first >= 1 && *last >= *first && *first <= 16384 && *width > 0 &&
          xml.attribute("customWidth").value_or("0") != "0") {
        const auto end = static_cast<std::size_t>(std::min<std::int64_t>(*last, 16384));
        if (sheet.column_widths.size() < end) sheet.column_widths.resize(end, 0.0);
        for (auto i = static_cast<std::size_t>(*first - 1); i < end; ++i) sheet.column_widths[i] = *width;
      }
    } else if (name == "pane") {
      const std::string state = xml.attribute("state").value_or("");
      if (state == "frozen" || state == "frozenSplit") {
        const auto x = util::parse_double(xml.attribute("xSplit").value_or("0")).value_or(0);
        const auto y = util::parse_double(xml.attribute("ySplit").value_or("0")).value_or(0);
        if (x >= 0 && x < 16384) sheet.frozen_columns = static_cast<std::uint32_t>(x);
        if (y >= 0 && y < 1048576) sheet.frozen_rows = static_cast<std::uint32_t>(y);
      }
    }
  }
  if (bad_strings > 0) ctx.warn(std::format("{} cell(s) refer to missing shared strings and were left empty.", bad_strings));
  return {};
}

}  // namespace

Result<ir::Table> read_xlsx_package(ByteView input, const ReadContext& ctx) {
  CVT_TRY_ASSIGN(auto zip, util::ZipReader::open(input, ctx.limits));
  CVT_TRY_ASSIGN(const std::string workbook_part, main_part(zip, "xl/workbook.xml"));
  if (workbook_part.ends_with(".bin")) return fail(Status::Decode, "this is a binary (XLSB) workbook");
  CVT_TRY_ASSIGN(const std::string workbook_xml, read_xml_part(zip, workbook_part));
  CVT_TRY_ASSIGN(const auto rels, read_relationships(zip, workbook_part));

  bool date1904 = false;
  std::vector<SheetInfo> sheets;
  {
    XmlPull xml(workbook_xml);
    while (true) {
      CVT_TRY_ASSIGN(const XmlPull::Event event, xml.next());
      if (event == XmlPull::Event::End) break;
      if (event != XmlPull::Event::StartElement) continue;
      const std::string_view name = xml.local_name();
      if (name == "workbookPr") {
        const std::string flag = xml.attribute("date1904").value_or("0");
        date1904 = flag == "1" || flag == "true";
      } else if (name == "sheet") {
        SheetInfo info;
        info.name = xml.attribute("name").value_or("");
        const std::string state = xml.attribute("state").value_or("visible");
        info.visibility = state == "hidden"       ? ir::SheetVisibility::Hidden
                          : state == "veryHidden" ? ir::SheetVisibility::VeryHidden
                                                  : ir::SheetVisibility::Visible;
        const std::string id = xml.attribute("r:id").value_or(xml.attribute("id").value_or(""));
        for (const Relationship& rel : rels) {
          if (rel.id == id) info.part = rel.target;
        }
        CVT_TRY(ctx.limits.check_archive_entries(sheets.size() + 1));
        sheets.push_back(std::move(info));
      }
    }
  }
  std::string styles_part;
  std::string strings_part;
  for (const Relationship& rel : rels) {
    if (rel.type.ends_with("/styles")) styles_part = rel.target;
    if (rel.type.ends_with("/sharedStrings")) strings_part = rel.target;
  }
  CellBudget budget(ctx.limits);
  CVT_TRY_ASSIGN(const Styles styles, read_styles(zip, styles_part));
  CVT_TRY_ASSIGN(const auto strings, read_shared_strings(zip, strings_part, budget, ctx));

  ir::Table table;
  for (std::size_t i = 0; i < sheets.size(); ++i) {
    const SheetInfo& info = sheets[i];
    ir::Sheet sheet;
    sheet.name = info.name;
    sheet.visibility = info.visibility;
    if (info.part.empty() || !zip.contains(info.part)) {
      // Chart sheets and dialog sheets have no cells.
      if (!info.part.empty() && !zip.contains(info.part)) ctx.warn(std::format("Sheet \"{}\" is missing from the file.", info.name));
      table.sheets.push_back(std::move(sheet));
      continue;
    }
    CVT_TRY_ASSIGN(const std::string text, read_xml_part(zip, info.part));
    const double base = 0.05 + 0.95 * static_cast<double>(i) / static_cast<double>(sheets.size());
    CVT_TRY(read_worksheet(text, sheet, styles, strings, date1904, budget, ctx, base, 0.95 / static_cast<double>(sheets.size())));
    trim_sheet(sheet);
    table.sheets.push_back(std::move(sheet));
  }
  guess_headers(table);
  return table;
}

namespace {

Result<AnyIR> read_xlsx(ReadContext& ctx) {
  CVT_TRY_ASSIGN(ir::Table table, read_xlsx_package(ctx.input, ctx));
  return table;
}

int sniff_kind(ByteView data, std::string_view wanted) {
  const std::string kind = excel_package_kind(data);
  if (kind.empty()) return 0;
  if (kind == wanted) return 95;
  return 0;
}

}  // namespace

void register_xlsx(Registry& registry) {
  struct Flavour {
    std::string_view id;
    std::string_view name;
    std::string_view description;
    std::string_view mime;
    int popularity;
  };
  const Flavour flavours[] = {
      {"xlsx", "Excel workbook", "Microsoft Excel spreadsheet (Office Open XML).",
       "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", 92},
      {"xlsm", "Excel macro-enabled workbook", "Excel workbook that may contain macros (the macros are not run or kept).",
       "application/vnd.ms-excel.sheet.macroEnabled.12", 40},
      {"xltx", "Excel template", "Excel workbook template.",
       "application/vnd.openxmlformats-officedocument.spreadsheetml.template", 25},
      {"xltm", "Excel macro-enabled template", "Excel workbook template that may contain macros.",
       "application/vnd.ms-excel.template.macroEnabled.12", 10},
  };
  for (const Flavour& flavour : flavours) {
    FormatInfo info;
    info.id = std::string(flavour.id);
    info.name = std::string(flavour.name);
    info.description = std::string(flavour.description);
    info.category = Category::Spreadsheet;
    info.extensions = {std::string(flavour.id)};
    info.mime = std::string(flavour.mime);
    info.popularity = flavour.popularity;
    const std::string wanted(flavour.id);
    info.sniff = [wanted](ByteView data, std::string_view) { return sniff_kind(data, wanted); };
    registry.add_format(std::move(info));

    Codec codec;
    codec.format_id = std::string(flavour.id);
    codec.kind = Kind::Table;
    codec.read = read_xlsx;
    if (flavour.id != "xltm") {
      codec.write = write_xlsx;
      codec.write_options = xlsx_write_options();
    }
    registry.add_codec(std::move(codec));
  }
  register_xlsb(registry);
}

}  // namespace cvt::table
