// Convertible core - table family: legacy spreadsheet interchange formats.
//
//   * SYLK (Symbolic Link, .slk): read and write. Cells (C records) with
//     numbers, quoted strings, booleans and errors, formulas (E, kept in
//     R1C1 form translated to A1), number formats (P and F records) so that
//     dates come back as dates.
//   * DIF (Data Interchange Format, .dif): read and write. Header items,
//     numeric and string data items, TRUE/FALSE values.
//   * Lotus 1-2-3 (.wks, .wk1) and Quattro Pro for DOS (.wq1): read. BOF,
//     INTEGER, NUMBER, LABEL, FORMULA (cached result) and STRING records;
//     the special date and time formats turn serial numbers into dates.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/table/codepages.hpp"
#include "families/table/common.hpp"
#include "families/table/spreadsheet.hpp"
#include "families/table/table.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

std::string file_stem(std::string_view filename) {
  if (const std::size_t slash = filename.find_last_of("/\\"); slash != std::string_view::npos) filename.remove_prefix(slash + 1);
  if (const std::size_t dot = filename.rfind('.'); dot != std::string_view::npos && dot > 0) filename = filename.substr(0, dot);
  return std::string(filename);
}

// Lines joined so that a quoted value spanning line breaks stays one record.
std::vector<std::string> logical_lines(std::string_view text) {
  std::vector<std::string> out;
  std::string current;
  bool open = false;
  for (const std::string_view line : util::split_lines(text)) {
    if (open) {
      current += "\n";
      current += line;
    } else {
      current = std::string(line);
    }
    std::size_t quotes = 0;
    for (const char c : line) quotes += c == '"' ? 1 : 0;
    if (quotes % 2 == 1) open = !open;
    if (!open) out.push_back(std::move(current));
  }
  if (open) out.push_back(std::move(current));
  return out;
}

// SYLK escape sequences: ESC " :" is a line break; other ESC sequences
// (character-set switches) are dropped with the character they introduce.
std::string sylk_unescape(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\x1B') {
      if (text.substr(i).starts_with("\x1B :")) {
        out.push_back('\n');
        i += 2;
      } else {
        i += 2;
      }
      continue;
    }
    out.push_back(text[i]);
  }
  return out;
}

// --------------------------------------------------------------------------- SYLK

