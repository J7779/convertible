// Convertible core - table family: SQLite 3 database files.
//
// Reader: the file format of https://sqlite.org/fileformat2.html, written
// from the specification: the 100-byte header, table and index b-tree pages,
// varints, records with every serial type, overflow chains, UTF-8 and UTF-16
// text encodings, and the sqlite_schema table whose CREATE TABLE statements
// are parsed for column names, declared types, INTEGER PRIMARY KEY rowid
// aliases, WITHOUT ROWID primary-key order and generated columns. Every
// table becomes a sheet whose first row holds the column names. Pages are
// visited at most once (cyclic or shared b-trees are rejected) and payload
// bytes count against the decompression limit. Only the main database file
// is read: changes still in a -wal file are not included.
//
// Writer: one table per sheet with declared column types inferred from the
// cells, rows in rowid order, table b-trees with interior pages when needed,
// overflow pages for large values, no free pages. The result passes
// `PRAGMA integrity_check`.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "util/bytes.hpp"
#include "engine/registry.hpp"
#include "engine/sniffers.hpp"
#include "families/table/common.hpp"
#include "families/table/sql.hpp"
#include "families/table/table.hpp"
#include "util/base64.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

constexpr std::string_view magic{"SQLite format 3\0", 16};

// --------------------------------------------------------------------------- varints and values

struct Varint {
  std::uint64_t value = 0;
  std::size_t length = 0;  // 0 = malformed / truncated
};

Varint read_varint(ByteView data, std::size_t at) {
  Varint out;
  for (std::size_t i = 0; i < 9; ++i) {
    if (at + i >= data.size()) return {};
    const auto b = std::to_integer<std::uint8_t>(data[at + i]);
    if (i == 8) {
      out.value = (out.value << 8) | b;
      out.length = 9;
      return out;
    }
    out.value = (out.value << 7) | (b & 0x7Fu);
    if ((b & 0x80u) == 0) {
      out.length = i + 1;
      return out;
    }
  }
  return {};
}

void write_varint(Bytes& out, std::uint64_t value) {
  if (value > 0x00FFFFFFFFFFFFFFull) {
    std::uint8_t bytes[9];
    bytes[8] = static_cast<std::uint8_t>(value);
    value >>= 8;
    for (int i = 7; i >= 0; --i) {
      bytes[i] = static_cast<std::uint8_t>((value & 0x7F) | 0x80);
      value >>= 7;
    }
    for (const std::uint8_t b : bytes) out.push_back(static_cast<std::byte>(b));
    return;
  }
  std::uint8_t buffer[9];
  int n = 0;
  do {
    buffer[n++] = static_cast<std::uint8_t>(value & 0x7F);
    value >>= 7;
  } while (value != 0);
  for (int i = n - 1; i >= 0; --i) out.push_back(static_cast<std::byte>(buffer[i] | (i > 0 ? 0x80 : 0x00)));
}

std::size_t varint_size(std::uint64_t value) {
  Bytes scratch;
  write_varint(scratch, value);
  return scratch.size();
}

enum class ValueKind { Null, Integer, Real, Text, Blob };

struct Value {
  ValueKind kind = ValueKind::Null;
  std::int64_t integer = 0;
  double real = 0.0;
  std::string text;  // text (UTF-8) or blob bytes
};

// --------------------------------------------------------------------------- SQL schema parsing

struct SqlToken {
  enum Kind { Word, Quoted, String, Number, Punct } kind = Word;
  std::string text;
};

