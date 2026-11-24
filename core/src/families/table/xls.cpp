// Convertible core - table family: Excel 97-2003 workbooks (.xls) reader.
//
// The workbook lives in the "Workbook" (BIFF8) or "Book" (BIFF5/7) stream
// of a compound file. The reader understands the globals (code page, date
// system, number formats, XF records, the shared string table with its
// CONTINUE splits, rich text and Unicode/compressed characters, sheet
// directory with visibility) and each worksheet's cell records: NUMBER, RK,
// MULRK, LABELSST, LABEL, RSTRING, BOOLERR, FORMULA with cached number,
// string, boolean and error results (formula text is decompiled for common
// constructs), merged cells, column widths and frozen panes.
// Encrypted workbooks are recognised and refused.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/table/xls.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/table/cfb.hpp"
#include "families/table/codepages.hpp"
#include "families/table/common.hpp"
#include "families/table/spreadsheet.hpp"
#include "families/table/table.hpp"
#include "util/text.hpp"

namespace cvt::table {

double decode_rk(std::uint32_t rk) noexcept {
  double value = 0.0;
  if ((rk & 2u) != 0) {
    value = static_cast<double>(static_cast<std::int32_t>(rk) >> 2);
  } else {
    value = std::bit_cast<double>(static_cast<std::uint64_t>(rk & 0xFFFFFFFCu) << 32);
  }
  return (rk & 1u) != 0 ? value / 100.0 : value;
}

// --------------------------------------------------------------------------- formula decompiler

namespace {

struct FunctionInfo {
  std::uint16_t id;
  std::string_view name;
  int arguments;  // fixed count for ptgFunc; -1 when variable
};

constexpr FunctionInfo functions[] = {
    {0, "COUNT", -1},     {1, "IF", -1},          {2, "ISNA", 1},        {3, "ISERROR", 1},     {4, "SUM", -1},
    {5, "AVERAGE", -1},   {6, "MIN", -1},         {7, "MAX", -1},        {8, "ROW", -1},        {9, "COLUMN", -1},
    {10, "NA", 0},        {11, "NPV", -1},        {12, "STDEV", -1},     {13, "DOLLAR", -1},    {14, "FIXED", -1},
    {15, "SIN", 1},       {16, "COS", 1},         {17, "TAN", 1},        {18, "ATAN", 1},       {19, "PI", 0},
    {20, "SQRT", 1},      {21, "EXP", 1},         {22, "LN", 1},         {23, "LOG10", 1},      {24, "ABS", 1},
    {25, "INT", 1},       {26, "SIGN", 1},        {27, "ROUND", 2},      {28, "LOOKUP", -1},    {29, "INDEX", -1},
    {30, "REPT", 2},      {31, "MID", 3},         {32, "LEN", 1},        {33, "VALUE", 1},      {34, "TRUE", 0},
    {35, "FALSE", 0},     {36, "AND", -1},        {37, "OR", -1},        {38, "NOT", 1},        {39, "MOD", 2},
    {46, "VAR", -1},      {48, "TEXT", 2},        {56, "PV", -1},        {57, "FV", -1},        {58, "NPER", -1},
    {59, "PMT", -1},      {60, "RATE", -1},       {63, "RAND", 0},       {64, "MATCH", -1},     {65, "DATE", 3},
    {66, "TIME", 3},      {67, "DAY", 1},         {68, "MONTH", 1},      {69, "YEAR", 1},       {70, "WEEKDAY", -1},
    {71, "HOUR", 1},      {72, "MINUTE", 1},      {73, "SECOND", 1},     {74, "NOW", 0},        {76, "ROWS", 1},
    {77, "COLUMNS", 1},   {78, "OFFSET", -1},     {82, "SEARCH", -1},    {83, "TRANSPOSE", 1},  {97, "ATAN2", 2},
    {98, "ASIN", 1},      {99, "ACOS", 1},        {100, "CHOOSE", -1},   {101, "HLOOKUP", -1},  {102, "VLOOKUP", -1},
    {105, "ISREF", 1},    {109, "LOG", -1},       {111, "CHAR", 1},      {112, "LOWER", 1},     {113, "UPPER", 1},
    {114, "PROPER", 1},   {115, "LEFT", -1},      {116, "RIGHT", -1},    {117, "EXACT", 2},     {118, "TRIM", 1},
    {119, "REPLACE", 4},  {120, "SUBSTITUTE", -1}, {121, "CODE", 1},     {124, "FIND", -1},     {125, "CELL", -1},
    {126, "ISERR", 1},    {127, "ISTEXT", 1},     {128, "ISNUMBER", 1},  {129, "ISBLANK", 1},   {130, "T", 1},
    {131, "N", 1},        {140, "DATEVALUE", 1},  {141, "TIMEVALUE", 1}, {142, "SLN", 3},       {148, "INDIRECT", -1},
    {162, "CLEAN", 1},    {167, "IPMT", -1},      {168, "PPMT", -1},     {169, "COUNTA", -1},   {183, "PRODUCT", -1},
    {184, "FACT", 1},     {190, "ISNONTEXT", 1},  {193, "STDEVP", -1},   {194, "VARP", -1},     {197, "TRUNC", -1},
    {198, "ISLOGICAL", 1}, {212, "ROUNDUP", 2},   {213, "ROUNDDOWN", 2}, {216, "RANK", -1},     {219, "ADDRESS", -1},
    {220, "DAYS360", -1}, {221, "TODAY", 0},      {227, "MEDIAN", -1},   {228, "SUMPRODUCT", -1}, {229, "SINH", 1},
    {230, "COSH", 1},     {231, "TANH", 1},       {269, "AVEDEV", -1},   {276, "COMBIN", 2},    {279, "EVEN", 1},
    {285, "FLOOR", 2},    {288, "CEILING", 2},    {298, "ODD", 1},       {300, "POISSON", 3},   {313, "SLOPE", 2},
    {336, "CONCATENATE", -1}, {337, "POWER", 2},  {342, "RADIANS", 1},   {343, "DEGREES", 1},   {344, "SUBTOTAL", -1},
    {345, "SUMIF", -1},   {346, "COUNTIF", 2},    {347, "COUNTBLANK", 1}, {354, "ROMAN", -1},   {359, "HYPERLINK", -1},
    {361, "AVERAGEA", -1}, {362, "MAXA", -1},     {363, "MINA", -1},
};

const FunctionInfo* find_function(std::uint16_t id) {
  for (const FunctionInfo& f : functions) {
    if (f.id == id) return &f;
  }
  return nullptr;
}

std::string ref_text(std::uint16_t row, std::uint16_t column_field) {
  const bool column_relative = (column_field & 0x4000u) != 0;
  const bool row_relative = (column_field & 0x8000u) != 0;
  std::string out;
  if (!column_relative) out.push_back('$');
  out += column_name(column_field & 0x3FFFu);
  if (!row_relative) out.push_back('$');
  out += std::to_string(std::uint32_t{row} + 1);
  return out;
}

}  // namespace

std::optional<std::string> decompile_biff8_formula(ByteView tokens, std::uint32_t, std::uint32_t) {
  util::ByteReader r(tokens);
  std::vector<std::string> stack;
  const auto pop = [&](std::string& out) {
    if (stack.empty()) return false;
    out = std::move(stack.back());
    stack.pop_back();
    return true;
  };
  static constexpr std::string_view binary[] = {"+", "-", "*", "/", "^", "&", "<", "<=", "=", ">=", ">", "<>", " ", ",", ":"};
  while (!r.at_end()) {
    const std::uint8_t ptg = r.u8();
    if (stack.size() > 1024) return std::nullopt;
    if (ptg >= 0x03 && ptg <= 0x11) {
      std::string b;
      std::string a;
      if (!pop(b) || !pop(a)) return std::nullopt;
      stack.push_back(a + std::string(binary[ptg - 0x03]) + b);
      continue;
    }
    switch (ptg) {
      case 0x12:
      case 0x13:
      case 0x14:
      case 0x15: {
        std::string a;
        if (!pop(a)) return std::nullopt;
        stack.push_back(ptg == 0x12 ? "+" + a : ptg == 0x13 ? "-" + a : ptg == 0x14 ? a + "%" : "(" + a + ")");
        continue;
      }
      case 0x16: stack.emplace_back(); continue;
      case 0x17: {
        const std::uint8_t count = r.u8();
        const std::uint8_t flags = r.u8();
        std::u16string units;
        for (std::uint8_t i = 0; i < count; ++i) units.push_back((flags & 1u) != 0 ? static_cast<char16_t>(r.u16le()) : static_cast<char16_t>(r.u8()));
        if (!r.ok()) return std::nullopt;
        stack.push_back("\"" + util::replace_all(util::utf16_to_utf8(units), "\"", "\"\"") + "\"");
        continue;
      }
      case 0x19: {
        const std::uint8_t flags = r.u8();
        const std::uint16_t data = r.u16le();
        if ((flags & 0x04u) != 0) r.skip((std::size_t{data} + 1) * 2);  // CHOOSE jump table
        if ((flags & 0x10u) != 0) {
          std::string a;
          if (!pop(a)) return std::nullopt;
          stack.push_back("SUM(" + a + ")");
        }
        continue;
      }
      case 0x1C: stack.emplace_back(excel_error_text(r.u8())); continue;
      case 0x1D: stack.emplace_back(r.u8() != 0 ? "TRUE" : "FALSE"); continue;
      case 0x1E: stack.push_back(std::to_string(r.u16le())); continue;
      case 0x1F: stack.push_back(format_number(r.f64le())); continue;
      default: break;
    }
    if ((ptg & 0x60u) == 0) return std::nullopt;  // ptgExp, ptgTbl, extended tokens
    const std::uint8_t base = static_cast<std::uint8_t>((ptg & 0x1Fu) | 0x20u);
    switch (base) {
      case 0x21:
      case 0x22: {
        int count = 0;
        std::uint16_t id = 0;
        if (base == 0x22) {
          count = r.u8() & 0x7F;
          id = r.u16le() & 0x7FFFu;
        } else {
          id = r.u16le();
        }
        const FunctionInfo* info = find_function(id);
        if (info == nullptr) return std::nullopt;
        if (base == 0x21) {
          if (info->arguments < 0) return std::nullopt;
          count = info->arguments;
        }
        if (static_cast<std::size_t>(count) > stack.size()) return std::nullopt;
        std::string args;
        for (std::size_t i = stack.size() - static_cast<std::size_t>(count); i < stack.size(); ++i) {
          if (!args.empty() || i > stack.size() - static_cast<std::size_t>(count)) args += ",";
          args += stack[i];
        }
        stack.resize(stack.size() - static_cast<std::size_t>(count));
        stack.push_back(std::string(info->name) + "(" + args + ")");
        continue;
      }
      case 0x24: {
        const std::uint16_t row = r.u16le();
        const std::uint16_t column = r.u16le();
        stack.push_back(ref_text(row, column));
        continue;
      }
      case 0x25: {
        const std::uint16_t row1 = r.u16le();
        const std::uint16_t row2 = r.u16le();
        const std::uint16_t column1 = r.u16le();
        const std::uint16_t column2 = r.u16le();
        stack.push_back(ref_text(row1, column1) + ":" + ref_text(row2, column2));
        continue;
      }
      case 0x2A: r.skip(4); stack.emplace_back("#REF!"); continue;
      case 0x2B: r.skip(8); stack.emplace_back("#REF!"); continue;
      default: return std::nullopt;
    }
  }
  if (!r.ok() || stack.size() != 1) return std::nullopt;
  return stack.front();
}

// --------------------------------------------------------------------------- records

namespace {

struct BiffRecord {
  std::uint16_t type = 0;
  ByteView data;
  std::size_t offset = 0;  // of the record header in the stream
};

class BiffStream {
 public:
  explicit BiffStream(ByteView data) : data_(data) {}
  void seek(std::size_t position) { position_ = position; }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  std::optional<BiffRecord> next() {
    if (position_ + 4 > data_.size()) return std::nullopt;
    util::ByteReader r(data_.subspan(position_));
    BiffRecord record;
    record.offset = position_;
    record.type = r.u16le();
    const std::uint16_t length = r.u16le();
    if (length > r.remaining()) {
      truncated_ = true;
      return std::nullopt;
    }
    record.data = r.bytes(length);
    position_ += 4 + length;
    return record;
  }
  /// The next record if it is a CONTINUE, without consuming anything else.
  std::optional<ByteView> next_continue() {
    const std::size_t saved = position_;
    auto record = next();
    if (record && record->type == biff::continue_record) return record->data;
    position_ = saved;
    return std::nullopt;
  }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }

 private:
  ByteView data_;
  std::size_t position_ = 0;
  bool truncated_ = false;
};

// Reads across a record and its CONTINUE records. Strings that cross a
// boundary restart with a fresh option byte telling the character width.
class SegmentReader {
 public:
  explicit SegmentReader(std::vector<ByteView> segments) : segments_(std::move(segments)) {}
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] bool at_end() {
    settle();
    return segment_ >= segments_.size();
  }
  std::uint8_t u8() {
    settle();
    if (segment_ >= segments_.size()) {
      ok_ = false;
      return 0;
    }
    return std::to_integer<std::uint8_t>(segments_[segment_][position_++]);
  }
  std::uint16_t u16() {
    const std::uint16_t lo = u8();
    return static_cast<std::uint16_t>(lo | (u8() << 8));
  }
  std::uint32_t u32() {
    const std::uint32_t lo = u16();
    return lo | (static_cast<std::uint32_t>(u16()) << 16);
  }
  void skip(std::uint64_t count) {
    while (count > 0) {
      settle();
      if (segment_ >= segments_.size()) {
        ok_ = false;
        return;
      }
      const std::size_t available = segments_[segment_].size() - position_;
      const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(available, count));
      position_ += take;
      count -= take;
    }
  }
  /// `count` characters, `high_byte` telling the width of the first run.
  std::string characters(std::uint32_t count, bool high_byte) {
    std::u16string units;
    units.reserve(std::min<std::uint32_t>(count, 65536));
    while (count > 0) {
      if (segment_ < segments_.size() && position_ >= segments_[segment_].size()) {
        ++segment_;
        position_ = 0;
        if (segment_ >= segments_.size()) break;
        high_byte = (std::to_integer<std::uint8_t>(segments_[segment_][position_++]) & 1u) != 0;
        continue;
      }
      if (segment_ >= segments_.size()) break;
      const ByteView seg = segments_[segment_];
      if (high_byte) {
        if (position_ + 2 > seg.size()) {
          position_ = seg.size();
          continue;
        }
        units.push_back(static_cast<char16_t>(std::to_integer<std::uint8_t>(seg[position_]) |
                                              (std::to_integer<std::uint8_t>(seg[position_ + 1]) << 8)));
        position_ += 2;
      } else {
        units.push_back(static_cast<char16_t>(std::to_integer<std::uint8_t>(seg[position_])));
        position_ += 1;
      }
      --count;
    }
    if (count > 0) ok_ = false;
    return util::utf16_to_utf8(units);
  }

 private:
  void settle() {
    while (segment_ < segments_.size() && position_ >= segments_[segment_].size()) {
      ++segment_;
      position_ = 0;
    }
  }
  std::vector<ByteView> segments_;
  std::size_t segment_ = 0;
  std::size_t position_ = 0;
  bool ok_ = true;
};

