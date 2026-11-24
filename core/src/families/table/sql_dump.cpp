// Convertible core - table family: SQL scripts (dumps).
//
// Writer: CREATE TABLE plus batched multi-row INSERT statements for SQLite,
// MySQL/MariaDB or PostgreSQL, with each dialect's identifier quoting, types
// and string escaping, inside one transaction.
//
// Reader: the data of simple dumps - CREATE TABLE for column names and
// types, INSERT INTO ... VALUES with any number of rows, and PostgreSQL's
// COPY ... FROM stdin blocks (pg_dump's default). Other statements are
// skipped. MySQL backslash escapes are understood when the script uses
// backtick-quoted identifiers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/table/common.hpp"
#include "families/table/sql.hpp"
#include "families/table/table.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

// --------------------------------------------------------------------------- writing

enum class Dialect { Sqlite, Mysql, Postgres };

std::string quote_identifier(std::string_view name, Dialect dialect) {
  if (dialect == Dialect::Mysql) return "`" + util::replace_all(name, "`", "``") + "`";
  return "\"" + util::replace_all(name, "\"", "\"\"") + "\"";
}

std::string quote_string(std::string_view text, Dialect dialect) {
  std::string out = "'";
  for (const char c : util::sanitize_utf8(text)) {
    if (c == '\'') {
      out += "''";
    } else if (dialect == Dialect::Mysql && c == '\\') {
      out += "\\\\";
    } else if (c == '\0') {
      out += dialect == Dialect::Mysql ? "\\0" : "";  // PostgreSQL text cannot hold NUL
    } else {
      out.push_back(c);
    }
  }
  return out + "'";
}

std::string column_type(std::string_view generic, Dialect dialect, const ir::Sheet& sheet, std::size_t column,
                        std::size_t first_row) {
  if (generic == "DATETIME") {
    bool dates = true;
    bool times = true;
    for (std::size_t r = first_row; r < sheet.rows.size(); ++r) {
      const ir::Cell& cell = sheet.at(r, column);
      if (cell.type != ir::CellType::DateTime) continue;
      const double day_seconds = cell.number - std::floor(cell.number / 86400.0) * 86400.0;
      dates = dates && day_seconds == 0.0;
      times = times && is_time_only(cell.number);
    }
    if (times) return "TIME";
    if (dates) return "DATE";
    return dialect == Dialect::Postgres ? "TIMESTAMP" : "DATETIME";
  }
  if (generic == "INTEGER") return dialect == Dialect::Sqlite ? "INTEGER" : "BIGINT";
  if (generic == "REAL") return dialect == Dialect::Mysql ? "DOUBLE" : dialect == Dialect::Postgres ? "DOUBLE PRECISION" : "REAL";
  if (generic == "BOOLEAN") return "BOOLEAN";
  return "TEXT";
}

std::string sql_value(const ir::Cell& cell, std::string_view type, Dialect dialect) {
  const bool text_column = type == "TEXT";
  switch (cell.type) {
    case ir::CellType::Empty: return "NULL";
    case ir::CellType::String: return quote_string(cell.text, dialect);
    case ir::CellType::Number: {
      if (!std::isfinite(cell.number)) return "NULL";
      std::string number = cell.number == std::trunc(cell.number) && std::fabs(cell.number) < 9.0e15
                               ? std::format("{}", static_cast<std::int64_t>(cell.number))
                               : util::format_double(cell.number);
      if (const auto exact = util::parse_int(cell.text); exact && std::fabs(cell.number) >= 9.0e15) number = std::to_string(*exact);
      return text_column && dialect != Dialect::Sqlite ? quote_string(number, dialect) : number;
    }
    case ir::CellType::Bool: {
      if (text_column && dialect != Dialect::Sqlite) return cell.number != 0 ? "'TRUE'" : "'FALSE'";
      if (dialect == Dialect::Sqlite) return cell.number != 0 ? "1" : "0";
      return cell.number != 0 ? "TRUE" : "FALSE";
    }
    case ir::CellType::DateTime: {
      std::string text = type == "TIME" ? format_datetime(cell.number, DateStyle::Time)
                         : type == "DATE" ? format_datetime(cell.number, DateStyle::Date)
                                          : format_datetime(cell.number, DateStyle::DateTime);
      if (type != "TIME" && type != "DATE" && is_time_only(cell.number) && text_column) text = format_datetime(cell.number, DateStyle::Time);
      std::ranges::replace(text, 'T', ' ');
      return quote_string(text, dialect);
    }
  }
  return "NULL";
}