std::vector<SqlToken> tokenize_sql(std::string_view sql) {
  std::vector<SqlToken> tokens;
  std::size_t i = 0;
  while (i < sql.size() && tokens.size() < 100'000) {
    const char c = sql[i];
    if (util::is_ascii_space(c)) {
      ++i;
    } else if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      while (i < sql.size() && sql[i] != '\n') ++i;
    } else if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      const std::size_t end = sql.find("*/", i + 2);
      i = end == std::string_view::npos ? sql.size() : end + 2;
    } else if (c == '"' || c == '`' || c == '[' || c == '\'') {
      const char close = c == '[' ? ']' : c;
      std::string text;
      ++i;
      while (i < sql.size()) {
        if (sql[i] == close) {
          if (close != ']' && i + 1 < sql.size() && sql[i + 1] == close) {
            text.push_back(close);
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        text.push_back(sql[i++]);
      }
      tokens.push_back({c == '\'' ? SqlToken::String : SqlToken::Quoted, std::move(text)});
    } else if (util::is_ascii_alpha(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
      const std::size_t start = i;
      while (i < sql.size() && (util::is_ascii_alpha(sql[i]) || util::is_ascii_digit(sql[i]) || sql[i] == '_' || sql[i] == '$' ||
                                static_cast<unsigned char>(sql[i]) >= 0x80)) {
        ++i;
      }
      tokens.push_back({SqlToken::Word, std::string(sql.substr(start, i - start))});
    } else if (util::is_ascii_digit(c) || (c == '.' && i + 1 < sql.size() && util::is_ascii_digit(sql[i + 1]))) {
      const std::size_t start = i;
      while (i < sql.size() && (util::is_ascii_digit(sql[i]) || sql[i] == '.' || sql[i] == 'e' || sql[i] == 'E' ||
                                ((sql[i] == '+' || sql[i] == '-') && (sql[i - 1] == 'e' || sql[i - 1] == 'E')))) {
        ++i;
      }
      tokens.push_back({SqlToken::Number, std::string(sql.substr(start, i - start))});
    } else {
      tokens.push_back({SqlToken::Punct, std::string(1, c)});
      ++i;
    }
  }
  return tokens;
}

bool is_keyword(const SqlToken& token, std::string_view word) {
  return token.kind == SqlToken::Word && util::iequals(token.text, word);
}

struct ColumnDef {
  std::string name;
  std::string type;          // declared type, upper case words joined by spaces
  bool primary_key = false;
  bool virtual_generated = false;  // not stored in records
};

struct TableDef {
  std::vector<ColumnDef> columns;
  std::vector<std::size_t> primary_key;  // column indices in key order
  bool without_rowid = false;
  std::optional<std::size_t> rowid_alias;
};

std::optional<TableDef> parse_create_table(std::string_view sql) {
  const std::vector<SqlToken> tokens = tokenize_sql(sql);
  std::size_t i = 0;
  // Find the opening parenthesis of the column list.
  while (i < tokens.size() && !(tokens[i].kind == SqlToken::Punct && tokens[i].text == "(")) {
    if (is_keyword(tokens[i], "AS")) return std::nullopt;  // CREATE TABLE ... AS SELECT
    ++i;
  }
  if (i >= tokens.size()) return std::nullopt;
  ++i;
  // Split the definitions at top-level commas.
  std::vector<std::vector<SqlToken>> parts(1);
  int depth = 0;
  std::size_t end = i;
  for (; end < tokens.size(); ++end) {
    const SqlToken& t = tokens[end];
    if (t.kind == SqlToken::Punct && t.text == "(") ++depth;
    if (t.kind == SqlToken::Punct && t.text == ")") {
      if (depth == 0) break;
      --depth;
    }
    if (depth == 0 && t.kind == SqlToken::Punct && t.text == ",") {
      parts.emplace_back();
      continue;
    }
    parts.back().push_back(t);
  }
  TableDef table;
  for (std::size_t k = end + 1; k + 1 < tokens.size(); ++k) {
    if (is_keyword(tokens[k], "WITHOUT") && is_keyword(tokens[k + 1], "ROWID")) table.without_rowid = true;
  }
  static constexpr std::string_view constraint_words[] = {"CONSTRAINT", "PRIMARY", "NOT", "NULL", "UNIQUE", "CHECK",
                                                          "DEFAULT", "COLLATE", "REFERENCES", "GENERATED", "AS"};
  std::vector<std::string> table_pk;
  for (const auto& part : parts) {
    if (part.empty()) continue;
    const SqlToken& first = part[0];
    if (is_keyword(first, "PRIMARY") || is_keyword(first, "CONSTRAINT") || is_keyword(first, "UNIQUE") ||
        is_keyword(first, "CHECK") || is_keyword(first, "FOREIGN")) {
      // Table constraint: remember PRIMARY KEY (a, b).
      for (std::size_t k = 0; k + 1 < part.size(); ++k) {
        if (is_keyword(part[k], "PRIMARY") && is_keyword(part[k + 1], "KEY")) {
          std::size_t p = k + 2;
          if (p < part.size() && part[p].text == "(") ++p;
          int d = 0;
          for (; p < part.size(); ++p) {
            if (part[p].text == "(" && part[p].kind == SqlToken::Punct) ++d;
            if (part[p].text == ")" && part[p].kind == SqlToken::Punct) {
              if (d == 0) break;
              --d;
            }
            if (d == 0 && (part[p].kind == SqlToken::Word || part[p].kind == SqlToken::Quoted) &&
                (p == k + 2 || part[p - 1].text == "(" || part[p - 1].text == ",")) {
              table_pk.push_back(part[p].text);
            }
          }
        }
      }
      continue;
    }
    ColumnDef column;
    column.name = first.text;
    std::size_t k = 1;
    for (; k < part.size(); ++k) {
      const SqlToken& t = part[k];
      bool constraint = false;
      for (const std::string_view word : constraint_words) constraint = constraint || is_keyword(t, word);
      if (constraint) break;
      if (t.kind == SqlToken::Punct && t.text == "(") {
        // Type arguments such as VARCHAR(10).
        int d = 0;
        for (; k < part.size(); ++k) {
          if (part[k].text == "(") ++d;
          if (part[k].text == ")" && --d == 0) break;
        }
        continue;
      }
      if (!column.type.empty()) column.type += " ";
      column.type += util::to_upper(t.text);
    }
    bool generated = false;
    bool stored = false;
    for (; k < part.size(); ++k) {
      if (k + 1 < part.size() && is_keyword(part[k], "PRIMARY") && is_keyword(part[k + 1], "KEY")) column.primary_key = true;
      if (is_keyword(part[k], "AS")) generated = true;
      if (is_keyword(part[k], "STORED")) stored = true;
    }
    column.virtual_generated = generated && !stored;
    table.columns.push_back(std::move(column));
  }
  if (table.columns.empty()) return std::nullopt;
  for (std::size_t c = 0; c < table.columns.size(); ++c) {
    if (table.columns[c].primary_key) table.primary_key.push_back(c);
  }
  for (const std::string& name : table_pk) {
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
      if (util::iequals(table.columns[c].name, name)) {
        table.columns[c].primary_key = true;
        if (std::ranges::find(table.primary_key, c) == table.primary_key.end()) table.primary_key.push_back(c);
      }
    }
  }
  if (!table.without_rowid && table.primary_key.size() == 1 && table.columns[table.primary_key[0]].type == "INTEGER") {
    table.rowid_alias = table.primary_key[0];
  }
  return table;
}

// --------------------------------------------------------------------------- reading

struct Database {
  ByteView data;
  std::uint32_t page_size = 0;
  std::uint32_t usable = 0;
  std::uint32_t page_count = 0;
  std::uint32_t encoding = 1;
  Limits limits;
  std::uint64_t payload_total = 0;