// XLUnicodeString (BIFF8: u16 count, option byte, characters) within one record.
std::optional<std::string> unicode_string(util::ByteReader& r, bool short_count) {
  const std::uint32_t count = short_count ? r.u8() : r.u16le();
  const std::uint8_t flags = r.u8();
  if (!r.ok()) return std::nullopt;
  // Rich-text runs and phonetic data follow the characters; nothing after
  // the string is read, so only their headers need skipping.
  if ((flags & 0x08u) != 0) (void)r.u16le();
  if ((flags & 0x04u) != 0) (void)r.u32le();
  std::u16string units;
  units.reserve(count);
  for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
    units.push_back((flags & 1u) != 0 ? static_cast<char16_t>(r.u16le()) : static_cast<char16_t>(r.u8()));
  }
  if (!r.ok()) return std::nullopt;
  return util::utf16_to_utf8(units);
}

// BIFF5 byte string in the workbook code page.
std::optional<std::string> byte_string(util::ByteReader& r, bool short_count, const CodePageInfo& page) {
  const std::uint32_t count = short_count ? r.u8() : r.u16le();
  const ByteView bytes = r.bytes(count);
  if (!r.ok()) return std::nullopt;
  return decode_code_page(bytes, page);
}

struct SheetEntry {
  std::string name;
  std::uint32_t offset = 0;
  ir::SheetVisibility visibility = ir::SheetVisibility::Visible;
  std::uint8_t type = 0;  // 0 worksheet, 1 macro sheet, 2 chart, 6 VB module
};

