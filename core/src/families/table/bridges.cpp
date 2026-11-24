// Convertible core - table family: bridges from ir::Table to other kinds.
//
//   Table -> Data      rows as an array of objects keyed by the header row
//                      (or as arrays), one sheet or all sheets keyed by name;
//   Table -> Document  each sheet as a heading and a table block;
//   Table -> Archive   one file per sheet in any table format (csv by default),
//                      which is how a workbook reaches single-table formats
//                      without losing its other sheets.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/table/common.hpp"
#include "families/table/table.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

Json cell_json(const ir::Cell& cell) {
  switch (cell.type) {
    case ir::CellType::Empty: return nullptr;
    case ir::CellType::String: return Json(cell.text);
    case ir::CellType::Bool: return Json(cell.number != 0.0);
    case ir::CellType::DateTime: return Json(format_datetime(cell.number));
    case ir::CellType::Number:
      if (std::isfinite(cell.number) && cell.number == std::trunc(cell.number) && std::fabs(cell.number) <= 9007199254740992.0) {
        // Exact integers stay integers; the display text keeps digits beyond 2^53.
        if (const auto exact = util::parse_int(cell.text)) return Json(*exact);
        return Json(static_cast<std::int64_t>(cell.number));
      }
      if (const auto exact = util::parse_int(cell.text)) return Json(*exact);
      return Json(cell.number);
  }
  return nullptr;
}

std::vector<std::string> column_keys(const ir::Sheet& sheet, std::size_t columns) {
  std::vector<std::string> keys;
  std::set<std::string> used;
  for (std::size_t c = 0; c < columns; ++c) {
    std::string key = sheet.rows.empty() ? std::string() : std::string(util::trim(sheet.at(0, c).text));
    if (key.empty()) key = std::format("column_{}", c + 1);
    std::string candidate = key;
    for (int n = 2; used.contains(candidate); ++n) candidate = std::format("{}_{}", key, n);
    used.insert(candidate);
    keys.push_back(std::move(candidate));
  }
  return keys;
}

Json sheet_json(const ir::Sheet& sheet, std::string_view layout) {
  const bool records = layout == "records" || (layout == "auto" && sheet.has_header);
  Json rows = Json::array();
  if (!records) {
    for (const auto& row : sheet.rows) {
      Json values = Json::array();
      for (const ir::Cell& cell : row) values.push_back(cell_json(cell));
      rows.push_back(std::move(values));
    }
    return rows;
  }
  const std::size_t columns = sheet.column_count();
  const std::vector<std::string> keys = column_keys(sheet, columns);
  for (std::size_t r = 1; r < sheet.rows.size(); ++r) {
    Json object = Json::object();
    for (std::size_t c = 0; c < columns; ++c) object.set(keys[c], cell_json(sheet.at(r, c)));
    rows.push_back(std::move(object));
  }
  return rows;
}

Result<AnyIR> table_to_data(AnyIR value, Context& ctx) {
  const ir::Table table = std::move(value).get<ir::Table>();
  const std::string layout = ctx.options.get_enum("data_layout", "auto", {"auto", "records", "arrays"});
  ir::Data data;
  if (ctx.options.get_bool("all_sheets", false)) {
    data.value = Json::object();
    const std::vector<std::string> names = unique_sheet_names(table, 255, "");
    for (std::size_t i = 0; i < table.sheets.size(); ++i) data.value.set(names[i], sheet_json(table.sheets[i], layout));
    return data;
  }
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(table, ctx));
  data.value = sheet_json(*sheet, layout);
  return data;
}

Result<AnyIR> table_to_document(AnyIR value, Context& ctx) {
  const ir::Table table = std::move(value).get<ir::Table>();
  const bool headings = ctx.options.get_bool("sheet_headings", true);
  const bool include_hidden = ctx.options.get_bool("include_hidden", false);
  ir::Document document;
  std::size_t shown = 0;
  for (const ir::Sheet& sheet : table.sheets) shown += sheet.hidden() && !include_hidden ? 0 : 1;
  for (const ir::Sheet& sheet : table.sheets) {
    if (sheet.hidden() && !include_hidden) continue;
    if (headings && (shown > 1 || !sheet.name.empty())) {
      document.blocks.push_back(ir::Heading{2, {ir::Text{sheet.name.empty() ? std::string("Sheet") : sheet.name}}});
    }
    ir::TableBlock block;
    const std::size_t columns = sheet.column_count();
    for (std::size_t r = 0; r < sheet.rows.size(); ++r) {
      ir::TableRow row;
      row.header = r == 0 && sheet.has_header;
      for (std::size_t c = 0; c < columns; ++c) {
        const ir::CellRange* merge = nullptr;
        for (const ir::CellRange& m : sheet.merged) {
          if (m.contains(r, c)) merge = &m;
        }
        if (merge != nullptr && (merge->first_row != r || merge->first_column != c)) continue;  // covered
        ir::TableCell cell;
        const std::string& text = sheet.at(r, c).text;
        if (!text.empty()) {
          // Line breaks inside a cell become explicit breaks.
          bool first = true;
          for (const std::string_view line : util::split(text, '\n')) {
            if (!first) cell.content.push_back(ir::LineBreak{});
            if (!line.empty()) cell.content.push_back(ir::Text{std::string(line)});
            first = false;
          }
        }
        if (merge != nullptr) {
          cell.colspan = static_cast<int>(std::min<std::uint32_t>(merge->last_column - merge->first_column + 1, 1000));
          cell.rowspan = static_cast<int>(std::min<std::uint32_t>(merge->last_row - merge->first_row + 1, 65535));
        }
        row.cells.push_back(std::move(cell));
      }
      block.rows.push_back(std::move(row));
    }
    if (!block.rows.empty()) document.blocks.push_back(std::move(block));
  }
  return document;
}