  [[nodiscard]] ByteView page(std::uint32_t number) const {
    const std::uint64_t offset = std::uint64_t{number - 1} * page_size;
    return data.subspan(static_cast<std::size_t>(offset), page_size);
  }
};

std::string decode_text(const Database& db, ByteView bytes) {
  if (db.encoding == 1) return util::sanitize_utf8(as_chars(bytes));
  return util::decode_text(bytes, db.encoding == 2 ? util::TextEncoding::Utf16LE : util::TextEncoding::Utf16BE).text;
}

Result<std::vector<Value>> decode_record(const Database& db, ByteView payload) {
  const Varint header_size = read_varint(payload, 0);
  if (header_size.length == 0 || header_size.value > payload.size() || header_size.value < header_size.length) {
    return fail(Status::Decode, "damaged SQLite record header");
  }
  std::vector<std::uint64_t> types;
  std::size_t at = header_size.length;
  while (at < header_size.value) {
    const Varint type = read_varint(payload, at);
    if (type.length == 0) return fail(Status::Decode, "damaged SQLite record header");
    types.push_back(type.value);
    at += type.length;
  }
  std::vector<Value> values;
  std::size_t body = static_cast<std::size_t>(header_size.value);
  for (const std::uint64_t type : types) {
    Value value;
    std::uint64_t size = 0;
    switch (type) {
      case 0: break;
      case 1: size = 1; break;
      case 2: size = 2; break;
      case 3: size = 3; break;
      case 4: size = 4; break;
      case 5: size = 6; break;
      case 6:
      case 7: size = 8; break;
      case 8:
      case 9:
        value.kind = ValueKind::Integer;
        value.integer = type == 9 ? 1 : 0;
        break;
      case 10:
      case 11: return fail(Status::Decode, "reserved SQLite serial type");
      default: size = (type - (type % 2 == 0 ? 12 : 13)) / 2; break;
    }
    if (size > payload.size() - body) return fail(Status::Decode, "SQLite record is truncated");
    const ByteView field = payload.subspan(body, static_cast<std::size_t>(size));
    body += static_cast<std::size_t>(size);
    if (type >= 1 && type <= 6) {
      std::uint64_t raw = 0;
      for (const std::byte b : field) raw = (raw << 8) | std::to_integer<std::uint8_t>(b);
      const unsigned bits = static_cast<unsigned>(size * 8);
      if (bits < 64 && (raw >> (bits - 1)) != 0) raw |= ~0ull << bits;  // sign extension
      value.kind = ValueKind::Integer;
      value.integer = static_cast<std::int64_t>(raw);
    } else if (type == 7) {
      std::uint64_t raw = 0;
      for (const std::byte b : field) raw = (raw << 8) | std::to_integer<std::uint8_t>(b);
      value.kind = ValueKind::Real;
      value.real = std::bit_cast<double>(raw);
    } else if (type >= 12) {
      value.kind = type % 2 == 0 ? ValueKind::Blob : ValueKind::Text;
      value.text = value.kind == ValueKind::Text ? decode_text(db, field) : to_string(field);
    }
    values.push_back(std::move(value));
  }
  return values;
}

// Collects payloads of a b-tree in key order.
class TreeWalker {
 public:
  TreeWalker(Database& db, std::vector<bool>& visited) : db_(db), visited_(visited) {}

  /// Calls `on_row(rowid, payload)` for each row of a table b-tree, or each
  /// entry of an index b-tree (rowid 0).
  Result<void> walk(std::uint32_t root, const std::function<Result<void>(std::int64_t, ByteView)>& on_row,
                    const Context& ctx) {
    struct Frame {
      std::uint32_t page;
      std::size_t next_cell;  // for interior pages
      int depth;
    };
    std::vector<Frame> stack{{root, 0, 0}};
    CVT_TRY(enter(root));
    std::size_t pages = 0;
    while (!stack.empty()) {
      Frame& frame = stack.back();
      const ByteView page = db_.page(frame.page);
      const std::size_t header = frame.page == 1 ? 100 : 0;
      if (header + 8 > page.size()) return fail(Status::Decode, "damaged SQLite page {}", frame.page);
      util::ByteReader r(page.subspan(header));
      const std::uint8_t type = r.u8();
      (void)r.u16be();
      const std::uint16_t cells = r.u16be();
      if (type != 2 && type != 5 && type != 10 && type != 13) return fail(Status::Decode, "SQLite page {} is not a b-tree page", frame.page);
      const bool leaf = type == 10 || type == 13;
      const bool index = type == 2 || type == 10;
      const std::size_t pointers = header + (leaf ? 8 : 12);
      if (pointers + std::size_t{cells} * 2 > page.size()) return fail(Status::Decode, "damaged SQLite page {}", frame.page);
      const auto cell_offset = [&](std::size_t k) {
        return static_cast<std::size_t>(std::to_integer<std::uint8_t>(page[pointers + k * 2]) << 8 |
                                        std::to_integer<std::uint8_t>(page[pointers + k * 2 + 1]));
      };
      if (leaf) {
        if ((++pages & 255) == 0) CVT_TRY(ctx.checkpoint(0.5));
        for (std::size_t k = 0; k < cells; ++k) {
          std::size_t at = cell_offset(k);
          const Varint size = read_varint(page, at);
          if (size.length == 0) return fail(Status::Decode, "damaged SQLite cell");
          at += size.length;
          std::int64_t rowid = 0;
          if (!index) {
            const Varint key = read_varint(page, at);
            if (key.length == 0) return fail(Status::Decode, "damaged SQLite cell");
            rowid = static_cast<std::int64_t>(key.value);
            at += key.length;
          }
          CVT_TRY_ASSIGN(const Bytes payload, read_payload(page, at, size.value, index));
          CVT_TRY(on_row(rowid, payload));
        }
        stack.pop_back();
        continue;
      }
      // Interior page: visit child k, then (for index trees) cell k's own entry.
      if (frame.next_cell > cells) {
        stack.pop_back();
        continue;
      }
      std::uint32_t child = 0;
      if (frame.next_cell == cells) {
        r.seek(8);
        child = r.u32be();
      } else {
        const std::size_t at = cell_offset(frame.next_cell);
        if (at + 4 > page.size()) return fail(Status::Decode, "damaged SQLite cell");
        util::ByteReader c(page.subspan(at));
        child = c.u32be();
      }
      const std::size_t this_cell = frame.next_cell;
      ++frame.next_cell;
      const int depth = frame.depth;
      if (index && this_cell > 0 && this_cell <= cells) {
        // The entry of the previous interior cell comes after its left subtree.
        std::size_t at = cell_offset(this_cell - 1) + 4;
        const Varint size = read_varint(page, at);
        if (size.length == 0) return fail(Status::Decode, "damaged SQLite cell");
        at += size.length;
        CVT_TRY_ASSIGN(const Bytes payload, read_payload(page, at, size.value, true));
        CVT_TRY(on_row(0, payload));
      }
      if (depth >= 40) return fail(Status::Decode, "SQLite b-tree is too deep");
      CVT_TRY(enter(child));
      stack.push_back({child, 0, depth + 1});
    }
    return {};
  }

