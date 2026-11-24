// Convertible core - table family: Excel binary workbooks (XLSB) reader.
//
// An XLSB file is an OPC (ZIP) package like XLSX, but its parts are BIFF12
// record streams [MS-XLSB]: a record is a 7-bit-encoded type and size
// followed by the payload. The reader takes sheet names and visibility, the
// date system, shared strings, number formats (for dates), cell values
// (RK, real, string, boolean, error and the cached results of formulas),
// merged cells, column widths and frozen panes. Formula text is not
// decompiled from its parsed form.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <bit>
#include <cstdint>
#include <optional>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/table/common.hpp"
#include "families/table/ooxml.hpp"
#include "families/table/spreadsheet.hpp"
#include "families/table/table.hpp"
#include "families/table/xlsx.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::table {

namespace {

struct Record {
  std::uint32_t type = 0;
  ByteView data;
};

// Iterates the records of a BIFF12 stream.
class RecordReader {
 public:
  explicit RecordReader(ByteView data) : reader_(data) {}

  /// Next record; nullopt at the end. A damaged header ends the stream with `ok() == false`.
  std::optional<Record> next() {
    if (reader_.at_end()) return std::nullopt;
    Record record;
    const std::uint8_t b0 = reader_.u8();
    record.type = b0 & 0x7Fu;
    if ((b0 & 0x80u) != 0) {
      const std::uint8_t b1 = reader_.u8();
      record.type |= static_cast<std::uint32_t>(b1 & 0x7Fu) << 7;
    }
    std::uint32_t size = 0;
    for (int i = 0; i < 4; ++i) {
      const std::uint8_t b = reader_.u8();
      size |= static_cast<std::uint32_t>(b & 0x7Fu) << (7 * i);
      if ((b & 0x80u) == 0) break;
    }
    record.data = reader_.bytes(size);
    if (!reader_.ok()) {
      damaged_ = true;
      return std::nullopt;
    }
    return record;
  }
  [[nodiscard]] bool damaged() const noexcept { return damaged_; }