// Split a SYLK record on ';' (";;" is a literal semicolon).
std::vector<std::string> sylk_fields(std::string_view line) {
  std::vector<std::string> fields;
  std::string current;
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') quoted = !quoted;
    if (c == ';' && i + 1 < line.size() && line[i + 1] == ';') {
      current.push_back(';');  // a doubled semicolon is a literal one, quoted or not
      ++i;
      continue;
    }
    if (c == ';' && !quoted) {
      fields.push_back(std::move(current));
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  fields.push_back(std::move(current));
  return fields;
}

Result<AnyIR> read_sylk(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, decode_source_text(ctx.input, ctx.options.get_string("source_encoding", "auto"), ctx));
  const std::vector<std::string> lines = logical_lines(text);
  if (lines.empty() || !lines[0].starts_with("ID;")) return fail(Status::Decode, "not a SYLK file (no ID record)");
  ir::Sheet sheet;
  sheet.name = file_stem(ctx.filename);
  CellBudget budget(ctx.limits);
  std::vector<std::string> formats;  // P records in order
  std::map<std::pair<std::int64_t, std::int64_t>, std::size_t> cell_formats;
  std::int64_t row = 1;
  std::int64_t column = 1;
  bool date1904 = false;
  struct Pending {
    std::int64_t row;
    std::int64_t column;
    std::string value;
    std::string formula;
  };
  std::vector<Pending> cells;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const std::string_view line = lines[i];
    if (line.empty()) continue;
    if (line == "E") break;
    const std::vector<std::string> fields = sylk_fields(line);
    const std::string& type = fields[0];
    if (type == "P") {
      for (std::size_t k = 1; k < fields.size(); ++k) {
        if (!fields[k].empty() && fields[k][0] == 'P') {
          // SYLK escapes characters of the format code with backslashes.
          if (formats.size() < 10000) formats.push_back(fields[k].substr(1));
        }
      }
      continue;
    }
    if (type == "O") {
      for (std::size_t k = 1; k < fields.size(); ++k) date1904 = date1904 || fields[k] == "D";
      continue;
    }
    if (type != "C" && type != "F") continue;
    std::optional<std::size_t> format;
    std::string value;
    std::string formula;
    bool has_value = false;
    for (std::size_t k = 1; k < fields.size(); ++k) {
      const std::string& f = fields[k];
      if (f.empty()) continue;
      const char key = f[0];
      const std::string_view rest = std::string_view(f).substr(1);
      if (key == 'Y') {
        row = util::parse_int(rest).value_or(row);
      } else if (key == 'X') {
        column = util::parse_int(rest).value_or(column);
      } else if (key == 'K' && type == "C") {
        value = std::string(rest);
        has_value = true;
      } else if (key == 'E' && type == "C") {
        formula = std::string(rest);
      } else if (key == 'P' && type == "F") {
        if (const auto index = util::parse_int(rest); index && *index >= 0) format = static_cast<std::size_t>(*index);
      }
    }
    if (row < 1 || column < 1 || row > 16'777'216 || column > 65'536) return fail(Status::Decode, "SYLK cell position out of range");
    if (type == "F" && format) cell_formats[{row, column}] = *format;
    if (type == "C" && (has_value || !formula.empty())) {
      CVT_TRY(budget.take(1));
      cells.push_back({row, column, std::move(value), std::move(formula)});
    }
  }
  for (Pending& p : cells) {
    ir::Cell cell;
    std::string& v = p.value;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
      cell = ir::Cell::string(sylk_unescape(util::replace_all(v.substr(1, v.size() - 2), "\"\"", "\"")));
    } else if (v == "TRUE" || v == "FALSE") {
      cell = ir::Cell::boolean(v == "TRUE");
    } else if (const auto number = util::parse_double(v)) {
      FormatKind kind = FormatKind::General;
      std::string code;
      if (const auto it = cell_formats.find({p.row, p.column}); it != cell_formats.end() && it->second < formats.size()) {
        code = util::replace_all(formats[it->second], "\\", "");
        kind = classify_number_format(code);
      }
      cell = spreadsheet_number(*number, kind, date1904);
      if (!code.empty() && kind != FormatKind::General) {
        sheet.number_formats[{static_cast<std::uint32_t>(p.row - 1), static_cast<std::uint32_t>(p.column - 1)}] = code;
      }
    } else if (!v.empty()) {
      cell = ir::Cell::string(v);
    }
    const auto r = static_cast<std::uint32_t>(p.row - 1);
    const auto c = static_cast<std::uint32_t>(p.column - 1);
    CVT_TRY(put_cell(sheet, r, c, std::move(cell), budget));
    if (!p.formula.empty()) sheet.formulas[{r, c}] = r1c1_to_a1_formula(p.formula, r, c);
  }
  trim_sheet(sheet);
  sheet.has_header = guess_header(sheet);
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

std::string sylk_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == ';') {
      out += ";;";
    } else if (c == '"') {
      out += "\"\"";
    } else if (c == '\n' || c == '\r') {
      out += ' ';
    } else {
      out.push_back(c);
    }
  }
  return out;
}