 private:
  Result<void> enter(std::uint32_t page) {
    if (page == 0 || page > db_.page_count) return fail(Status::Decode, "SQLite b-tree points to page {} outside the file", page);
    if (visited_[page]) return fail(Status::Decode, "SQLite b-tree page {} is used twice (corrupt database)", page);
    visited_[page] = true;
    return {};
  }

  Result<Bytes> read_payload(ByteView page, std::size_t at, std::uint64_t size, bool index) {
    const std::uint64_t u = db_.usable;
    const std::uint64_t x = index ? ((u - 12) * 64 / 255) - 23 : u - 35;
    const std::uint64_t m = ((u - 12) * 32 / 255) - 23;
    std::uint64_t local = size;
    if (size > x) {
      const std::uint64_t k = m + ((size - m) % (u - 4));
      local = k <= x ? k : m;
    }
    db_.payload_total += size;
    CVT_TRY(db_.limits.check_decompressed(db_.payload_total));
    if (at + local > page.size()) return fail(Status::Decode, "SQLite cell runs past the end of its page");
    Bytes out(page.begin() + static_cast<std::ptrdiff_t>(at), page.begin() + static_cast<std::ptrdiff_t>(at + local));
    if (local == size) return out;
    if (at + local + 4 > page.size()) return fail(Status::Decode, "SQLite cell runs past the end of its page");
    util::ByteReader pointer(page.subspan(static_cast<std::size_t>(at + local), 4));
    std::uint32_t next = pointer.u32be();
    std::set<std::uint32_t> seen;
    while (out.size() < size) {
      if (next == 0 || next > db_.page_count || !seen.insert(next).second) {
        return fail(Status::Decode, "damaged SQLite overflow chain");
      }
      const ByteView overflow = db_.page(next);
      util::ByteReader o(overflow);
      next = o.u32be();
      const std::uint64_t take = std::min<std::uint64_t>(size - out.size(), u - 4);
      const ByteView chunk = o.bytes(static_cast<std::size_t>(take));
      if (!o.ok()) return fail(Status::Decode, "damaged SQLite overflow page");
      out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
  }

  Database& db_;
  std::vector<bool>& visited_;
};

ir::Cell value_cell(const Value& value, const ColumnDef* column, std::string_view blobs) {
  const std::string type = column != nullptr ? column->type : std::string();
  switch (value.kind) {
    case ValueKind::Null: return ir::Cell{};
    case ValueKind::Integer:
      if (type.find("BOOL") != std::string::npos && (value.integer == 0 || value.integer == 1)) {
        return ir::Cell::boolean(value.integer == 1);
      }
      return integer_cell(value.integer);
    case ValueKind::Real: return number_cell(value.real);
    case ValueKind::Text:
      if (type.find("DATE") != std::string::npos || type.find("TIME") != std::string::npos) {
        if (const auto date = parse_iso_datetime(value.text)) return ir::Cell::datetime(date->unix_seconds, value.text);
      }
      return ir::Cell::string(value.text);
    case ValueKind::Blob:
      if (blobs == "base64") return ir::Cell::string(util::base64_encode(as_bytes(value.text)));
      return ir::Cell::string(util::hex_encode(as_bytes(value.text)));
  }
  return ir::Cell{};
}

Result<AnyIR> read_sqlite(ReadContext& ctx) {
  ByteView input = ctx.input;
  if (input.size() < 100 || as_chars(input).substr(0, 16) != magic) return fail(Status::Decode, "not an SQLite 3 database");
  util::ByteReader h(input);
  h.seek(16);
  std::uint32_t page_size = h.u16be();
  if (page_size == 1) page_size = 65536;
  const std::uint8_t write_version = h.u8();
  const std::uint8_t read_version = h.u8();
  const std::uint8_t reserved = h.u8();
  if (page_size < 512 || page_size > 65536 || (page_size & (page_size - 1)) != 0) {
    return fail(Status::Decode, "invalid SQLite page size {}", page_size);
  }
  if (read_version > 2) return fail(Status::Decode, "this SQLite file format version is not supported");
  h.seek(56);
  const std::uint32_t encoding = h.u32be();
  Database db;
  db.data = input;
  db.page_size = page_size;
  db.usable = page_size - reserved;
  if (db.usable < 480) return fail(Status::Decode, "invalid SQLite reserved space");
  db.page_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(input.size() / page_size, 0x7FFFFFFF));
  db.encoding = encoding == 2 || encoding == 3 ? encoding : 1;
  db.limits = ctx.limits;
  if (db.page_count == 0) return fail(Status::Decode, "the SQLite database is truncated");
  if (write_version == 2 || read_version == 2) {
    ctx.warn("The database uses write-ahead logging; changes still in its -wal file are not included.");
  }
  const std::string blobs = ctx.options.get_enum("blob_format", "hex", {"hex", "base64"});

