// Convertible core - table family: dBase / xBase tables (.dbf).
//
// Reader: dBase III, IV, V and 7, FoxBASE, FoxPro and Visual FoxPro files.
// Field types C, N, F, L, D, I, B (double in Visual FoxPro), Y (currency),
// T (date-time), @ (timestamp), O (double), + (autoincrement), V, Q; memo
// fields (M, G, P) live in a separate .dbt/.fpt file that is not available,
// so they are left empty with a warning. Deleted records are skipped. Text
// is decoded with the code page named by the language driver byte (or the
// `source_encoding` option).
//
// Writer: dBase III with field types chosen from the cells (L, D, N with the
// needed decimals, C) in the chosen code page.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/table/codepages.hpp"
#include "families/table/common.hpp"
#include "families/table/table.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

struct DriverCodePage {
  std::uint8_t id;
  int code_page;
};

constexpr DriverCodePage drivers[] = {
    {0x01, 437},  {0x02, 850},  {0x03, 1252}, {0x04, 10000}, {0x08, 865},  {0x09, 437},  {0x0A, 850},  {0x0B, 437},
    {0x0D, 437},  {0x0E, 850},  {0x0F, 437},  {0x10, 850},   {0x11, 437},  {0x12, 850},  {0x14, 850},  {0x15, 437},
    {0x16, 850},  {0x17, 865},  {0x18, 437},  {0x19, 437},   {0x1A, 850},  {0x1B, 437},  {0x1F, 852},  {0x22, 852},
    {0x23, 852},  {0x24, 860},  {0x25, 850},  {0x26, 866},   {0x37, 850},  {0x40, 852},  {0x4D, 936},  {0x4E, 949},
    {0x4F, 950},  {0x50, 874},  {0x57, 1252}, {0x58, 1252},  {0x59, 1252}, {0x64, 852},  {0x65, 866},  {0x66, 865},
    {0x67, 861},  {0x6A, 737},  {0x6B, 857},  {0x6C, 863},   {0x78, 950},  {0x79, 949},  {0x7A, 936},  {0x7B, 932},
    {0x7C, 874},  {0x7D, 1255}, {0x7E, 1256}, {0x96, 10007}, {0x97, 10029}, {0x98, 10006}, {0xC8, 1250}, {0xC9, 1251},
    {0xCA, 1254}, {0xCB, 1253}, {0xCC, 1257},
};

const CodePageInfo* driver_code_page(std::uint8_t id) {
  for (const DriverCodePage& d : drivers) {
    if (d.id == id) return find_code_page(d.code_page);
  }
  return nullptr;
}

struct Field {
  std::string name;
  char type = 'C';
  std::uint32_t offset = 0;  // within the record, after the deletion flag
  std::uint32_t length = 0;
  std::uint8_t decimals = 0;
};

// Julian day number -> Unix days.
constexpr std::int64_t julian_unix_epoch = 2440588;