std::string safe_file_stem(std::string_view name) {
  std::string out;
  for (const char c : name) {
    const auto u = static_cast<unsigned char>(c);
    out.push_back(u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                          c == '>' || c == '|'
                      ? '_'
                      : c);
  }
  out = std::string(util::trim(out));
  while (!out.empty() && (out.front() == '.' || out.back() == '.')) {
    if (out.front() == '.') out.erase(out.begin());
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  return out.empty() ? std::string("sheet") : out;
}

Result<AnyIR> table_to_archive(AnyIR value, Context& ctx) {
  const ir::Table table = std::move(value).get<ir::Table>();
  if (ctx.registry == nullptr) return fail(Status::Internal, "no registry available to write the sheets");
  const std::string format = normalize_format_id(ctx.options.get_string("sheet_format", "csv"));
  const Codec* writer = nullptr;
  for (const Codec* codec : ctx.registry->codecs_for(format)) {
    if (codec->kind == Kind::Table && codec->write) {
      writer = codec;
      break;
    }
  }
  const FormatInfo* info = ctx.registry->find(format);
  if (writer == nullptr || info == nullptr) {
    return fail(Status::InvalidArgument, "'{}' is not a table format that can be written (option 'sheet_format')", format);
  }
  const bool include_hidden = ctx.options.get_bool("include_hidden", false);
  CVT_TRY(ctx.limits.check_archive_entries(table.sheets.size()));
  const std::string extension = info->extensions.empty() ? format : info->extensions.front();
  const std::vector<std::string> names = unique_sheet_names(table, 120, "");
  std::set<std::string> used;
  ir::Archive archive;
  for (std::size_t i = 0; i < table.sheets.size(); ++i) {
    if (table.sheets[i].hidden() && !include_hidden) continue;
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(table.sheets.size())));
    ir::Table single;
    single.sheets.push_back(table.sheets[i]);
    WriteContext write;
    const double n = static_cast<double>(table.sheets.size());
    write.progress = ctx.progress.sub(static_cast<double>(i) / n, static_cast<double>(i + 1) / n);
    write.limits = ctx.limits;
    write.warnings = ctx.warnings;
    write.registry = ctx.registry;
    write.format_id = format;
    Options options = ctx.options;
    options.erase("sheet");
    CVT_TRY_ASSIGN(write.options, options.normalized(writer->write_options));
    CVT_TRY_ASSIGN(Bytes bytes, writer->write(AnyIR(std::move(single)), write));
    std::string stem = safe_file_stem(names[i]);
    std::string file = stem + "." + extension;
    for (int copy = 2; used.contains(util::to_lower(file)); ++copy) file = std::format("{} ({}).{}", stem, copy, extension);
    used.insert(util::to_lower(file));
    ir::ArchiveEntry entry;
    entry.path = std::move(file);
    entry.bytes = std::move(bytes);
    archive.entries.push_back(std::move(entry));
  }
  if (archive.entries.empty()) return fail(Status::Encode, "the workbook has no sheets to write");
  return archive;
}

}  // namespace

void register_bridges(Registry& registry) {
  {
    Bridge bridge;
    bridge.from = Kind::Table;
    bridge.to = Kind::Data;
    bridge.cost = 15;
    bridge.label = "table rows as structured data";
    bridge.fn = table_to_data;
    bridge.options = {
        OptionSpec::choice("data_layout", "Row layout", "auto", {"auto", "records", "arrays"},
                           "records: one object per row keyed by the header row; arrays: one array per row; auto uses "
                           "records when the table has a header row."),
        OptionSpec::boolean("all_sheets", "All sheets", false,
                            "Write every sheet, as an object keyed by sheet name, instead of a single sheet."),
        sheet_option(),
    };
    registry.add_bridge(std::move(bridge));
  }
  {
    Bridge bridge;
    bridge.from = Kind::Table;
    bridge.to = Kind::Document;
    bridge.cost = 15;
    bridge.lossy = true;
    bridge.label = "render sheets as document tables";
    bridge.fn = table_to_document;
    bridge.options = {
        OptionSpec::boolean("sheet_headings", "Sheet headings", true, "Put each sheet's name above its table."),
        OptionSpec::boolean("include_hidden", "Include hidden sheets", false, "Also render sheets that are hidden."),
    };
    registry.add_bridge(std::move(bridge));
  }
  {
    Bridge bridge;
    bridge.from = Kind::Table;
    bridge.to = Kind::Archive;
    // Above the cost of packing the file itself (implicit Blob read + wrap):
    // "md -> zip" or "xlsx -> zip" means the file in an archive, not one CSV
    // per table. The bridge only serves sources that have no bytes of their own.
    bridge.cost = 2000;
    bridge.label = "one file per sheet";
    bridge.fn = table_to_archive;
    bridge.options = {
        OptionSpec::text("sheet_format", "Sheet file format", "csv",
                         "Format of the file written for each sheet (csv, tsv, xlsx, json-compatible table formats...)."),
        OptionSpec::boolean("include_hidden", "Include hidden sheets", false, "Also write sheets that are hidden."),
    };
    registry.add_bridge(std::move(bridge));
  }
}

}  // namespace cvt::table