  std::vector<bool> visited(std::size_t{db.page_count} + 1, false);
  TreeWalker walker(db, visited);
  struct SchemaRow {
    std::string type;
    std::string name;
    std::int64_t root = 0;
    std::string sql;
  };
  std::vector<SchemaRow> schema;
  CVT_TRY(walker.walk(1, [&](std::int64_t, ByteView payload) -> Result<void> {
    CVT_TRY_ASSIGN(const auto values, decode_record(db, payload));
    if (values.size() < 5) return {};
    SchemaRow row;
    row.type = values[0].text;
    row.name = values[1].text;
    row.root = values[3].kind == ValueKind::Integer ? values[3].integer : 0;
    row.sql = values[4].text;
    CVT_TRY(ctx.limits.check_archive_entries(schema.size() + 1));
    schema.push_back(std::move(row));
    return {};
  }, ctx));

  ir::Table table;
  CellBudget budget(ctx.limits);
  std::size_t skipped_virtual = 0;
  for (const SchemaRow& entry : schema) {
    if (entry.type != "table" || util::istarts_with(entry.name, "sqlite_")) continue;
    if (entry.root <= 0) {
      ++skipped_virtual;
      continue;
    }
    if (entry.root > db.page_count) return fail(Status::Decode, "table '{}' has no valid root page", entry.name);
    const auto def = parse_create_table(entry.sql);
    if (!def) {
      ctx.warn(std::format("Table \"{}\" has a definition that could not be understood and was skipped.", entry.name));
      continue;
    }
    ir::Sheet sheet;
    sheet.name = entry.name;
    sheet.has_header = true;
    std::vector<ir::Cell> header;
    for (const ColumnDef& column : def->columns) header.push_back(ir::Cell::string(column.name));
    sheet.rows.push_back(std::move(header));
    // Order of values in a record.
    std::vector<std::size_t> stored;
    if (def->without_rowid) {
      stored = def->primary_key;
      for (std::size_t c = 0; c < def->columns.size(); ++c) {
        if (std::ranges::find(def->primary_key, c) == def->primary_key.end() && !def->columns[c].virtual_generated) stored.push_back(c);
      }
    } else {
      for (std::size_t c = 0; c < def->columns.size(); ++c) {
        if (!def->columns[c].virtual_generated) stored.push_back(c);
      }
    }
    CVT_TRY(walker.walk(static_cast<std::uint32_t>(entry.root), [&](std::int64_t rowid, ByteView payload) -> Result<void> {
      CVT_TRY_ASSIGN(const auto values, decode_record(db, payload));
      CVT_TRY(budget.take(def->columns.size() + 1));
      std::vector<ir::Cell> row(def->columns.size());
      for (std::size_t k = 0; k < values.size() && k < stored.size(); ++k) {
        const std::size_t c = stored[k];
        row[c] = value_cell(values[k], &def->columns[c], blobs);
      }
      if (def->rowid_alias && row[*def->rowid_alias].empty()) row[*def->rowid_alias] = integer_cell(rowid);
      sheet.rows.push_back(std::move(row));
      return {};
    }, ctx));
    trim_sheet(sheet);
    table.sheets.push_back(std::move(sheet));
  }
  if (skipped_virtual > 0) ctx.warn(std::format("{} virtual table(s) have no stored rows and were skipped.", skipped_virtual));
  return table;
}

// --------------------------------------------------------------------------- writing

constexpr std::uint32_t write_page_size = 4096;

struct PageImage {
  Bytes bytes = Bytes(write_page_size, std::byte{0});
};

class PageAllocator {
 public:
  std::uint32_t allocate() {
    pages_.emplace_back();
    return static_cast<std::uint32_t>(pages_.size());  // page numbers start at 1
  }
  Bytes& page(std::uint32_t number) { return pages_[number - 1].bytes; }
  [[nodiscard]] std::size_t count() const noexcept { return pages_.size(); }
  [[nodiscard]] std::vector<PageImage>& pages() noexcept { return pages_; }

 private:
  std::vector<PageImage> pages_;
};

void put_u16(Bytes& page, std::size_t at, std::uint32_t v) {
  page[at] = static_cast<std::byte>(v >> 8);
  page[at + 1] = static_cast<std::byte>(v);
}
void put_u32(Bytes& page, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) page[at + static_cast<std::size_t>(i)] = static_cast<std::byte>(v >> (24 - 8 * i));
}