struct Globals {
  bool biff8 = true;
  bool date1904 = false;
  const CodePageInfo* page = nullptr;
  std::map<std::uint16_t, std::string> formats;
  std::vector<std::uint16_t> xf_formats;
  std::vector<std::string> strings;
  std::vector<SheetEntry> sheets;
};

Result<std::vector<std::string>> read_sst(ByteView first, BiffStream& stream, CellBudget& budget) {
  std::vector<ByteView> segments{first};
  while (const auto more = stream.next_continue()) segments.push_back(*more);
  SegmentReader r(std::move(segments));
  (void)r.u32();  // total references
  const std::uint32_t unique = r.u32();
  if (!r.ok()) return fail(Status::Decode, "the shared string table is truncated");
  std::vector<std::string> strings;
  for (std::uint32_t i = 0; i < unique && !r.at_end(); ++i) {
    if ((i & 4095) == 0) CVT_TRY(budget.take(4096));
    const std::uint16_t count = r.u16();
    const std::uint8_t flags = r.u8();
    std::uint16_t runs = 0;
    std::uint32_t extended = 0;
    if ((flags & 0x08u) != 0) runs = r.u16();
    if ((flags & 0x04u) != 0) extended = r.u32();
    std::string text = r.characters(count, (flags & 1u) != 0);
    r.skip(std::uint64_t{runs} * 4 + extended);
    if (!r.ok()) break;
    strings.push_back(std::move(text));
  }
  return strings;
}

