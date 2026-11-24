// Convertible core - data family: bridges from ir::Data to other kinds.
//
//   Data -> Table      records become rows (see `flatten`); an object whose
//                      members are all arrays becomes one sheet per member;
//                      any other object becomes a two-column key/value sheet.
//   Data -> PlainText  pretty-printed JSON.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/data.hpp"
#include "ir/plain_text.hpp"
#include "ir/table.hpp"
#include "util/text.hpp"

namespace cvt::data {

namespace {

constexpr std::size_t max_columns = 16384;

struct FlattenStyle {
  bool dotted = true;
  std::string separator = ".";
};

/// A scalar (or tagged value) as a cell.
ir::Cell leaf_cell(const Json& value) {
  switch (tag_kind(value)) {
    case TagKind::None: break;
    case TagKind::Date: {
      const std::string text(tag_text(value));
      if (const auto parsed = parse_datetime(text); parsed && parsed->kind != DateTime::Kind::Time) {
        const double seconds = static_cast<double>(to_unix_seconds(*parsed)) + parsed->nanos() / 1e9;
        return ir::Cell::datetime(seconds, text);
      }
      return ir::Cell::string(text);
    }
    case TagKind::BigInt:
    case TagKind::Decimal: {
      const std::string text(tag_text(value));
      return ir::Cell::numeric(util::parse_double(text).value_or(0.0), text);
    }
    default: {
      const Json plain = plain_value(value);
      if (plain.is_string()) return ir::Cell::string(plain.as_string());
      if (plain.is_number()) return leaf_cell(plain);
      if (plain.is_null()) return ir::Cell{};
      return ir::Cell::string(util::dump_json(plain));
    }
  }
  switch (value.type()) {
    case Json::Type::Null: return ir::Cell{};
    case Json::Type::Bool: return ir::Cell::boolean(value.as_bool());
    case Json::Type::Int64:
    case Json::Type::UInt64: return ir::Cell::numeric(value.as_double(), format_integer(value));
    case Json::Type::Double: {
      const double d = value.as_double();
      return ir::Cell::numeric(d, std::isfinite(d) ? util::format_double(d) : format_double(d));
    }
    case Json::Type::String: return ir::Cell::string(value.as_string());
    default: break;
  }
  return ir::Cell::string(util::dump_json(value));
}

bool is_leaf(const Json& value) { return !value.is_structured() || tag_kind(value) != TagKind::None; }

/// Collects the columns of one record: (path, cell) in document order.
void flatten(const Json& value, std::string& path, const FlattenStyle& style,
             std::vector<std::pair<std::string, ir::Cell>>& out) {
  if (is_leaf(value) || !style.dotted || value.empty()) {
    if (is_leaf(value)) {
      out.emplace_back(path, leaf_cell(value));
    } else {
      auto text = write_json_text(value, JsonStyle{.indent = -1});
      out.emplace_back(path, ir::Cell::string(text ? std::move(*text) : std::string()));
    }
    return;
  }
  const std::size_t length = path.size();
  const auto child = [&](std::string_view key, const Json& element) {
    if (!path.empty()) path += style.separator;
    path += key;
    flatten(element, path, style, out);
    path.resize(length);
  };
  if (value.is_array()) {
    for (std::size_t i = 0; i < value.size(); ++i) child(std::to_string(i), value[i]);
  } else {
    for (const auto& member : value.items()) child(member.key(), member.value());
  }
}

Result<ir::Sheet> records_sheet(const Json& records, const FlattenStyle& style, std::string name, const Context& ctx) {
  ir::Sheet sheet;
  sheet.name = std::move(name);
  sheet.has_header = true;
  std::vector<std::string> header;
  std::map<std::string, std::size_t, std::less<>> column_of;
  std::vector<std::vector<std::pair<std::size_t, ir::Cell>>> rows;
  rows.reserve(records.size());
  std::size_t index = 0;
  for (const Json& record : records.elements()) {
    if ((++index & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(index) / static_cast<double>(records.size())));
    std::vector<std::pair<std::string, ir::Cell>> cells;
    std::string path;
    if (record.is_object() && tag_kind(record) == TagKind::None) {
      // The members of a record are its columns, whatever the flattening.
      for (const auto& member : record.items()) {
        path = member.key();
        flatten(member.value(), path, style, cells);
      }
    } else {
      path = "value";
      if (is_leaf(record)) {
        cells.emplace_back(path, leaf_cell(record));
      } else {
        flatten(record, path, style, cells);
      }
    }
    std::vector<std::pair<std::size_t, ir::Cell>> row;
    row.reserve(cells.size());
    for (auto& [key, cell] : cells) {
      auto found = column_of.find(key);
      if (found == column_of.end()) {
        if (header.size() >= max_columns) {
          return fail(Status::Limit, "the records have more than {} distinct fields; try the option "
                      "flatten = \"json\"", max_columns);
        }
        found = column_of.emplace(key, header.size()).first;
        header.push_back(key);
      }
      row.emplace_back(found->second, std::move(cell));
    }
    rows.push_back(std::move(row));
  }
  std::vector<ir::Cell> header_row;
  header_row.reserve(header.size());
  for (std::string& title : header) header_row.push_back(ir::Cell::string(std::move(title)));
  sheet.rows.push_back(std::move(header_row));
  for (auto& row : rows) {
    std::vector<ir::Cell> cells;
    for (auto& [column, cell] : row) {
      if (cells.size() <= column) cells.resize(column + 1);
      cells[column] = std::move(cell);
    }
    sheet.rows.push_back(std::move(cells));
  }
  return sheet;
}

ir::Sheet key_value_sheet(const Json& object, const FlattenStyle& style) {
  ir::Sheet sheet;
  sheet.has_header = true;
  sheet.rows.push_back({ir::Cell::string("key"), ir::Cell::string("value")});
  std::vector<std::pair<std::string, ir::Cell>> cells;
  std::string path;
  for (const auto& member : object.items()) {
    path = member.key();
    flatten(member.value(), path, style, cells);
  }
  for (auto& [key, cell] : cells) sheet.rows.push_back({ir::Cell::string(std::move(key)), std::move(cell)});
  return sheet;
}

Result<AnyIR> data_to_table(AnyIR value, Context& ctx) {
  const ir::Data data = std::move(value).get<ir::Data>();
  FlattenStyle style;
  style.dotted = ctx.options.get_enum("flatten", "dotted", {"dotted", "json"}) == "dotted";
  style.separator = ctx.options.get_string("path_separator", ".");
  ir::Table table;
  const Json& root = data.value;
  if (root.is_array()) {
    CVT_TRY_ASSIGN(ir::Sheet sheet, records_sheet(root, style, "", ctx));
    table.sheets.push_back(std::move(sheet));
  } else if (root.is_object() && tag_kind(root) == TagKind::None && !root.empty() &&
             std::ranges::all_of(root.items(), [](const auto& m) { return m.value().is_array(); })) {
    for (const auto& member : root.items()) {
      CVT_TRY_ASSIGN(ir::Sheet sheet, records_sheet(member.value(), style, member.key(), ctx));
      table.sheets.push_back(std::move(sheet));
    }
  } else if (root.is_object() && tag_kind(root) == TagKind::None) {
    table.sheets.push_back(key_value_sheet(root, style));
  } else {
    ir::Sheet sheet;
    sheet.rows.push_back({leaf_cell(root)});
    table.sheets.push_back(std::move(sheet));
  }
  return AnyIR(std::move(table));
}

Result<AnyIR> data_to_text(AnyIR value, Context& ctx) {
  const ir::Data data = std::move(value).get<ir::Data>();
  CVT_TRY(check_write_depth(data.value, "text"));
  JsonStyle style;
  style.indent = static_cast<int>(ctx.options.get_int("indent", 2, 0, 16));
  CVT_TRY_ASSIGN(std::string text, write_json_text(data.value, style));
  ir::PlainText out;
  out.text = std::move(text);
  return AnyIR(std::move(out));
}

}  // namespace

void register_data_bridges(Registry& registry) {
  Bridge table;
  table.from = Kind::Data;
  table.to = Kind::Table;
  table.cost = 15;
  table.lossy = true;
  table.label = "lay out records as rows";
  // Records must be in the source (JSON, YAML ...): a syntax tree a bridge just
  // made from a document or source code is no list of records.
  table.after_reader_only = true;
  table.options = {
      OptionSpec::choice("flatten", "Nested values", "dotted", {"dotted", "json"},
                         "Spread nested objects and arrays over columns named by their path (a.b, tags.0), or keep "
                         "each nested value as JSON text in one cell."),
      OptionSpec::text("path_separator", "Path separator", ".", "Joins the parts of a nested column name.")
          .advanced(),
  };
  table.fn = data_to_table;
  registry.add_bridge(std::move(table));

  Bridge text;
  text.from = Kind::Data;
  text.to = Kind::PlainText;
  text.cost = 25;
  text.lossy = false;
  text.label = "pretty-print data as JSON text";
  text.options = {indent_option(2)};
  text.fn = data_to_text;
  registry.add_bridge(std::move(text));
}

}  // namespace cvt::data