// A table-leaf cell, with overflow pages already written.
Bytes leaf_cell(std::int64_t rowid, const Bytes& payload, PageAllocator& pages) {
  const std::uint64_t u = write_page_size;
  const std::uint64_t x = u - 35;
  const std::uint64_t m = ((u - 12) * 32 / 255) - 23;
  const std::uint64_t p = payload.size();
  std::uint64_t local = p;
  if (p > x) {
    const std::uint64_t k = m + ((p - m) % (u - 4));
    local = k <= x ? k : m;
  }
  Bytes cell;
  write_varint(cell, p);
  write_varint(cell, static_cast<std::uint64_t>(rowid));
  cell.insert(cell.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(local));
  if (local < p) {
    // Overflow chain.
    std::uint64_t offset = local;
    std::uint32_t first = 0;
    std::uint32_t previous = 0;
    while (offset < p) {
      const std::uint32_t number = pages.allocate();
      if (first == 0) first = number;
      if (previous != 0) put_u32(pages.page(previous), 0, number);
      Bytes& page = pages.page(number);
      const std::uint64_t take = std::min<std::uint64_t>(p - offset, u - 4);
      ::cvt::util::copy_bytes(page.data() + 4, payload.data() + offset, static_cast<std::size_t>(take));
      offset += take;
      previous = number;
    }
    for (int i = 0; i < 4; ++i) cell.push_back(static_cast<std::byte>(first >> (24 - 8 * i)));
  }
  return cell;
}

// Lay out cells on a b-tree page (`header_offset` is 100 for page 1).
void write_btree_page(Bytes& page, std::size_t header_offset, std::uint8_t type, const std::vector<Bytes>& cells,
                      std::uint32_t right_child) {
  const bool leaf = type == 13;
  const std::size_t header_size = leaf ? 8 : 12;
  std::size_t content = write_page_size;
  const std::size_t pointers = header_offset + header_size;
  for (std::size_t k = 0; k < cells.size(); ++k) {
    content -= cells[k].size();
    ::cvt::util::copy_bytes(page.data() + content, cells[k].data(), cells[k].size());
    put_u16(page, pointers + k * 2, static_cast<std::uint32_t>(content));
  }
  page[header_offset] = static_cast<std::byte>(type);
  put_u16(page, header_offset + 1, 0);
  put_u16(page, header_offset + 3, static_cast<std::uint32_t>(cells.size()));
  put_u16(page, header_offset + 5, content == 65536 ? 0 : static_cast<std::uint32_t>(content));
  page[header_offset + 7] = std::byte{0};
  if (!leaf) put_u32(page, header_offset + 8, right_child);
}

struct Node {
  std::uint32_t page;
  std::int64_t max_rowid;
};

// Build a table b-tree from rows (payloads, rowids 1..n). When `root_page`
// is given (page 1 for the schema), the root is placed there.
std::uint32_t build_table_tree(const std::vector<Bytes>& payloads, PageAllocator& pages, std::optional<std::uint32_t> root_page) {
  const std::size_t root_offset = root_page && *root_page == 1 ? 100 : 0;
  // Pack leaves.
  std::vector<std::vector<Bytes>> leaves;
  std::vector<std::int64_t> leaf_max;
  {
    std::vector<Bytes> current;
    std::size_t used = 8;
    for (std::size_t i = 0; i < payloads.size(); ++i) {
      Bytes cell = leaf_cell(static_cast<std::int64_t>(i + 1), payloads[i], pages);
      if (!current.empty() && used + cell.size() + 2 > write_page_size) {
        leaves.push_back(std::move(current));
        leaf_max.push_back(static_cast<std::int64_t>(i));
        current.clear();
        used = 8;
      }
      used += cell.size() + 2;
      current.push_back(std::move(cell));
    }
    leaves.push_back(std::move(current));
    leaf_max.push_back(static_cast<std::int64_t>(payloads.size()));
  }
  // A single leaf is the root itself (if it fits on the designated page).
  const auto fits = [&](const std::vector<Bytes>& cells, std::size_t offset, std::size_t header) {
    std::size_t total = offset + header;
    for (const Bytes& c : cells) total += c.size() + 2;
    return total <= write_page_size;
  };
  if (leaves.size() == 1 && fits(leaves[0], root_offset, 8)) {
    const std::uint32_t number = root_page ? *root_page : pages.allocate();
    write_btree_page(pages.page(number), root_offset, 13, leaves[0], 0);
    return number;
  }
  std::vector<Node> level;
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const std::uint32_t number = pages.allocate();
    write_btree_page(pages.page(number), 0, 13, leaves[i], 0);
    level.push_back({number, leaf_max[i]});
  }
  while (true) {
    // Group children into interior pages.
    std::vector<std::vector<Node>> groups;
    std::vector<Node> current;
    std::size_t used = 12;
    for (const Node& node : level) {
      const std::size_t cell = 4 + varint_size(static_cast<std::uint64_t>(node.max_rowid)) + 2;
      if (!current.empty() && used + cell > write_page_size) {
        groups.push_back(std::move(current));
        current.clear();
        used = 12;
      }
      used += cell;
      current.push_back(node);
    }
    groups.push_back(std::move(current));
    const auto cells_of = [](const std::vector<Node>& group) {
      std::vector<Bytes> cells;
      for (std::size_t k = 0; k + 1 < group.size(); ++k) {
        Bytes cell;
        for (int i = 0; i < 4; ++i) cell.push_back(static_cast<std::byte>(group[k].page >> (24 - 8 * i)));
        write_varint(cell, static_cast<std::uint64_t>(group[k].max_rowid));
        cells.push_back(std::move(cell));
      }
      return cells;
    };
    if (groups.size() == 1) {
      const std::vector<Bytes> cells = cells_of(groups[0]);
      if (fits(cells, root_offset, 12)) {
        const std::uint32_t number = root_page ? *root_page : pages.allocate();
        write_btree_page(pages.page(number), root_offset, 5, cells, groups[0].back().page);
        return number;
      }
    }
    std::vector<Node> next;
    for (const auto& group : groups) {
      const std::uint32_t number = pages.allocate();
      write_btree_page(pages.page(number), 0, 5, cells_of(group), group.back().page);
      next.push_back({number, group.back().max_rowid});
    }
    level = std::move(next);
  }
}