Result<Globals> read_globals(ByteView stream_bytes, CellBudget& budget, const ReadContext& ctx) {
  Globals globals;
  BiffStream stream(stream_bytes);
  const auto bof = stream.next();
  if (!bof || (bof->type != biff::bof && bof->type != 0x0209 && bof->type != 0x0409)) {
    return fail(Status::Decode, "the workbook stream does not start with a BOF record");
  }
  util::ByteReader b(bof->data);
  const std::uint16_t version = b.u16le();
  const std::uint16_t kind = b.u16le();
  if (bof->type != biff::bof) return fail(Status::Decode, "BIFF2-4 workbooks are not supported");
  globals.biff8 = version == 0x0600;
  if (!globals.biff8 && version != 0x0500) return fail(Status::Decode, "unsupported BIFF version 0x{:04X}", version);
  if (kind != 0x0005) return fail(Status::Decode, "the workbook stream does not start with the workbook globals");
  globals.page = find_code_page(1252);
  while (const auto record = stream.next()) {
    util::ByteReader r(record->data);
    switch (record->type) {
      case biff::eof: return globals;
      case biff::filepass: return fail(Status::Decode, "the workbook is password-protected (encrypted); it cannot be read");
      case biff::codepage: {
        const std::uint16_t page = r.u16le();
        if (page == 1200) break;  // UTF-16
        if (const CodePageInfo* info = find_code_page(page == 32768 ? 10000 : page)) globals.page = info;
        break;
      }
      case biff::datemode: globals.date1904 = r.u16le() == 1; break;
      case biff::format: {
        const std::uint16_t id = r.u16le();
        auto code = globals.biff8 ? unicode_string(r, false) : byte_string(r, true, *globals.page);
        if (code) globals.formats[id] = std::move(*code);
        break;
      }
      case biff::xf: {
        (void)r.u16le();
        const std::uint16_t format = r.u16le();
        if (globals.xf_formats.size() < 65536) globals.xf_formats.push_back(format);
        break;
      }
      case biff::sst: {
        if (!globals.biff8) break;
        CVT_TRY_ASSIGN(globals.strings, read_sst(record->data, stream, budget));
        break;
      }
      case biff::boundsheet: {
        SheetEntry entry;
        entry.offset = r.u32le();
        const std::uint8_t state = r.u8();
        entry.type = r.u8();
        entry.visibility = (state & 3u) == 1 ? ir::SheetVisibility::Hidden
                           : (state & 3u) == 2 ? ir::SheetVisibility::VeryHidden
                                                 : ir::SheetVisibility::Visible;
        auto name = globals.biff8 ? unicode_string(r, true) : byte_string(r, true, *globals.page);
        if (!name) return fail(Status::Decode, "damaged sheet record");
        entry.name = std::move(*name);
        CVT_TRY(ctx.limits.check_archive_entries(globals.sheets.size() + 1));
        globals.sheets.push_back(std::move(entry));
        break;
      }
      default: break;
    }
  }
  if (stream.truncated()) return fail(Status::Decode, "the workbook stream is truncated");
  return globals;
}