Result<AnyIR> read_dbf(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  const std::uint8_t version = r.u8();
  r.skip(3);
  const std::uint32_t record_count = r.u32le();
  const std::uint16_t header_length = r.u16le();
  const std::uint16_t record_length = r.u16le();
  if (!r.ok() || ctx.input.size() < 32) return fail(Status::Decode, "not a dBase file (header too short)");
  const bool level7 = (version & 0x07) == 4;
  const bool foxpro = version == 0x30 || version == 0x31 || version == 0x32 || version == 0xF5 || version == 0xFB;
  const std::size_t descriptor_size = level7 ? 48 : 32;
  const std::size_t first_descriptor = level7 ? 68 : 32;
  if (header_length < first_descriptor + 1 || header_length > ctx.input.size() || record_length < 1) {
    return fail(Status::Decode, "not a dBase file (inconsistent header)");
  }
  const std::uint8_t driver = std::to_integer<std::uint8_t>(ctx.input[29]);

  std::vector<Field> fields;
  std::uint32_t offset = 0;
  for (std::size_t at = first_descriptor; at + descriptor_size <= header_length; at += descriptor_size) {
    if (std::to_integer<std::uint8_t>(ctx.input[at]) == 0x0D) break;
    util::ByteReader d(ctx.input.subspan(at, descriptor_size));
    Field field;
    const std::size_t name_size = level7 ? 32 : 11;
    std::string_view name = d.chars(name_size);
    name = name.substr(0, name.find('\0'));
    field.name = std::string(util::trim(name));
    field.type = static_cast<char>(d.u8());
    if (level7) {
      field.length = d.u8();
      field.decimals = d.u8();
    } else {
      d.skip(4);  // displacement (FoxPro) or reserved
      field.length = d.u8();
      field.decimals = d.u8();
      if (field.type == 'C' && !foxpro && field.decimals > 0) {
        field.length += static_cast<std::uint32_t>(field.decimals) << 8;  // Clipper long character fields
        field.decimals = 0;
      }
    }
    field.offset = offset;
    offset += field.length;
    if (fields.size() >= 2048) return fail(Status::Decode, "the dBase file declares too many fields");
    fields.push_back(std::move(field));
  }
  if (fields.empty()) return fail(Status::Decode, "the dBase file has no fields");
  if (offset + 1 > record_length) return fail(Status::Decode, "dBase field lengths exceed the record length");
  const std::uint64_t available = (ctx.input.size() - header_length) / record_length;
  std::uint64_t records = record_count;
  if (records > available) {
    ctx.warn(std::format("The header announces {} records but the file holds {}; the rest is missing.", records, available));
    records = available;
  }

  // Text decoding.
  const std::string encoding = ctx.options.get_string("source_encoding", "auto");
  const CodePageInfo* page = nullptr;
  bool utf8 = false;
  if (encoding == "auto") {
    page = driver_code_page(driver);
    if (page == nullptr) {
      const ByteView body = ctx.input.subspan(header_length, static_cast<std::size_t>(records * record_length));
      utf8 = util::is_valid_utf8(as_chars(body));
      page = find_code_page(1252);
    }
  } else if (encoding == "utf-8") {
    utf8 = true;
  } else {
    page = find_code_page(encoding);
    if (page == nullptr) return fail(Status::InvalidArgument, "unsupported source encoding '{}' for dBase files", encoding);
  }
  const auto text_of = [&](ByteView bytes) {
    if (utf8 || page == nullptr) return util::sanitize_utf8(as_chars(bytes));
    return decode_code_page(bytes, *page);
  };

  ir::Sheet sheet;
  {
    const std::string_view name = ctx.filename;
    const std::size_t slash = name.find_last_of("/\\");
    std::string stem(slash == std::string_view::npos ? name : name.substr(slash + 1));
    if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0) stem.resize(dot);
    sheet.name = stem;
  }
  sheet.has_header = true;
  std::vector<std::size_t> visible;  // fields shown (the _NullFlags system field is hidden)
  std::vector<ir::Cell> header;
  for (std::size_t f = 0; f < fields.size(); ++f) {
    if (fields[f].type == '0') continue;
    visible.push_back(f);
    header.push_back(ir::Cell::string(text_of(as_bytes(fields[f].name))));
  }
  sheet.rows.push_back(std::move(header));
  CellBudget budget(ctx.limits);
  bool memo = false;
  for (std::uint64_t i = 0; i < records; ++i) {
    if ((i & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(records)));
    const ByteView record = ctx.input.subspan(static_cast<std::size_t>(header_length + i * record_length), record_length);
    if (std::to_integer<std::uint8_t>(record[0]) == '*') continue;  // deleted
    if (std::to_integer<std::uint8_t>(record[0]) == 0x1A) break;
    CVT_TRY(budget.take(visible.size() + 1));
    std::vector<ir::Cell> row;
    for (const std::size_t f : visible) {
      const Field& field = fields[f];
      const ByteView raw = record.subspan(1 + field.offset, field.length);
      const std::string_view chars = as_chars(raw);
      ir::Cell cell;
      switch (field.type) {
        case 'C':
        case 'V': {
          std::string text = text_of(raw);
          while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
          if (!text.empty()) cell = ir::Cell::string(std::move(text));
          break;
        }
        case 'N':
        case 'F': {
          const std::string_view text = util::trim(chars);
          if (!text.empty() && text.find_first_not_of('*') != std::string_view::npos) {
            if (const auto number = parse_number(text)) {
              cell = ir::Cell::numeric(*number, field.decimals == 0 && util::parse_int(text) ? std::string(text) : format_number(*number));
            } else if (const auto comma = parse_number(text, ',')) {
              cell = number_cell(*comma);
            }
          }
          break;
        }
        case 'L': {
          const char c = chars.empty() ? ' ' : chars[0];
          if (c == 'T' || c == 't' || c == 'Y' || c == 'y') cell = ir::Cell::boolean(true);
          if (c == 'F' || c == 'f' || c == 'N' || c == 'n') cell = ir::Cell::boolean(false);
          break;
        }
        case 'D': {
          const std::string_view text = util::trim(chars);
          if (text.size() == 8) {
            const auto y = util::parse_int(text.substr(0, 4));
            const auto m = util::parse_int(text.substr(4, 2));
            const auto d = util::parse_int(text.substr(6, 2));
            if (y && m && d && *m >= 1 && *m <= 12 && *d >= 1 && *d <= 31) {
              cell = datetime_cell(static_cast<double>(days_from_civil(*y, static_cast<unsigned>(*m), static_cast<unsigned>(*d))) * 86400.0, DateStyle::Date);
            }
          }
          break;
        }
        case 'I':
        case '+': {
          if (raw.size() == 4) {
            util::ByteReader v(raw);
            std::int32_t value = 0;
            if (level7) {
              value = static_cast<std::int32_t>(v.u32be() ^ 0x80000000u);
            } else {
              value = v.i32le();
            }
            cell = integer_cell(value);
          }
          break;
        }
        case 'B':
        case 'O': {
          if (raw.size() == 8 && (foxpro || field.type == 'O')) {
            util::ByteReader v(raw);
            double value = 0;
            if (field.type == 'O') {
              std::uint64_t bits = v.u64be();
              bits = (bits & 0x8000000000000000ull) != 0 ? bits ^ 0x8000000000000000ull : ~bits;
              value = std::bit_cast<double>(bits);
            } else {
              value = v.f64le();
            }
            cell = number_cell(value);
          } else if (!util::trim(chars).empty()) {
            memo = true;
          }
          break;
        }
        case 'Y': {
          if (raw.size() == 8) {
            util::ByteReader v(raw);
            const std::int64_t value = v.i64le();
            cell = ir::Cell::numeric(static_cast<double>(value) / 10000.0,
                                     std::format("{}{}.{:04}", value < 0 ? "-" : "", (value < 0 ? -(value / 10000) : value / 10000),
                                                 (value < 0 ? -(value % 10000) : value % 10000)));
          }
          break;
        }
        case 'T':
        case '@': {
          if (raw.size() == 8) {
            util::ByteReader v(raw);
            const std::uint32_t day = field.type == '@' ? v.u32be() : v.u32le();
            const std::uint32_t ms = field.type == '@' ? v.u32be() : v.u32le();
            if (day != 0) {
              const double unix_seconds = static_cast<double>(static_cast<std::int64_t>(day) - julian_unix_epoch) * 86400.0 + ms / 1000.0;
              cell = datetime_cell(unix_seconds, DateStyle::DateTime);
            }
          }
          break;
        }
        case 'Q': {
          if (!raw.empty()) cell = ir::Cell::string(util::hex_encode(raw));
          break;
        }
        case 'M':
        case 'G':
        case 'P': {
          const std::string_view text = util::trim(chars);
          bool reference = false;
          for (const char c : text) reference = reference || (c != '0' && c != ' ' && c != '\0');
          if (raw.size() == 4) {
            util::ByteReader v(raw);
            reference = v.u32le() != 0;
          }
          memo = memo || reference;
          break;
        }
        default: break;
      }
      row.push_back(std::move(cell));
    }
    sheet.rows.push_back(std::move(row));
  }
  if (memo) ctx.warn("Memo fields are stored in a separate .dbt/.fpt file, which is not available; they were left empty.");
  trim_sheet(sheet);
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