Bytes make_record(const std::vector<Value>& values) {
  Bytes header;
  Bytes body;
  for (const Value& value : values) {
    switch (value.kind) {
      case ValueKind::Null: write_varint(header, 0); break;
      case ValueKind::Integer: {
        const std::int64_t v = value.integer;
        if (v == 0 || v == 1) {
          write_varint(header, v == 0 ? 8 : 9);
          break;
        }
        int bytes = 8;
        std::uint64_t type = 6;
        if (v >= -128 && v <= 127) {
          bytes = 1;
          type = 1;
        } else if (v >= -32768 && v <= 32767) {
          bytes = 2;
          type = 2;
        } else if (v >= -8388608 && v <= 8388607) {
          bytes = 3;
          type = 3;
        } else if (v >= -2147483648LL && v <= 2147483647LL) {
          bytes = 4;
          type = 4;
        } else if (v >= -140737488355328LL && v <= 140737488355327LL) {
          bytes = 6;
          type = 5;
        }
        write_varint(header, type);
        const auto raw = static_cast<std::uint64_t>(v);
        for (int i = bytes - 1; i >= 0; --i) body.push_back(static_cast<std::byte>(raw >> (8 * i)));
        break;
      }
      case ValueKind::Real: {
        write_varint(header, 7);
        const auto raw = std::bit_cast<std::uint64_t>(value.real);
        for (int i = 7; i >= 0; --i) body.push_back(static_cast<std::byte>(raw >> (8 * i)));
        break;
      }
      case ValueKind::Text:
      case ValueKind::Blob:
        write_varint(header, value.text.size() * 2 + (value.kind == ValueKind::Text ? 13 : 12));
        append(body, value.text);
        break;
    }
  }
  // The header size includes its own varint.
  std::size_t total = header.size() + 1;
  for (std::size_t k = 1; k <= 9; ++k) {
    total = header.size() + k;
    if (varint_size(total) == k) break;
  }
  Bytes record;
  write_varint(record, total);
  record.insert(record.end(), header.begin(), header.end());
  record.insert(record.end(), body.begin(), body.end());
  return record;
}

std::string quote_identifier(std::string_view name) { return "\"" + util::replace_all(name, "\"", "\"\"") + "\""; }

bool integral(double v) { return std::isfinite(v) && v == std::trunc(v) && std::fabs(v) < 9.2e18; }

}  // namespace

SqlColumns sql_columns(const ir::Sheet& sheet, bool use_header) {
  SqlColumns out;
  const std::size_t columns = sheet.column_count();
  out.first_data_row = use_header && !sheet.rows.empty() ? 1 : 0;
  std::set<std::string> used;
  for (std::size_t c = 0; c < columns; ++c) {
    std::string name = use_header ? std::string(util::trim(sheet.at(0, c).text)) : std::string();
    if (name.empty()) name = std::format("column{}", c + 1);
    std::string candidate = name;
    for (int n = 2; used.contains(util::to_lower(candidate)); ++n) candidate = std::format("{}_{}", name, n);
    used.insert(util::to_lower(candidate));
    out.names.push_back(std::move(candidate));
    // Declared type from the content.
    bool any = false;
    bool ints = true;
    bool numbers = true;
    bool bools = true;
    bool dates = true;
    bool texts = true;
    for (std::size_t r = out.first_data_row; r < sheet.rows.size(); ++r) {
      const ir::Cell& cell = sheet.at(r, c);
      if (cell.empty()) continue;
      any = true;
      ints = ints && cell.type == ir::CellType::Number && integral(cell.number);
      numbers = numbers && cell.type == ir::CellType::Number;
      bools = bools && cell.type == ir::CellType::Bool;
      dates = dates && cell.type == ir::CellType::DateTime;
      texts = texts && cell.type == ir::CellType::String;
    }
    out.types.push_back(!any ? "TEXT" : ints ? "INTEGER" : numbers ? "REAL" : bools ? "BOOLEAN" : dates ? "DATETIME" : texts ? "TEXT" : "");
  }
  return out;
}