struct XfInfo {
  FormatKind kind = FormatKind::General;
  std::string code;
};

Result<void> read_sheet(ByteView stream_bytes, const SheetEntry& entry, const Globals& globals,
                        const std::vector<XfInfo>& xfs, ir::Sheet& sheet, CellBudget& budget, const ReadContext& ctx,
                        bool& had_unknown_formula) {
  BiffStream stream(stream_bytes);
  stream.seek(entry.offset);
  const auto bof = stream.next();
  if (!bof || bof->type != biff::bof) return fail(Status::Decode, "sheet \"{}\" does not start with a BOF record", entry.name);
  if (entry.type != 0) return {};  // charts, macro and VB sheets hold no cells we can show
  const auto put = [&](std::uint32_t row, std::uint32_t column, std::uint16_t xf_index, ir::Cell cell) -> Result<void> {
    const bool numeric = cell.type == ir::CellType::Number || cell.type == ir::CellType::DateTime;
    CVT_TRY(put_cell(sheet, row, column, std::move(cell), budget));
    if (numeric && xf_index < xfs.size() && !xfs[xf_index].code.empty() && xfs[xf_index].kind != FormatKind::General) {
      sheet.number_formats[{row, column}] = xfs[xf_index].code;
    }
    return {};
  };
  const auto number = [&](double value, std::uint16_t xf_index) {
    const FormatKind kind = xf_index < xfs.size() ? xfs[xf_index].kind : FormatKind::General;
    return spreadsheet_number(value, kind, globals.date1904);
  };
  std::optional<std::pair<std::uint32_t, std::uint32_t>> pending_string;  // FORMULA waiting for its STRING
  std::uint16_t pending_xf = 0;
  int depth = 1;  // embedded BOF/EOF pairs (charts inside sheets)
  std::size_t count = 0;
  bool frozen = false;
  std::uint16_t pane_x = 0;
  std::uint16_t pane_y = 0;
  while (const auto record = stream.next()) {
    if ((++count & 16383) == 0) CVT_TRY(ctx.checkpoint(0.5));
    util::ByteReader r(record->data);
    const std::uint16_t type = record->type;
    if (type == biff::bof) {
      ++depth;
      continue;
    }
    if (type == biff::eof) {
      if (--depth == 0) break;
      continue;
    }
    if (depth > 1) continue;
    if (type == biff::string && pending_string) {
      auto text = globals.biff8 ? unicode_string(r, false) : byte_string(r, false, *globals.page);
      if (text) CVT_TRY(put(pending_string->first, pending_string->second, pending_xf, ir::Cell::string(std::move(*text))));
      pending_string.reset();
      continue;
    }
    switch (type) {
      case biff::number: {
        const std::uint16_t row = r.u16le();
        const std::uint16_t column = r.u16le();
        const std::uint16_t xf_index = r.u16le();
        const double value = r.f64le();
        if (r.ok()) CVT_TRY(put(row, column, xf_index, number(value, xf_index)));
        break;
      }
      case biff::rk: {
        const std::uint16_t row = r.u16le();
        const std::uint16_t column = r.u16le();
        const std::uint16_t xf_index = r.u16le();
        const std::uint32_t rk = r.u32le();
        if (r.ok()) CVT_TRY(put(row, column, xf_index, number(decode_rk(rk), xf_index)));
        break;
      }
      case biff::mulrk: {
        const std::uint16_t row = r.u16le();
        std::uint16_t column = r.u16le();
        const std::size_t items = record->data.size() >= 6 ? (record->data.size() - 6) / 6 : 0;
        for (std::size_t i = 0; i < items; ++i, ++column) {
          const std::uint16_t xf_index = r.u16le();
          const std::uint32_t rk = r.u32le();
          if (!r.ok()) break;
          CVT_TRY(put(row, column, xf_index, number(decode_rk(rk), xf_index)));
        }
        break;
      }
      case biff::labelsst: {
        const std::uint16_t row = r.u16le();
        const std::uint16_t column = r.u16le();
        const std::uint16_t xf_index = r.u16le();
        const std::uint32_t index = r.u32le();
        if (r.ok() && index < globals.strings.size()) CVT_TRY(put(row, column, xf_index, ir::Cell::string(globals.strings[index])));
        break;
      }
      case biff::label:
      case biff::rstring: {
        const std::uint16_t row = r.u16le();
        const std::uint16_t column = r.u16le();
        const std::uint16_t xf_index = r.u16le();
        auto text = globals.biff8 ? unicode_string(r, false) : byte_string(r, false, *globals.page);
        if (text && !text->empty()) CVT_TRY(put(row, column, xf_index, ir::Cell::string(std::move(*text))));
        break;
      }
      case biff::boolerr: {
        const std::uint16_t row = r.u16le();
        const std::uint16_t column = r.u16le();
        const std::uint16_t xf_index = r.u16le();
        const std::uint8_t value = r.u8();
        const std::uint8_t error = r.u8();
        if (!r.ok()) break;
        CVT_TRY(put(row, column, xf_index,
                    error != 0 ? ir::Cell::string(std::string(excel_error_text(value))) : ir::Cell::boolean(value != 0)));
        break;
      }
      case biff::formula: {
        const std::uint16_t row = r.u16le();
        const std::uint16_t column = r.u16le();
        const std::uint16_t xf_index = r.u16le();
        const ByteView value = r.bytes(8);
        (void)r.u16le();  // flags
        (void)r.u32le();  // chn
        if (!r.ok()) break;
        if (globals.biff8) {
          const std::uint16_t size = r.u16le();
          const ByteView tokens = r.bytes(size);
          if (r.ok()) {
            if (auto text = decompile_biff8_formula(tokens, row, column)) {
              sheet.formulas[{row, column}] = std::move(*text);
            } else {
              had_unknown_formula = true;
            }
          }
        }
        util::ByteReader v(value);
        const auto b = [&](std::size_t i) { return std::to_integer<std::uint8_t>(value[i]); };
        if (b(6) == 0xFF && b(7) == 0xFF) {
          switch (b(0)) {
            case 0:  // string result in the following STRING record
              pending_string = std::pair<std::uint32_t, std::uint32_t>{row, column};
              pending_xf = xf_index;
              break;
            case 1: CVT_TRY(put(row, column, xf_index, ir::Cell::boolean(b(2) != 0))); break;
            case 2: CVT_TRY(put(row, column, xf_index, ir::Cell::string(std::string(excel_error_text(b(2)))))); break;
            case 3: CVT_TRY(put(row, column, xf_index, ir::Cell::string(""))); break;
            default: break;
          }
        } else {
          CVT_TRY(put(row, column, xf_index, number(v.f64le(), xf_index)));
        }
        break;
      }
      case biff::mergedcells: {
        const std::uint16_t count_ranges = r.u16le();
        for (std::uint16_t i = 0; i < count_ranges && r.ok(); ++i) {
          ir::CellRange range;
          range.first_row = r.u16le();
          range.last_row = r.u16le();
          range.first_column = r.u16le();
          range.last_column = r.u16le();
          if (r.ok() && range.first_row <= range.last_row && range.first_column <= range.last_column &&
              sheet.merged.size() < 100'000) {
            sheet.merged.push_back(range);
          }
        }
        break;
      }
      case biff::colinfo: {
        const std::uint16_t first = r.u16le();
        const std::uint16_t last = r.u16le();
        const std::uint16_t width = r.u16le();
        if (r.ok() && first <= last && last < 256 && width > 0) {
          if (sheet.column_widths.size() <= last) sheet.column_widths.resize(std::size_t{last} + 1, 0.0);
          for (std::uint32_t c = first; c <= last; ++c) sheet.column_widths[c] = std::round(width / 256.0 * 100.0) / 100.0;
        }
        break;
      }
      case biff::window2: {
        const std::uint16_t flags = r.u16le();
        frozen = r.ok() && (flags & 0x0008u) != 0;
        break;
      }
      case biff::pane: {
        pane_x = r.u16le();
        pane_y = r.u16le();
        break;
      }
      default: break;
    }
  }
  if (frozen) {
    sheet.frozen_columns = pane_x;
    sheet.frozen_rows = pane_y;
  }
  return {};
}