Result<Bytes> write_sylk(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  std::map<std::string, std::size_t> formats;
  std::string body;
  for (std::size_t r = 0; r < sheet->rows.size(); ++r) {
    for (std::size_t c = 0; c < sheet->rows[r].size(); ++c) {
      const ir::Cell& cell = sheet->rows[r][c];
      if (cell.empty()) continue;
      const std::string code = cell_number_format(*sheet, r, c);
      if (!code.empty()) {
        const auto [it, inserted] = formats.emplace(code, formats.size());
        body += std::format("F;P{};FG0G;Y{};X{}\n", it->second, r + 1, c + 1);
      }
      body += std::format("C;Y{};X{};K", r + 1, c + 1);
      switch (cell.type) {
        case ir::CellType::String: body += "\"" + sylk_escape(cell.text) + "\""; break;
        case ir::CellType::Number: body += std::isfinite(cell.number) ? util::format_double(cell.number) : std::string("#NUM!"); break;
        case ir::CellType::Bool: body += cell.number != 0 ? "TRUE" : "FALSE"; break;
        case ir::CellType::DateTime: body += util::format_double(unix_to_excel_serial(cell.number)); break;
        case ir::CellType::Empty: break;
      }
      if (const std::string_view f = sheet->formula_at(r, c); !f.empty()) {
        body += ";E" + sylk_escape(a1_to_r1c1_formula(f, static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(c)));
      }
      body += "\n";
    }
    CVT_TRY(ctx.limits.check_output(body.size()));
  }
  std::vector<std::string> ordered(formats.size());
  for (const auto& [code, index] : formats) ordered[index] = code;
  std::string out = "ID;P\n";
  for (const std::string& code : ordered) {
    std::string escaped;
    for (const char c : code) {
      if (c == '-' || c == ':' || c == '/' || c == ' ') escaped.push_back('\\');
      escaped.push_back(c == ';' ? ',' : c);
    }
    out += "P;P" + escaped + "\n";
  }
  out += std::format("B;Y{};X{}\n", std::max<std::size_t>(1, sheet->rows.size()), std::max<std::size_t>(1, sheet->column_count()));
  out += body;
  out += "E\n";
  return encode_output_text(out, ctx.options.get_string("encoding", "windows-1252"), false, ctx);
}

// --------------------------------------------------------------------------- DIF

Result<AnyIR> read_dif(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const std::string text, decode_source_text(ctx.input, ctx.options.get_string("source_encoding", "auto"), ctx));
  const std::vector<std::string> lines = logical_lines(text);
  if (lines.empty() || util::trim(lines[0]) != "TABLE") return fail(Status::Decode, "not a DIF file (no TABLE header)");
  std::size_t i = 0;
  // Header: topic, "vector,value", "string" triplets up to DATA.
  bool data = false;
  while (i + 2 < lines.size()) {
    const std::string_view topic = util::trim(lines[i]);
    i += 3;
    if (topic == "DATA") {
      data = true;
      break;
    }
  }
  if (!data) return fail(Status::Decode, "the DIF file has no DATA section");
  ir::Sheet sheet;
  sheet.name = file_stem(ctx.filename);
  CellBudget budget(ctx.limits);
  std::int64_t row = -1;
  std::uint64_t column = 0;
  const InferOptions infer = infer_options_from(ctx.options);
  while (i + 1 < lines.size()) {
    const std::string_view head = util::trim(lines[i]);
    const std::string_view payload = lines[i + 1];
    i += 2;
    const std::size_t comma = head.find(',');
    const auto type = util::parse_int(head.substr(0, comma));
    const std::string_view number = comma == std::string_view::npos ? std::string_view() : head.substr(comma + 1);
    if (!type) return fail(Status::Decode, "damaged DIF data item");
    if (*type == -1) {
      const std::string_view word = util::trim(payload);
      if (word == "BOT") {
        ++row;
        column = 0;
        continue;
      }
      if (word == "EOD") break;
      continue;
    }
    if (row < 0) return fail(Status::Decode, "DIF data before the first BOT");
    ir::Cell cell;
    if (*type == 0) {
      const std::string_view indicator = util::trim(payload);
      if (indicator == "V") {
        if (const auto value = util::parse_double(number)) {
          cell = number_cell(*value);
        } else if (!number.empty()) {
          // Some writers put the formatted value here ("TRUE", "2024-05-01").
          cell = infer_cell(std::string(number), infer);
        }
      } else if (indicator == "TRUE" || indicator == "FALSE") {
        cell = ir::Cell::boolean(indicator == "TRUE");
      } else if (indicator == "NA" || indicator == "ERROR") {
        cell = ir::Cell::string(indicator == "NA" ? "#N/A" : "#VALUE!");
      }
    } else if (*type == 1) {
      std::string_view value = payload;
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
      std::string unescaped = util::replace_all(value, "\"\"", "\"");
      cell = infer.dates || infer.numbers ? (unescaped.empty() ? ir::Cell{} : ir::Cell::string(std::move(unescaped)))
                                          : ir::Cell::string(std::move(unescaped));
    }
    if (!cell.empty()) CVT_TRY(put_cell(sheet, static_cast<std::uint64_t>(row), column, std::move(cell), budget));
    ++column;
  }
  trim_sheet(sheet);
  sheet.has_header = guess_header(sheet);
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