// --------------------------------------------------------------------------- writing

struct OutField {
  std::string name;
  char type = 'C';
  std::uint32_t length = 1;
  std::uint8_t decimals = 0;
};

std::string dbf_field_name(std::string_view text, std::size_t index, std::set<std::string>& used) {
  std::string name;
  for (const char c : text) {
    const char u = util::ascii_upper(c);
    if (util::is_ascii_alpha(u) || util::is_ascii_digit(u) || u == '_') {
      name.push_back(u);
    } else if (c == ' ' || c == '-' || c == '.') {
      name.push_back('_');
    }
  }
  if (name.empty() || !util::is_ascii_alpha(name[0])) name = std::format("F{}{}", index + 1, name.empty() ? "" : "_" + name);
  name.resize(std::min<std::size_t>(name.size(), 10));
  std::string candidate = name;
  for (int n = 2; used.contains(candidate); ++n) {
    const std::string suffix = std::format("_{}", n);
    candidate = name.substr(0, 10 - suffix.size()) + suffix;
  }
  used.insert(candidate);
  return candidate;
}

Result<Bytes> write_dbf(const AnyIR& value, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const ir::Sheet* sheet, select_sheet(value.get<ir::Table>(), ctx));
  const std::string encoding = ctx.options.get_string("dbf_encoding", "windows-1252");
  const CodePageInfo* page = find_code_page(encoding);
  if (page == nullptr) return fail(Status::InvalidArgument, "unsupported dBase encoding '{}'", encoding);
  std::uint8_t driver = 0x57;
  for (const DriverCodePage& d : drivers) {
    if (d.code_page == page->code_page) {
      driver = d.id;
      break;
    }
  }
  if (page->code_page == 1252) driver = 0x03;
  const bool use_header = sheet->has_header;
  const std::size_t first = use_header ? 1 : 0;
  const std::size_t columns = std::min<std::size_t>(sheet->column_count(), 255);
  if (sheet->column_count() > 255) ctx.warn("dBase tables hold at most 255 fields; the remaining columns were dropped.");
  std::vector<OutField> fields;
  std::set<std::string> used;
  const auto encode = [&](std::string_view text) { return encode_code_page(text, *page); };
  std::size_t truncated = 0;
  for (std::size_t c = 0; c < columns; ++c) {
    OutField field;
    field.name = dbf_field_name(use_header ? sheet->at(0, c).text : std::string(), c, used);
    bool any = false;
    bool bools = true;
    bool dates = true;
    bool numbers = true;
    std::size_t max_text = 1;
    int decimals = 0;
    std::size_t integer_digits = 1;
    for (std::size_t r = first; r < sheet->rows.size(); ++r) {
      const ir::Cell& cell = sheet->at(r, c);
      if (cell.empty()) continue;
      any = true;
      bools = bools && cell.type == ir::CellType::Bool;
      const double day_seconds = cell.number - std::floor(cell.number / 86400.0) * 86400.0;
      dates = dates && cell.type == ir::CellType::DateTime && day_seconds == 0.0;
      numbers = numbers && cell.type == ir::CellType::Number && std::isfinite(cell.number);
      max_text = std::max(max_text, encode(cell.text).size());
      if (cell.type == ir::CellType::Number && std::isfinite(cell.number)) {
        const std::string shortest = util::format_double(cell.number);
        if (shortest.find('e') == std::string::npos) {
          const std::size_t dot = shortest.find('.');
          if (dot != std::string::npos) decimals = std::max(decimals, static_cast<int>(shortest.size() - dot - 1));
          const std::size_t digits = (dot == std::string::npos ? shortest.size() : dot) - (cell.number < 0 ? 1 : 0);
          integer_digits = std::max(integer_digits, digits + (cell.number < 0 ? 1 : 0));
        } else {
          numbers = numbers && std::fabs(cell.number) < 1e18 && std::fabs(cell.number) >= 1e-10;
          decimals = std::max(decimals, 10);
          integer_digits = std::max<std::size_t>(integer_digits, std::fabs(cell.number) >= 1 ? static_cast<std::size_t>(std::log10(std::fabs(cell.number))) + 2 : 2);
        }
      }
    }
    decimals = std::min(decimals, 15);
    if (!any) {
      field.type = 'C';
      field.length = 1;
    } else if (bools) {
      field.type = 'L';
      field.length = 1;
    } else if (dates) {
      field.type = 'D';
      field.length = 8;
    } else if (numbers && integer_digits + static_cast<std::size_t>(decimals) + (decimals > 0 ? 1 : 0) <= 20) {
      field.type = 'N';
      field.decimals = static_cast<std::uint8_t>(decimals);
      field.length = static_cast<std::uint32_t>(integer_digits + static_cast<std::size_t>(decimals) + (decimals > 0 ? 1 : 0));
    } else {
      field.type = 'C';
      field.length = static_cast<std::uint32_t>(std::min<std::size_t>(max_text, 254));
    }
    fields.push_back(std::move(field));
  }
  if (fields.empty()) fields.push_back(OutField{"FIELD1", 'C', 1, 0});
  std::uint32_t record_length = 1;
  for (const OutField& field : fields) record_length += field.length;
  if (record_length > 65535) return fail(Status::Encode, "the rows are too wide for a dBase file");
  const std::size_t records = sheet->rows.size() > first ? sheet->rows.size() - first : 0;
  const auto header_length = static_cast<std::uint16_t>(32 + 32 * fields.size() + 1);

  util::ByteWriter out(header_length + records * record_length + 1);
  out.u8(0x03);
  out.u8(100).u8(1).u8(1);  // last update 2000-01-01: a fixed date keeps the output reproducible
  out.u32le(static_cast<std::uint32_t>(records));
  out.u16le(header_length);
  out.u16le(static_cast<std::uint16_t>(record_length));
  out.zeros(17);
  out.u8(driver);
  out.zeros(2);
  for (const OutField& field : fields) {
    std::string name = field.name;
    name.resize(11, '\0');
    out.text(name);
    out.u8(static_cast<std::uint8_t>(field.type));
    out.zeros(4);
    out.u8(static_cast<std::uint8_t>(field.length));
    out.u8(field.decimals);
    out.zeros(14);
  }
  out.u8(0x0D);
  for (std::size_t r = first; r < sheet->rows.size(); ++r) {
    if ((r & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(sheet->rows.size())));
    out.u8(' ');
    for (std::size_t c = 0; c < fields.size(); ++c) {
      const OutField& field = fields[c];
      const ir::Cell& cell = sheet->at(r, c);
      std::string text;
      switch (field.type) {
        case 'L': text = cell.empty() ? "?" : cell.number != 0 ? "T" : "F"; break;
        case 'D':
          if (!cell.empty()) {
            const CivilDate date = civil_from_days(static_cast<std::int64_t>(std::floor(cell.number / 86400.0)));
            text = std::format("{:04}{:02}{:02}", date.year, date.month, date.day);
          }
          break;
        case 'N':
          if (!cell.empty()) {
            text = field.decimals == 0 ? std::format("{}", static_cast<std::int64_t>(std::llround(cell.number)))
                                       : std::format("{:.{}f}", cell.number, static_cast<int>(field.decimals));
            if (text.size() < field.length) text.insert(0, field.length - text.size(), ' ');
          }
          break;
        default:
          text = encode(cell.text);
          if (text.size() > field.length) {
            text.resize(field.length);
            ++truncated;
          }
          break;
      }
      text.resize(field.length, ' ');
      out.text(text);
    }
    CVT_TRY(ctx.limits.check_output(out.size()));
  }
  out.u8(0x1A);
  if (truncated > 0) ctx.warn(std::format("{} text value(s) were longer than 254 bytes and were cut.", truncated));
  return out.take();
}