Result<AnyIR> read_xls(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const CfbReader cfb, CfbReader::open(ctx.input, ctx.limits));
  if (cfb.contains("EncryptedPackage")) {
    return fail(Status::Decode, "this is a password-protected (encrypted) Excel workbook; it cannot be read");
  }
  auto stream_index = cfb.find("Workbook");
  if (!stream_index) stream_index = cfb.find("Book");
  if (!stream_index) return fail(Status::Decode, "the compound file holds no Excel workbook stream");
  CVT_TRY_ASSIGN(const Bytes stream, cfb.read(*stream_index));
  CellBudget budget(ctx.limits);
  CVT_TRY_ASSIGN(const Globals globals, read_globals(stream, budget, ctx));
  std::vector<XfInfo> xfs;
  for (const std::uint16_t format : globals.xf_formats) {
    XfInfo info;
    const auto it = globals.formats.find(format);
    info.code = it != globals.formats.end() ? it->second : std::string(builtin_number_format(format));
    info.kind = classify_number_format(info.code);
    xfs.push_back(std::move(info));
  }
  ir::Table table;
  bool unknown_formula = false;
  for (std::size_t i = 0; i < globals.sheets.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(globals.sheets.size())));
    const SheetEntry& entry = globals.sheets[i];
    ir::Sheet sheet;
    sheet.name = entry.name;
    sheet.visibility = entry.visibility;
    if (entry.offset >= stream.size()) {
      ctx.warn(std::format("Sheet \"{}\" points outside the workbook stream and was left empty.", entry.name));
    } else {
      CVT_TRY(read_sheet(stream, entry, globals, xfs, sheet, budget, ctx, unknown_formula));
    }
    trim_sheet(sheet);
    table.sheets.push_back(std::move(sheet));
  }
  guess_headers(table);
  if (unknown_formula) ctx.warn("Some formulas could not be translated; their cached values were kept.");
  return table;
}