namespace {

Value cell_value(const ir::Cell& cell) {
  Value v;
  switch (cell.type) {
    case ir::CellType::Empty: break;
    case ir::CellType::String:
      v.kind = ValueKind::Text;
      v.text = util::sanitize_utf8(cell.text);
      break;
    case ir::CellType::Number:
      if (integral(cell.number)) {
        v.kind = ValueKind::Integer;
        v.integer = util::parse_int(cell.text).value_or(static_cast<std::int64_t>(cell.number));
        if (static_cast<double>(v.integer) != cell.number && std::fabs(cell.number) < 9.0e15) v.integer = static_cast<std::int64_t>(cell.number);
      } else if (std::isfinite(cell.number)) {
        v.kind = ValueKind::Real;
        v.real = cell.number;
      }
      break;
    case ir::CellType::Bool:
      v.kind = ValueKind::Integer;
      v.integer = cell.number != 0 ? 1 : 0;
      break;
    case ir::CellType::DateTime: {
      v.kind = ValueKind::Text;
      v.text = format_datetime(cell.number);
      std::ranges::replace(v.text, 'T', ' ');
      break;
    }
  }
  return v;
}

Result<Bytes> write_sqlite(const AnyIR& value, WriteContext& ctx) {
  const ir::Table& table = value.get<ir::Table>();
  const std::string header_mode = ctx.options.get_enum("column_names", "auto", {"auto", "first_row", "generated"});
  PageAllocator pages;
  const std::uint32_t schema_page = pages.allocate();  // page 1
  std::vector<Bytes> schema_rows;
  std::set<std::string> used_names;
  for (std::size_t s = 0; s < table.sheets.size(); ++s) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(s) / static_cast<double>(std::max<std::size_t>(1, table.sheets.size()))));
    const ir::Sheet& sheet = table.sheets[s];
    std::string name = util::sanitize_utf8(sheet.name);
    if (util::trim(name).empty()) name = std::format("Sheet{}", s + 1);
    if (util::istarts_with(name, "sqlite_")) name = "t_" + name;
    std::string candidate = name;
    for (int n = 2; used_names.contains(util::to_lower(candidate)); ++n) candidate = std::format("{}_{}", name, n);
    used_names.insert(util::to_lower(candidate));
    name = candidate;
    const bool use_header = header_mode == "first_row" || (header_mode == "auto" && sheet.has_header);
    const SqlColumns columns = sql_columns(sheet, use_header);
    std::string sql = "CREATE TABLE " + quote_identifier(name) + " (";
    for (std::size_t c = 0; c < columns.names.size(); ++c) {
      if (c > 0) sql += ", ";
      sql += quote_identifier(columns.names[c]);
      if (!columns.types[c].empty()) sql += " " + columns.types[c];
    }
    if (columns.names.empty()) sql += "\"column1\" TEXT";
    sql += ")";
    std::vector<Bytes> payloads;
    for (std::size_t r = columns.first_data_row; r < sheet.rows.size(); ++r) {
      std::vector<Value> values;
      for (std::size_t c = 0; c < columns.names.size(); ++c) values.push_back(cell_value(sheet.at(r, c)));
      payloads.push_back(make_record(values));
      CVT_TRY(ctx.limits.check_output(std::uint64_t{pages.count()} * write_page_size));
    }
    const std::uint32_t root = build_table_tree(payloads, pages, std::nullopt);
    std::vector<Value> row(5);
    row[0].kind = ValueKind::Text;
    row[0].text = "table";
    row[1].kind = ValueKind::Text;
    row[1].text = name;
    row[2] = row[1];
    row[3].kind = ValueKind::Integer;
    row[3].integer = root;
    row[4].kind = ValueKind::Text;
    row[4].text = sql;
    schema_rows.push_back(make_record(row));
  }
  build_table_tree(schema_rows, pages, schema_page);
  const auto page_count = static_cast<std::uint32_t>(pages.count());
  Bytes& first = pages.page(1);
  ::cvt::util::copy_bytes(first.data(), magic.data(), magic.size());
  put_u16(first, 16, write_page_size);
  first[18] = std::byte{1};  // legacy (rollback journal) write/read versions
  first[19] = std::byte{1};
  first[20] = std::byte{0};
  first[21] = std::byte{64};
  first[22] = std::byte{32};
  first[23] = std::byte{32};
  put_u32(first, 24, 1);           // file change counter
  put_u32(first, 28, page_count);  // database size in pages
  put_u32(first, 32, 0);           // no freelist
  put_u32(first, 36, 0);
  put_u32(first, 40, 1);           // schema cookie
  put_u32(first, 44, 4);           // schema format
  put_u32(first, 48, 0);           // default page cache size
  put_u32(first, 52, 0);           // no auto-vacuum
  put_u32(first, 56, 1);           // UTF-8
  put_u32(first, 60, 0);           // user version
  put_u32(first, 64, 0);           // incremental vacuum off
  put_u32(first, 68, 0);           // application id
  put_u32(first, 92, 1);           // version-valid-for
  put_u32(first, 96, 3046000);     // SQLITE_VERSION_NUMBER of the format written
  Bytes out;
  out.reserve(std::size_t{page_count} * write_page_size);
  for (PageImage& page : pages.pages()) out.insert(out.end(), page.bytes.begin(), page.bytes.end());
  CVT_TRY(ctx.limits.check_output(out.size()));
  return out;
}

}  // namespace

void register_sqlite(Registry& registry) {
  FormatInfo info;
  info.id = "sqlite";
  info.name = "SQLite database";
  info.description = "SQLite 3 database file; every table becomes a sheet.";
  info.category = Category::Data;
  info.extensions = {"sqlite", "sqlite3", "db", "db3"};
  info.mime = "application/vnd.sqlite3";
  info.popularity = 60;
  // SQLite is a container (GeoPackage, MBTiles ... are SQLite files with an
  // application id): the shared classifier names those, this is the rest.
  info.sniff = [](std::span<const std::byte> data, std::string_view filename) {
    const sniff::Verdict verdict = sniff::classify(sniff::Family::Sqlite, data, filename, "sqlite");
    return verdict.id == "sqlite" ? verdict.confidence : 0;
  };
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "sqlite";
  codec.kind = Kind::Table;
  codec.read = read_sqlite;
  codec.write = write_sqlite;
  codec.read_options = {OptionSpec::choice("blob_format", "Binary values", "hex", {"hex", "base64"},
                                           "How BLOB values are shown in cells.")
                            .advanced()};
  codec.write_options = {
      OptionSpec::choice("column_names", "Column names", "auto", {"auto", "first_row", "generated"},
                         "Take column names from the header row (auto: when the sheet has one) or generate column1, "
                         "column2, ..."),
  };
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::table