Result<Bytes> write_sql(const AnyIR& value, WriteContext& ctx) {
  const ir::Table& table = value.get<ir::Table>();
  const std::string dialect_name = ctx.options.get_enum("sql_dialect", "sqlite", {"sqlite", "mysql", "postgres"});
  const Dialect dialect = dialect_name == "mysql" ? Dialect::Mysql : dialect_name == "postgres" ? Dialect::Postgres : Dialect::Sqlite;
  const auto batch = static_cast<std::size_t>(ctx.options.get_int("batch_size", 100, 1, 100000));
  const bool create = ctx.options.get_bool("create_table", true);
  const bool drop = ctx.options.get_bool("drop_table", false);
  const std::string header_mode = ctx.options.get_enum("column_names", "auto", {"auto", "first_row", "generated"});
  std::string out;
  out += dialect == Dialect::Mysql ? "START TRANSACTION;\n" : "BEGIN;\n";
  std::map<std::string, int> used;
  for (std::size_t s = 0; s < table.sheets.size(); ++s) {
    const ir::Sheet& sheet = table.sheets[s];
    std::string name = util::sanitize_utf8(sheet.name);
    if (util::trim(name).empty()) name = std::format("Sheet{}", s + 1);
    if (const int n = used[util::to_lower(name)]++; n > 0) name += std::format("_{}", n + 1);
    const std::string table_name = quote_identifier(name, dialect);
    const bool use_header = header_mode == "first_row" || (header_mode == "auto" && sheet.has_header);
    const SqlColumns columns = sql_columns(sheet, use_header);
    std::vector<std::string> types;
    for (std::size_t c = 0; c < columns.names.size(); ++c) {
      types.push_back(column_type(columns.types[c], dialect, sheet, c, columns.first_data_row));
    }
    out += "\n";
    if (drop) out += "DROP TABLE IF EXISTS " + table_name + ";\n";
    if (create && !columns.names.empty()) {
      out += "CREATE TABLE " + table_name + " (\n";
      for (std::size_t c = 0; c < columns.names.size(); ++c) {
        out += "  " + quote_identifier(columns.names[c], dialect) + " " + types[c] + (c + 1 < columns.names.size() ? ",\n" : "\n");
      }
      out += ");\n";
    }
    if (columns.names.empty()) continue;
    std::string column_list = "(";
    for (std::size_t c = 0; c < columns.names.size(); ++c) column_list += (c > 0 ? ", " : "") + quote_identifier(columns.names[c], dialect);
    column_list += ")";
    for (std::size_t r = columns.first_data_row; r < sheet.rows.size(); r += batch) {
      if (((r / batch) & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(sheet.rows.size())));
      out += "INSERT INTO " + table_name + " " + column_list + " VALUES\n";
      const std::size_t end = std::min(sheet.rows.size(), r + batch);
      for (std::size_t k = r; k < end; ++k) {
        out += "  (";
        for (std::size_t c = 0; c < columns.names.size(); ++c) {
          if (c > 0) out += ", ";
          out += sql_value(sheet.at(k, c), types[c], dialect);
        }
        out += k + 1 < end ? "),\n" : ");\n";
      }
      CVT_TRY(ctx.limits.check_output(out.size()));
    }
  }
  out += "\nCOMMIT;\n";
  return to_bytes(out);
}