Result<Bytes> write_dif(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const std::size_t columns = sheet->column_count();
  std::string out = std::format("TABLE\r\n0,1\r\n\"\"\r\nVECTORS\r\n0,{}\r\n\"\"\r\nTUPLES\r\n0,{}\r\n\"\"\r\nDATA\r\n0,0\r\n\"\"\r\n",
                                columns, sheet->rows.size());
  for (std::size_t r = 0; r < sheet->rows.size(); ++r) {
    out += "-1,0\r\nBOT\r\n";
    for (std::size_t c = 0; c < columns; ++c) {
      const ir::Cell& cell = sheet->at(r, c);
      switch (cell.type) {
        case ir::CellType::Number:
          out += std::isfinite(cell.number) ? std::format("0,{}\r\nV\r\n", util::format_double(cell.number)) : std::string("0,0\r\nERROR\r\n");
          break;
        case ir::CellType::Bool: out += cell.number != 0 ? "0,1\r\nTRUE\r\n" : "0,0\r\nFALSE\r\n"; break;
        case ir::CellType::Empty: out += "1,0\r\n\"\"\r\n"; break;
        case ir::CellType::String:
        case ir::CellType::DateTime: {
          std::string text = util::replace_all(cell.text, "\"", "\"\"");
          for (char& ch : text) {
            if (ch == '\n' || ch == '\r') ch = ' ';
          }
          out += "1,0\r\n\"" + text + "\"\r\n";
          break;
        }
      }
    }
    CVT_TRY(ctx.limits.check_output(out.size()));
  }
  out += "-1,0\r\nEOD\r\n";
  return encode_output_text(out, ctx.options.get_string("encoding", "windows-1252"), false, ctx);
}

// --------------------------------------------------------------------------- Lotus 1-2-3 / Quattro Pro

DateStyle lotus_date_style(std::uint8_t format) {
  if ((format & 0x70) != 0x70) return DateStyle::Auto;  // not a "special" format
  switch (format & 0x0F) {
    case 2: case 3: case 4: case 9: case 10: return DateStyle::Date;
    case 7: case 8: case 11: case 12: return DateStyle::Time;
    default: return DateStyle::Auto;
  }
}

Result<AnyIR> read_lotus(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (r.u16le() != 0x0000 || r.u16le() != 2) return fail(Status::Decode, "not a Lotus 1-2-3 worksheet");
  const std::uint16_t version = r.u16le();
  if (version != 0x0404 && version != 0x0405 && version != 0x0406 && version != 0x5120 && version != 0x5121) {
    return fail(Status::Decode, "unsupported Lotus worksheet version 0x{:04X} (only WKS, WK1 and WQ1 are read)", version);
  }
  const CodePageInfo* page = find_code_page(437);
  ir::Sheet sheet;
  sheet.name = file_stem(ctx.filename);
  CellBudget budget(ctx.limits);
  std::size_t count = 0;
  std::optional<std::pair<std::uint16_t, std::uint16_t>> pending_string;
  while (!r.at_end()) {
    const std::uint16_t opcode = r.u16le();
    const std::uint16_t length = r.u16le();
    util::ByteReader rec = r.sub(length);
    if (!r.ok()) return fail(Status::Decode, "truncated Lotus record");
    if ((++count & 8191) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r.position()) / static_cast<double>(r.size())));
    if (opcode == 0x0001) break;  // EOF
    if (opcode < 0x0D || opcode > 0x33 || (opcode > 0x10 && opcode != 0x33)) continue;
    const std::uint8_t format = rec.u8();
    const std::uint16_t column = rec.u16le();
    const std::uint16_t row = rec.u16le();
    if (!rec.ok()) continue;
    ir::Cell cell;
    const auto numeric = [&](double value) {
      const DateStyle style = lotus_date_style(format);
      if (style != DateStyle::Auto && std::isfinite(value) && value >= 0 && value < 2958466.0) {
        return datetime_cell(excel_serial_to_unix(value), style);
      }
      return number_cell(value);
    };
    switch (opcode) {
      case 0x0D: cell = numeric(rec.i16le()); break;
      case 0x0E:
      case 0x10: {
        const double value = rec.f64le();
        if (rec.ok()) cell = numeric(value);
        if (opcode == 0x10 && std::isnan(value)) {
          pending_string = std::pair<std::uint16_t, std::uint16_t>{row, column};  // string result follows
          cell = ir::Cell{};
        }
        break;
      }
      case 0x0F:
      case 0x33: {
        std::string_view text = rec.cstring();
        if (!rec.ok()) text = as_chars(rec.rest());
        if (opcode == 0x0F && !text.empty() && (text[0] == '\'' || text[0] == '"' || text[0] == '^' || text[0] == '\\')) text.remove_prefix(1);
        if (!text.empty()) cell = ir::Cell::string(decode_code_page(as_bytes(text), *page));
        pending_string.reset();
        break;
      }
      default: break;
    }
    if (!cell.empty()) CVT_TRY(put_cell(sheet, row, column, std::move(cell), budget));
  }
  trim_sheet(sheet);
  sheet.has_header = guess_header(sheet);
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