int sniff_dbf(ByteView data, std::string_view) {
  if (data.size() < 33) return 0;
  util::ByteReader r(data);
  const std::uint8_t version = r.u8();
  static constexpr std::uint8_t versions[] = {0x02, 0x03, 0x04, 0x05, 0x30, 0x31, 0x32, 0x43, 0x63, 0x83, 0x8B, 0x8C, 0xCB, 0xF5, 0xFB};
  if (std::ranges::find(versions, version) == std::end(versions)) return 0;
  const std::uint8_t yy = r.u8();
  const std::uint8_t mm = r.u8();
  const std::uint8_t dd = r.u8();
  const std::uint32_t records = r.u32le();
  const std::uint16_t header = r.u16le();
  const std::uint16_t length = r.u16le();
  (void)yy;
  if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || header < 65 || length < 2) return 0;
  const bool level7 = (version & 0x07) == 4;
  if (!level7 && (header - 33) % 32 != 0 && version != 0x30 && version != 0x31 && version != 0x32) return 0;
  if (header > data.size()) return 0;
  if (std::uint64_t{header} + std::uint64_t{records} * length > data.size() + 1 + length) return 30;
  return 70;
}

}  // namespace

void register_dbf(Registry& registry) {
  FormatInfo info;
  info.id = "dbf";
  info.name = "dBase table";
  info.description = "dBase / FoxPro database table, also used for the attributes of shapefiles.";
  info.category = Category::Spreadsheet;
  info.extensions = {"dbf"};
  info.mime = "application/dbase";
  info.popularity = 30;
  info.sniff = sniff_dbf;
  registry.add_format(std::move(info));

  std::vector<std::string> encodings;
  for (const char* name : {"windows-1252", "cp437", "cp850", "cp852", "cp866", "windows-1250", "windows-1251",
                           "windows-1253", "windows-1254", "windows-1257", "mac-roman"}) {
    encodings.emplace_back(name);
  }
  Codec codec;
  codec.format_id = "dbf";
  codec.kind = Kind::Table;
  codec.read = read_dbf;
  codec.write = write_dbf;
  std::vector<std::string> read_encodings = {"auto", "utf-8"};
  for (const std::string& e : encodings) read_encodings.push_back(e);
  codec.read_options = {OptionSpec::choice("source_encoding", "Source encoding", "auto", std::move(read_encodings),
                                           "Code page of the text; auto uses the file's language driver byte.")
                            .advanced()};
  codec.write_options = {
      OptionSpec::choice("dbf_encoding", "Code page", "windows-1252", std::move(encodings),
                         "Code page the text is written in; it is recorded in the file's language driver byte."),
      sheet_option(),
  };
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::table