// --------------------------------------------------------------------------- reading

struct Token {
  enum Kind { End, Word, Identifier, String, Number, Blob, Punct } kind = End;
  std::string text;
};

class SqlLexer {
 public:
  SqlLexer(std::string_view sql, bool backslash_escapes) : sql_(sql), backslash_(backslash_escapes) {}
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  void seek(std::size_t pos) noexcept { pos_ = pos; }
  [[nodiscard]] std::string_view source() const noexcept { return sql_; }

  Token next() {
    skip_space();
    if (pos_ >= sql_.size()) return {};
    const char c = sql_[pos_];
    if ((c == 'X' || c == 'x') && pos_ + 1 < sql_.size() && sql_[pos_ + 1] == '\'') {
      ++pos_;
      Token t = quoted('\'', false);
      t.kind = Token::Blob;
      return t;
    }
    if ((c == 'E' || c == 'e') && pos_ + 1 < sql_.size() && sql_[pos_ + 1] == '\'') {
      ++pos_;
      return quoted('\'', true);
    }
    if (c == '\'') return quoted('\'', backslash_);
    if (c == '"' || c == '`') {
      Token t = quoted(c, false);
      t.kind = Token::Identifier;
      return t;
    }
    if (c == '[') {
      const std::size_t close = sql_.find(']', pos_);
      Token t{Token::Identifier, std::string(sql_.substr(pos_ + 1, close == std::string_view::npos ? std::string_view::npos : close - pos_ - 1))};
      pos_ = close == std::string_view::npos ? sql_.size() : close + 1;
      return t;
    }
    if (util::is_ascii_digit(c) || (c == '.' && pos_ + 1 < sql_.size() && util::is_ascii_digit(sql_[pos_ + 1]))) {
      const std::size_t start = pos_;
      while (pos_ < sql_.size() && (util::is_ascii_digit(sql_[pos_]) || sql_[pos_] == '.' || sql_[pos_] == 'e' || sql_[pos_] == 'E' ||
                                    ((sql_[pos_] == '+' || sql_[pos_] == '-') && (sql_[pos_ - 1] == 'e' || sql_[pos_ - 1] == 'E')))) {
        ++pos_;
      }
      return {Token::Number, std::string(sql_.substr(start, pos_ - start))};
    }
    if (util::is_ascii_alpha(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
      const std::size_t start = pos_;
      while (pos_ < sql_.size() && (util::is_ascii_alpha(sql_[pos_]) || util::is_ascii_digit(sql_[pos_]) || sql_[pos_] == '_' ||
                                    sql_[pos_] == '$' || static_cast<unsigned char>(sql_[pos_]) >= 0x80)) {
        ++pos_;
      }
      return {Token::Word, std::string(sql_.substr(start, pos_ - start))};
    }
    ++pos_;
    return {Token::Punct, std::string(1, c)};
  }

  /// Skip to just after the next top-level ';'.
  void skip_statement() {
    while (true) {
      const Token t = next();
      if (t.kind == Token::End || (t.kind == Token::Punct && t.text == ";")) return;
    }
  }

 private:
  void skip_space() {
    while (pos_ < sql_.size()) {
      const char c = sql_[pos_];
      if (util::is_ascii_space(c)) {
        ++pos_;
      } else if (c == '-' && pos_ + 1 < sql_.size() && sql_[pos_ + 1] == '-') {
        while (pos_ < sql_.size() && sql_[pos_] != '\n') ++pos_;
      } else if (c == '#' && backslash_) {
        while (pos_ < sql_.size() && sql_[pos_] != '\n') ++pos_;
      } else if (c == '/' && pos_ + 1 < sql_.size() && sql_[pos_ + 1] == '*') {
        const std::size_t end = sql_.find("*/", pos_ + 2);
        pos_ = end == std::string_view::npos ? sql_.size() : end + 2;
      } else {
        break;
      }
    }
  }

  Token quoted(char quote, bool backslash) {
    Token t{Token::String, {}};
    ++pos_;
    while (pos_ < sql_.size()) {
      const char c = sql_[pos_];
      if (backslash && c == '\\' && pos_ + 1 < sql_.size()) {
        const char e = sql_[pos_ + 1];
        switch (e) {
          case 'n': t.text.push_back('\n'); break;
          case 't': t.text.push_back('\t'); break;
          case 'r': t.text.push_back('\r'); break;
          case '0': t.text.push_back('\0'); break;
          case 'Z': t.text.push_back('\x1A'); break;
          case 'b': t.text.push_back('\b'); break;
          default: t.text.push_back(e); break;
        }
        pos_ += 2;
        continue;
      }
      if (c == quote) {
        if (pos_ + 1 < sql_.size() && sql_[pos_ + 1] == quote) {
          t.text.push_back(quote);
          pos_ += 2;
          continue;
        }
        ++pos_;
        return t;
      }
      t.text.push_back(c);
      ++pos_;
    }
    return t;
  }

  std::string_view sql_;
  std::size_t pos_ = 0;
  bool backslash_ = false;
};

bool keyword(const Token& t, std::string_view word) { return t.kind == Token::Word && util::iequals(t.text, word); }

struct DumpTable {
  std::string name;
  std::vector<std::string> columns;
  std::vector<std::string> types;
  ir::Sheet sheet;
};

// A qualified name ("schema"."table") -> its last part.
std::optional<std::string> read_name(SqlLexer& lexer, Token& t) {
  t = lexer.next();
  if (keyword(t, "IF")) {  // IF NOT EXISTS
    lexer.next();
    lexer.next();
    t = lexer.next();
  }
  if (t.kind != Token::Word && t.kind != Token::Identifier) return std::nullopt;
  std::string name = t.text;
  while (true) {
    const std::size_t save = lexer.position();
    Token dot = lexer.next();
    if (dot.kind == Token::Punct && dot.text == ".") {
      Token part = lexer.next();
      name = part.text;
      continue;
    }
    lexer.seek(save);
    break;
  }
  return name;
}

ir::Cell typed_cell(ir::Cell cell, std::string_view type) {
  const std::string upper = util::to_upper(type);
  if (cell.type == ir::CellType::String && (upper.find("DATE") != std::string::npos || upper.find("TIME") != std::string::npos)) {
    if (const auto date = parse_iso_datetime(cell.text)) return ir::Cell::datetime(date->unix_seconds, cell.text);
  }
  if (cell.type == ir::CellType::Number && upper.find("BOOL") != std::string::npos && (cell.number == 0 || cell.number == 1)) {
    return ir::Cell::boolean(cell.number == 1);
  }
  return cell;
}

Result<AnyIR> read_sql(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, decode_source_text(ctx.input, ctx.options.get_string("source_encoding", "auto"), ctx));
  const bool mysql = text.find('`') != std::string::npos;
  SqlLexer lexer(text, mysql);
  std::vector<DumpTable> tables;
  std::map<std::string, std::size_t> by_name;
  CellBudget budget(ctx.limits);
  const auto table_for = [&](const std::string& name) -> Result<DumpTable*> {
    const std::string key = util::to_lower(name);
    if (const auto it = by_name.find(key); it != by_name.end()) return &tables[it->second];
    CVT_TRY(ctx.limits.check_archive_entries(tables.size() + 1));
    by_name[key] = tables.size();
    tables.emplace_back();
    tables.back().name = name;
    tables.back().sheet.name = name;
    return &tables.back();
  };
  std::size_t statements = 0;
  while (true) {
    if ((++statements & 255) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(lexer.position()) / static_cast<double>(text.size())));
    Token t = lexer.next();
    if (t.kind == Token::End) break;
    if (keyword(t, "CREATE")) {
      Token next = lexer.next();
      while (keyword(next, "TEMP") || keyword(next, "TEMPORARY") || keyword(next, "UNLOGGED") || keyword(next, "OR") ||
             keyword(next, "REPLACE")) {
        next = lexer.next();
      }
      if (!keyword(next, "TABLE")) {
        lexer.skip_statement();
        continue;
      }
      Token name_token;
      const auto name = read_name(lexer, name_token);
      Token open = lexer.next();
      if (!name || open.text != "(") {
        lexer.skip_statement();
        continue;
      }
      CVT_TRY_ASSIGN(DumpTable * table, table_for(*name));
      table->columns.clear();
      table->types.clear();
      int depth = 0;
      bool at_start = true;
      bool in_definition = false;
      std::string type;
      while (true) {
        Token c = lexer.next();
        if (c.kind == Token::End) break;
        if (c.kind == Token::Punct && c.text == "(") {
          ++depth;
          continue;
        }
        if (c.kind == Token::Punct && c.text == ")") {
          if (depth == 0) break;
          --depth;
          continue;
        }
        if (depth > 0) continue;
        if (c.kind == Token::Punct && c.text == ",") {
          if (in_definition) table->types.push_back(type);
          at_start = true;
          in_definition = false;
          type.clear();
          continue;
        }
        if (at_start) {
          at_start = false;
          if (keyword(c, "PRIMARY") || keyword(c, "UNIQUE") || keyword(c, "KEY") || keyword(c, "INDEX") || keyword(c, "CONSTRAINT") ||
              keyword(c, "FOREIGN") || keyword(c, "CHECK") || keyword(c, "FULLTEXT") || keyword(c, "SPATIAL")) {
            continue;
          }
          table->columns.push_back(c.text);
          in_definition = true;
          continue;
        }
        if (in_definition && c.kind == Token::Word && type.find(' ') == std::string::npos && !keyword(c, "NOT") && !keyword(c, "NULL") &&
            !keyword(c, "DEFAULT") && !keyword(c, "PRIMARY")) {
          type += type.empty() ? c.text : "";
        }
      }
      if (in_definition) table->types.push_back(type);
      lexer.skip_statement();
      if (table->sheet.rows.empty()) {
        std::vector<ir::Cell> header;
        for (const std::string& column : table->columns) header.push_back(ir::Cell::string(column));
        table->sheet.rows.push_back(std::move(header));
        table->sheet.has_header = true;
      }
      continue;
    }
    if (keyword(t, "INSERT") || keyword(t, "REPLACE")) {
      Token into = lexer.next();
      while (into.kind == Token::Word && !keyword(into, "INTO")) into = lexer.next();
      Token name_token;
      const auto name = read_name(lexer, name_token);
      if (!name) {
        lexer.skip_statement();
        continue;
      }
      CVT_TRY_ASSIGN(DumpTable * table, table_for(*name));
      std::vector<std::string> columns;
      Token next = lexer.next();
      if (next.kind == Token::Punct && next.text == "(") {
        while (true) {
          Token c = lexer.next();
          if (c.kind == Token::End || c.text == ")") break;
          if (c.kind == Token::Word || c.kind == Token::Identifier) columns.push_back(c.text);
        }
        next = lexer.next();
      }
      if (!keyword(next, "VALUES") && !keyword(next, "VALUE")) {
        lexer.skip_statement();
        continue;
      }
      if (table->sheet.rows.empty()) {
        if (table->columns.empty()) table->columns = columns;
        if (!table->columns.empty()) {
          std::vector<ir::Cell> header;
          for (const std::string& column : table->columns) header.push_back(ir::Cell::string(column));
          table->sheet.rows.push_back(std::move(header));
          table->sheet.has_header = true;
        }
      }
      // Map the statement's columns onto the table's.
      std::vector<std::size_t> target;
      for (std::size_t k = 0; k < columns.size(); ++k) {
        std::size_t index = k;
        for (std::size_t c = 0; c < table->columns.size(); ++c) {
          if (util::iequals(table->columns[c], columns[k])) index = c;
        }
        target.push_back(index);
      }
      // Rows: ( v, v, ... ) separated by commas.
      while (true) {
        Token open = lexer.next();
        if (open.kind == Token::End || (open.kind == Token::Punct && open.text == ";")) break;
        if (open.kind == Token::Punct && open.text == ",") continue;
        if (!(open.kind == Token::Punct && open.text == "(")) {
          lexer.skip_statement();
          break;
        }
        std::vector<ir::Cell> row;
        bool negative = false;
        std::size_t k = 0;
        int nesting = 0;  // parentheses of function calls inside a value
        while (true) {
          Token v = lexer.next();
          if (v.kind == Token::End) break;
          if (v.kind == Token::Punct && v.text == "(") {
            ++nesting;
            continue;
          }
          if (v.kind == Token::Punct && v.text == ")") {
            if (nesting == 0) break;
            --nesting;
            continue;
          }
          if (nesting > 0) continue;
          if (v.kind == Token::Punct && v.text == ",") {
            ++k;
            continue;
          }
          if (v.kind == Token::Punct && v.text == "-") {
            negative = true;
            continue;
          }
          if (v.kind == Token::Punct && v.text == "+") continue;
          ir::Cell cell;
          if (v.kind == Token::String) {
            cell = ir::Cell::string(util::sanitize_utf8(v.text));
          } else if (v.kind == Token::Number) {
            if (const auto number = util::parse_double(v.text)) {
              const std::string shown = negative ? "-" + v.text : v.text;
              cell = ir::Cell::numeric(negative ? -*number : *number, util::parse_int(shown) ? shown : format_number(negative ? -*number : *number));
            }
          } else if (v.kind == Token::Blob) {
            cell = ir::Cell::string(util::to_lower(v.text));
          } else if (keyword(v, "TRUE") || keyword(v, "FALSE")) {
            cell = ir::Cell::boolean(keyword(v, "TRUE"));
          } else if (v.kind == Token::Word && !keyword(v, "NULL")) {
            // A function call or expression: keep its first word, skip arguments.
            cell = ir::Cell::string(v.text);
          }
          negative = false;
          const std::size_t column = k < target.size() ? target[k] : k;
          if (column >= row.size()) {
            CVT_TRY(budget.take(column + 1 - row.size()));
            row.resize(column + 1);
          }
          const std::string type = column < table->types.size() ? table->types[column] : std::string();
          row[column] = typed_cell(std::move(cell), type);
        }
        CVT_TRY(budget.take(1));
        table->sheet.rows.push_back(std::move(row));
      }
      continue;
    }
    if (keyword(t, "COPY")) {
      // PostgreSQL: COPY name (columns) FROM stdin; then tab-separated lines up to "\.".
      Token name_token;
      const auto name = read_name(lexer, name_token);
      std::vector<std::string> columns;
      Token next = lexer.next();
      if (next.text == "(") {
        while (true) {
          Token c = lexer.next();
          if (c.kind == Token::End || c.text == ")") break;
          if (c.kind == Token::Word || c.kind == Token::Identifier) columns.push_back(c.text);
        }
        next = lexer.next();
      }
      bool from_stdin = false;
      while (next.kind != Token::End && !(next.kind == Token::Punct && next.text == ";")) {
        if (keyword(next, "STDIN")) from_stdin = true;
        next = lexer.next();
      }
      if (!name || !from_stdin) continue;
      CVT_TRY_ASSIGN(DumpTable * table, table_for(*name));
      if (table->sheet.rows.empty()) {
        if (table->columns.empty()) table->columns = columns;
        std::vector<ir::Cell> header;
        for (const std::string& column : table->columns) header.push_back(ir::Cell::string(column));
        table->sheet.rows.push_back(std::move(header));
        table->sheet.has_header = true;
      }
      std::size_t pos = lexer.position();
      const std::string_view source = lexer.source();
      if (pos < source.size() && source[pos] == '\r') ++pos;
      if (pos < source.size() && source[pos] == '\n') ++pos;
      while (pos < source.size()) {
        std::size_t end = source.find('\n', pos);
        if (end == std::string_view::npos) end = source.size();
        std::string_view line = source.substr(pos, end - pos);
        if (line.ends_with('\r')) line.remove_suffix(1);
        pos = end + 1;
        if (line == "\\.") break;
        std::vector<ir::Cell> row;
        std::size_t column = 0;
        for (const std::string_view field : util::split(line, '\t')) {
          ir::Cell cell;
          if (field != "\\N") {
            std::string value;
            for (std::size_t i = 0; i < field.size(); ++i) {
              if (field[i] == '\\' && i + 1 < field.size()) {
                const char e = field[++i];
                value.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
              } else {
                value.push_back(field[i]);
              }
            }
            const std::string type = column < table->types.size() ? util::to_upper(table->types[column]) : std::string();
            const bool numeric = type.find("INT") != std::string::npos || type.find("NUMERIC") != std::string::npos ||
                                 type.find("DECIMAL") != std::string::npos || type.find("REAL") != std::string::npos ||
                                 type.find("DOUBLE") != std::string::npos || type.find("FLOAT") != std::string::npos;
            if (numeric && parse_number(value)) {
              cell = ir::Cell::numeric(*parse_number(value), value);
            } else if (type.find("BOOL") != std::string::npos && (value == "t" || value == "f")) {
              cell = ir::Cell::boolean(value == "t");
            } else {
              cell = typed_cell(ir::Cell::string(std::move(value)), type);
            }
          }
          row.push_back(std::move(cell));
          ++column;
        }
        CVT_TRY(budget.take(row.size() + 1));
        table->sheet.rows.push_back(std::move(row));
      }
      lexer.seek(std::min(pos, source.size()));
      continue;
    }
    lexer.skip_statement();
  }
  ir::Table table;
  for (DumpTable& t : tables) {
    trim_sheet(t.sheet);
    table.sheets.push_back(std::move(t.sheet));
  }
  if (table.sheets.empty()) return fail(Status::Decode, "the SQL script creates or fills no tables");
  return table;
}

}  // namespace