int sniff_sylk(ByteView data, std::string_view) {
  const std::string_view head = as_chars(data).substr(0, 64);
  return head.starts_with("ID;P") || head.starts_with("ID;N") ? 85 : 0;
}

int sniff_dif(ByteView data, std::string_view) {
  const std::string_view head = as_chars(data).substr(0, 64);
  return head.starts_with("TABLE\r\n0,1") || head.starts_with("TABLE\n0,1") ? 85 : 0;
}

}  // namespace

void register_legacy_sheets(Registry& registry) {
  const std::vector<OptionSpec> text_read_options = {source_encoding_option()};
  const OptionSpec encoding = OptionSpec::choice("encoding", "Encoding", "windows-1252", {"windows-1252", "utf-8", "cp437", "cp850"},
                                                 "Character encoding of the output.")
                                  .advanced();
  {
    FormatInfo info;
    info.id = "slk";
    info.name = "SYLK spreadsheet";
    info.description = "Symbolic Link interchange format of Multiplan and Excel.";
    info.category = Category::Spreadsheet;
    info.extensions = {"slk", "sylk"};
    info.mime = "application/x-sylk";
    info.popularity = 10;
    info.sniff = sniff_sylk;
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = "slk";
    codec.kind = Kind::Table;
    codec.read = read_sylk;
    codec.write = write_sylk;
    codec.read_options = text_read_options;
    codec.write_options = {encoding, sheet_option()};
    registry.add_codec(std::move(codec));
  }
  {
    FormatInfo info;
    info.id = "dif";
    info.name = "Data Interchange Format";
    info.description = "DIF spreadsheet interchange file (VisiCalc lineage).";
    info.category = Category::Spreadsheet;
    info.extensions = {"dif"};
    info.mime = "application/x-dif";
    info.popularity = 8;
    info.sniff = sniff_dif;
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = "dif";
    codec.kind = Kind::Table;
    codec.read = read_dif;
    codec.write = write_dif;
    codec.read_options = text_read_options;
    codec.write_options = {encoding, sheet_option()};
    registry.add_codec(std::move(codec));
  }
  struct Lotus {
    std::string_view id;
    std::string_view name;
    std::vector<std::string> extensions;
    SignaturePattern version;
  };
  const Lotus lotus[] = {
      {"wk1", "Lotus 1-2-3 worksheet", {"wk1", "wks"}, "00 00 02 00 06 04"},
      {"wq1", "Quattro Pro worksheet", {"wq1"}, "00 00 02 00 20 51"},
  };
  for (const Lotus& l : lotus) {
    FormatInfo info;
    info.id = std::string(l.id);
    info.name = std::string(l.name);
    info.description = "DOS-era spreadsheet (read only).";
    info.category = Category::Spreadsheet;
    info.extensions = l.extensions;
    info.mime = l.id == "wk1" ? "application/vnd.lotus-1-2-3" : "application/x-quattro-pro";
    info.popularity = 5;
    info.signatures = {Signature::at(0, l.version)};
    if (l.id == "wk1") info.signatures.push_back(Signature::at(0, "00 00 02 00 04 04"));
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = std::string(l.id);
    codec.kind = Kind::Table;
    codec.read = read_lotus;
    registry.add_codec(std::move(codec));
  }
}

}  // namespace cvt::table