int sniff_xls(ByteView data, std::string_view) {
  if (data.size() < 512 || !as_chars(data).starts_with("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1")) return 0;
  Limits limits;
  limits.max_decompressed_bytes = 16 * 1024 * 1024;
  auto cfb = CfbReader::open(data, limits);
  if (!cfb) return 0;
  return cfb->contains("Workbook") || cfb->contains("Book") ? 98 : 0;  // beats the shared OLE2 magic of .doc/.ppt
}

}  // namespace

void register_xls(Registry& registry) {
  FormatInfo info;
  info.id = "xls";
  info.name = "Excel 97-2003 workbook";
  info.description = "Binary Excel workbook (BIFF8), as written by Excel 97 to 2003.";
  info.category = Category::Spreadsheet;
  info.extensions = {"xls", "xlt"};
  info.mime = "application/vnd.ms-excel";
  info.popularity = 55;
  info.sniff = sniff_xls;
  registry.add_format(std::move(info));

  Codec codec;
  codec.format_id = "xls";
  codec.kind = Kind::Table;
  codec.read = read_xls;
  codec.write = write_xls;
  codec.write_options = {
      OptionSpec::boolean("header_style", "Style the header row", true,
                          "Make the header row bold with a light fill and a rule below it."),
      OptionSpec::boolean("freeze_header", "Freeze the header row", false, "Keep the first row visible while scrolling."),
      OptionSpec::boolean("auto_column_widths", "Fit column widths", true,
                          "Size columns to their content when the source does not specify widths."),
      options::strip_metadata(),
  };
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::table