void register_sql_dump(Registry& registry) {
  FormatInfo info;
  info.id = "sql";
  info.name = "SQL script";
  info.description = "SQL statements; tables are written as CREATE TABLE and INSERT statements.";
  info.category = Category::Code;
  info.extensions = {"sql"};
  info.mime = "application/sql";
  info.popularity = 45;
  registry.add_known_format(std::move(info));  // the code family may own the format's description

  Codec reader;
  reader.format_id = "sql";
  reader.kind = Kind::Table;
  reader.read = read_sql;
  reader.read_cost = 20;
  reader.lossy = true;
  reader.read_options = {source_encoding_option()};
  registry.add_codec(std::move(reader));

  Codec writer;
  writer.format_id = "sql";
  writer.kind = Kind::Table;
  writer.write = write_sql;
  writer.write_options = {
      OptionSpec::choice("sql_dialect", "SQL dialect", "sqlite", {"sqlite", "mysql", "postgres"},
                         "Database the statements are written for (quoting, types, escapes)."),
      OptionSpec::integer("batch_size", "Rows per INSERT", 100, 1, 100000, "How many rows each INSERT statement carries."),
      OptionSpec::boolean("create_table", "CREATE TABLE", true, "Write a CREATE TABLE statement before the rows."),
      OptionSpec::boolean("drop_table", "DROP TABLE first", false, "Write DROP TABLE IF EXISTS before creating each table.")
          .advanced(),
      OptionSpec::choice("column_names", "Column names", "auto", {"auto", "first_row", "generated"},
                         "Take column names from the header row (auto: when the sheet has one) or generate column1, "
                         "column2, ..."),
  };
  registry.add_codec(std::move(writer));
}

}  // namespace cvt::table