 private:
  util::ByteReader reader_;
  bool damaged_ = false;
};

// XLWideString: u32 character count + UTF-16LE characters.
std::optional<std::string> wide_string(util::ByteReader& reader, bool nullable = false) {
  const std::uint32_t count = reader.u32le();
  if (!reader.ok()) return std::nullopt;
  if (nullable && count == 0xFFFFFFFFu) return std::string();
  if (count > reader.remaining() / 2) return std::nullopt;
  std::u16string units(count, u'\0');
  for (char16_t& unit : units) unit = static_cast<char16_t>(reader.u16le());
  return util::utf16_to_utf8(units);
}

double rk_value(std::uint32_t rk) {
  double value = 0.0;
  if ((rk & 2u) != 0) {
    value = static_cast<double>(static_cast<std::int32_t>(rk) >> 2);
  } else {
    value = std::bit_cast<double>(static_cast<std::uint64_t>(rk & 0xFFFFFFFCu) << 32);
  }
  return (rk & 1u) != 0 ? value / 100.0 : value;
}

Result<Bytes> read_part(util::ZipReader& zip, std::string_view name) {
  const auto index = zip.find(name);
  if (!index) return fail(Status::Decode, "the package has no part '{}'", name);
  return zip.read(*index);
}

Result<AnyIR> read_xlsb(ReadContext& ctx) {
  CVT_TRY_ASSIGN(auto zip, util::ZipReader::open(ctx.input, ctx.limits));
  CVT_TRY_ASSIGN(const std::string workbook_part, main_part(zip, "xl/workbook.bin"));
  CVT_TRY_ASSIGN(const Bytes workbook, read_part(zip, workbook_part));
  CVT_TRY_ASSIGN(const auto rels, read_relationships(zip, workbook_part));

  struct SheetInfo {
    std::string name;
    std::string part;
    ir::SheetVisibility visibility = ir::SheetVisibility::Visible;
  };
  std::vector<SheetInfo> sheets;
  bool date1904 = false;
  {
    RecordReader records(workbook);
    while (const auto record = records.next()) {
      util::ByteReader r(record->data);
      if (record->type == 153) {  // BrtWbProp
        date1904 = (r.u32le() & 1u) != 0;
      } else if (record->type == 156) {  // BrtBundleSh
        const std::uint32_t state = r.u32le();
        (void)r.u32le();  // iTabID
        const auto rel_id = wide_string(r, true);
        const auto name = wide_string(r);
        if (!rel_id || !name) return fail(Status::Decode, "damaged sheet record in the workbook");
        SheetInfo info;
        info.name = *name;
        info.visibility = state == 1 ? ir::SheetVisibility::Hidden
                          : state == 2 ? ir::SheetVisibility::VeryHidden
                                       : ir::SheetVisibility::Visible;
        for (const Relationship& rel : rels) {
          if (rel.id == *rel_id) info.part = rel.target;
        }
        CVT_TRY(ctx.limits.check_archive_entries(sheets.size() + 1));
        sheets.push_back(std::move(info));
      }
    }
    if (records.damaged()) return fail(Status::Decode, "the workbook part is truncated");
  }

  CellBudget budget(ctx.limits);
  std::vector<std::string> strings;
  std::vector<FormatKind> kinds;
  std::vector<std::string> codes;
  for (const Relationship& rel : rels) {
    if (rel.type.ends_with("/sharedStrings")) {
      auto data = read_part(zip, rel.target);
      if (!data) continue;
      RecordReader records(*data);
      while (const auto record = records.next()) {
        if (record->type != 19) continue;  // BrtSSTItem
        util::ByteReader r(record->data);
        (void)r.u8();  // flags: rich text / phonetic runs follow the string
        auto text = wide_string(r);
        if ((strings.size() & 4095) == 0) CVT_TRY(budget.take(4096));
        strings.push_back(text ? std::move(*text) : std::string());
      }
    } else if (rel.type.ends_with("/styles")) {
      auto data = read_part(zip, rel.target);
      if (!data) continue;
      std::map<std::uint32_t, std::string> custom;
      bool in_cell_xfs = false;
      RecordReader records(*data);
      while (const auto record = records.next()) {
        util::ByteReader r(record->data);
        if (record->type == 44) {  // BrtFmt
          const std::uint16_t id = r.u16le();
          if (auto code = wide_string(r)) custom[id] = std::move(*code);
        } else if (record->type == 617) {  // BrtBeginCellXFs
          in_cell_xfs = true;
        } else if (record->type == 618) {
          in_cell_xfs = false;
        } else if (record->type == 47 && in_cell_xfs && codes.size() < 1'000'000) {  // BrtXF
          (void)r.u16le();
          const std::uint16_t format = r.u16le();
          const auto it = custom.find(format);
          std::string code = it != custom.end() ? it->second : std::string(builtin_number_format(format));
          kinds.push_back(classify_number_format(code));
          codes.push_back(std::move(code));
        }
      }
    }
  }

  ir::Table table;
  for (std::size_t s = 0; s < sheets.size(); ++s) {
    ir::Sheet sheet;
    sheet.name = sheets[s].name;
    sheet.visibility = sheets[s].visibility;
    if (sheets[s].part.empty() || !zip.contains(sheets[s].part)) {
      table.sheets.push_back(std::move(sheet));
      continue;
    }
    CVT_TRY_ASSIGN(const Bytes data, read_part(zip, sheets[s].part));
    RecordReader records(data);
    std::uint32_t row = 0;
    std::size_t count = 0;
    while (const auto record = records.next()) {
      if ((++count & 16383) == 0) {
        CVT_TRY(ctx.checkpoint((static_cast<double>(s) + 0.5) / static_cast<double>(sheets.size())));
      }
      util::ByteReader r(record->data);
      const std::uint32_t type = record->type;
      if (type == 0) {  // BrtRowHdr
        row = r.u32le();
        continue;
      }
      if ((type >= 1 && type <= 11) || type == 62) {
        const std::uint32_t column = r.u32le();
        const std::uint32_t style = r.u24le();
        (void)r.u8();
        if (!r.ok() || row >= 1'048'576 || column >= 16'384) continue;
        FormatKind kind = FormatKind::General;
        std::string_view code;
        if (style < kinds.size()) {
          kind = kinds[style];
          code = codes[style];
        }
        ir::Cell cell;
        switch (type) {
          case 2: cell = spreadsheet_number(rk_value(r.u32le()), kind, date1904); break;
          case 3:
          case 11: cell = ir::Cell::string(std::string(excel_error_text(r.u8()))); break;
          case 4:
          case 10: cell = ir::Cell::boolean(r.u8() != 0); break;
          case 5:
          case 9: cell = spreadsheet_number(r.f64le(), kind, date1904); break;
          case 6:
          case 8: {
            if (auto text = wide_string(r)) cell = ir::Cell::string(std::move(*text));
            break;
          }
          case 62: {
            (void)r.u8();
            if (auto text = wide_string(r)) cell = ir::Cell::string(std::move(*text));
            break;
          }
          case 7: {
            const std::uint32_t index = r.u32le();
            if (r.ok() && index < strings.size()) cell = ir::Cell::string(strings[index]);
            break;
          }
          default: break;
        }
        if (!r.ok() || cell.empty()) continue;
        const bool numeric = cell.type == ir::CellType::Number || cell.type == ir::CellType::DateTime;
        CVT_TRY(put_cell(sheet, row, column, std::move(cell), budget));
        if (numeric && !code.empty()) sheet.number_formats[{row, column}] = std::string(code);
      } else if (type == 176) {  // BrtMergeCell
        ir::CellRange range;
        range.first_row = r.u32le();
        range.last_row = r.u32le();
        range.first_column = r.u32le();
        range.last_column = r.u32le();
        if (r.ok() && range.first_row <= range.last_row && range.first_column <= range.last_column &&
            sheet.merged.size() < 100'000) {
          sheet.merged.push_back(range);
        }
      } else if (type == 60) {  // BrtColInfo
        const std::uint32_t first = r.u32le();
        const std::uint32_t last = r.u32le();
        const std::uint32_t width = r.u32le();
        if (r.ok() && first <= last && last < 16'384 && width > 0) {
          if (sheet.column_widths.size() <= last) sheet.column_widths.resize(last + 1, 0.0);
          for (std::uint32_t c = first; c <= last; ++c) sheet.column_widths[c] = width / 256.0;
        }
      } else if (type == 151) {  // BrtPane
        const double x = r.f64le();
        const double y = r.f64le();
        (void)r.u32le();
        (void)r.u32le();
        (void)r.u32le();
        const std::uint8_t flags = r.u8();
        if (r.ok() && (flags & 1u) != 0) {
          if (x >= 0 && x < 16384) sheet.frozen_columns = static_cast<std::uint32_t>(x);
          if (y >= 0 && y < 1048576) sheet.frozen_rows = static_cast<std::uint32_t>(y);
        }
      }
    }
    if (records.damaged()) ctx.warn(std::format("Sheet \"{}\" is truncated; the cells read so far were kept.", sheet.name));
    trim_sheet(sheet);
    table.sheets.push_back(std::move(sheet));
  }
  guess_headers(table);
  return table;
}

}  // namespace

void register_xlsb(Registry& registry) {
  FormatInfo info;
  info.id = "xlsb";
  info.name = "Excel binary workbook";
  info.description = "Excel workbook stored as binary records (BIFF12) in a ZIP package.";
  info.category = Category::Spreadsheet;
  info.extensions = {"xlsb"};
  info.mime = "application/vnd.ms-excel.sheet.binary.macroEnabled.12";
  info.popularity = 25;
  info.sniff = [](ByteView data, std::string_view) { return excel_package_kind(data) == "xlsb" ? 95 : 0; };
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "xlsb";
  codec.kind = Kind::Table;
  codec.read = read_xlsb;
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::table
