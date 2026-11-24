// Convertible core - table family: Excel 97-2003 (BIFF8) workbook writer.
//
// Writes a Workbook stream inside a compound file: globals (code page
// UTF-16, window, fonts, number formats, the 15 style XFs Excel expects plus
// our cell XFs, the Normal style, date system, sheet directory, shared
// string table split over CONTINUE records with its EXTSST index) and per
// sheet DIMENSIONS, WINDOW2/PANE, COLINFO, ROW and cell records (NUMBER,
// LABELSST, BOOLERR) and MERGEDCELLS. Formulas are not compiled to BIFF
// tokens: their cached values are written and a warning says so. No
// summary-information streams (author, dates) are written.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/registry.hpp"
#include "families/table/cfb.hpp"
#include "families/table/common.hpp"
#include "families/table/spreadsheet.hpp"
#include "families/table/xls.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

constexpr std::uint32_t max_rows = 65'536;
constexpr std::uint32_t max_columns = 256;
constexpr std::size_t max_record_data = 8224;
constexpr std::uint16_t xf_default = 15;
constexpr std::uint16_t xf_header = 16;
constexpr std::uint16_t xf_wrap = 17;
constexpr std::uint16_t first_format_xf = 18;

void record(util::ByteWriter& out, std::uint16_t type, ByteView data) {
  out.u16le(type);
  out.u16le(static_cast<std::uint16_t>(data.size()));
  out.bytes(data);
}

// Characters of an XLUnicodeString: compressed (Latin-1) when possible.
struct EncodedChars {
  bool wide = false;
  std::u16string units;
};

EncodedChars encode_chars(std::string_view utf8) {
  EncodedChars out;
  out.units = util::utf8_to_utf16(utf8);
  for (const char16_t unit : out.units) out.wide = out.wide || unit > 0xFF;
  return out;
}

void put_chars(util::ByteWriter& out, const EncodedChars& chars) {
  for (const char16_t unit : chars.units) {
    if (chars.wide) {
      out.u16le(static_cast<std::uint16_t>(unit));
    } else {
      out.u8(static_cast<std::uint8_t>(unit));
    }
  }
}

// XLUnicodeString with a 16-bit (or 8-bit when `short_count`) length.
void put_string(util::ByteWriter& out, std::string_view utf8, bool short_count) {
  EncodedChars chars = encode_chars(utf8);
  const std::size_t limit = short_count ? 255 : 65535;
  if (chars.units.size() > limit) chars.units.resize(limit);
  if (short_count) {
    out.u8(static_cast<std::uint8_t>(chars.units.size()));
  } else {
    out.u16le(static_cast<std::uint16_t>(chars.units.size()));
  }
  out.u8(chars.wide ? 1 : 0);
  put_chars(out, chars);
}

// Writes the SST over as many CONTINUE records as needed and records where
// each bucket of strings starts for EXTSST.
struct SstWriter {
  util::ByteWriter& out;
  std::size_t stream_base;  // offset of `out` in the stream
  std::vector<std::uint8_t> current;  // data of the record being filled
  bool first = true;
  std::vector<std::pair<std::uint32_t, std::uint16_t>> buckets;  // (stream offset, offset in record)

  void flush() {
    record(out, first ? biff::sst : biff::continue_record, as_bytes(std::span<const std::uint8_t>(current)));
    first = false;
    current.clear();
  }
  void u8(std::uint8_t v) { current.push_back(v); }
  void u16(std::uint16_t v) {
    current.push_back(static_cast<std::uint8_t>(v));
    current.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    u16(static_cast<std::uint16_t>(v));
    u16(static_cast<std::uint16_t>(v >> 16));
  }
  void string(std::string_view utf8, bool bucket_start) {
    EncodedChars chars = encode_chars(utf8);
    if (chars.units.size() > 32767) chars.units.resize(32767);
    // Keep the 3-byte header and at least one character together.
    if (current.size() + 3 + (chars.wide ? 2 : 1) > max_record_data) flush();
    if (bucket_start) {
      const std::size_t record_start = stream_base + out.size();
      buckets.emplace_back(static_cast<std::uint32_t>(record_start + 4 + current.size()),
                           static_cast<std::uint16_t>(4 + current.size()));
    }
    u16(static_cast<std::uint16_t>(chars.units.size()));
    u8(chars.wide ? 1 : 0);
    const std::size_t width = chars.wide ? 2 : 1;
    for (const char16_t unit : chars.units) {
      if (current.size() + width > max_record_data) {
        flush();
        u8(chars.wide ? 1 : 0);  // the continuation restarts with the option byte
      }
      if (chars.wide) {
        u16(static_cast<std::uint16_t>(unit));
      } else {
        u8(static_cast<std::uint8_t>(unit));
      }
    }
  }
};

struct XlsSheetPlan {
  const ir::Sheet* sheet = nullptr;
  std::string name;
  std::uint32_t rows = 0;
  std::uint32_t columns = 0;
};

}  // namespace

Result<Bytes> write_xls(const AnyIR& value, WriteContext& ctx) {
  const ir::Table& input = value.get<ir::Table>();
  ir::Table blank;
  blank.sheets.emplace_back();
  const ir::Table& table = input.sheets.empty() ? blank : input;
  const bool header_style = ctx.options.get_bool("header_style", true);
  const bool freeze_header = ctx.options.get_bool("freeze_header", false);
  const bool auto_widths = ctx.options.get_bool("auto_column_widths", true);
  const std::vector<std::string> names = unique_sheet_names(table, 31, "[]:*?/\\");

  // Shared strings, number formats and cell XFs.
  std::unordered_map<std::string, std::uint32_t> string_index;
  std::vector<const std::string*> strings;
  std::uint32_t string_references = 0;
  std::map<std::string, std::uint16_t> format_xf;   // format code -> XF index
  std::vector<std::pair<std::uint16_t, std::string>> custom_formats;  // (id, code)
  std::vector<std::uint16_t> format_ids;            // numFmt id per extra XF
  std::uint16_t next_format_id = 164;
  std::size_t truncated = 0;
  std::size_t formulas = 0;
  std::size_t early_dates = 0;
  const auto intern = [&](const std::string& text) {
    ++string_references;
    const auto [it, inserted] = string_index.emplace(text, static_cast<std::uint32_t>(strings.size()));
    if (inserted) strings.push_back(&it->first);
    return it->second;
  };
  const auto xf_for = [&](const std::string& code) -> std::uint16_t {
    if (code.empty()) return xf_default;
    if (const auto it = format_xf.find(code); it != format_xf.end()) return it->second;
    std::uint16_t id = 0;
    if (const auto builtin = builtin_number_format_id(code); builtin && *builtin < 50) {
      id = static_cast<std::uint16_t>(*builtin);
    } else {
      id = next_format_id++;
      custom_formats.emplace_back(id, code);
    }
    const auto xf = static_cast<std::uint16_t>(first_format_xf + format_ids.size());
    format_ids.push_back(id);
    format_xf.emplace(code, xf);
    return xf;
  };

  std::vector<XlsSheetPlan> plans;
  for (std::size_t i = 0; i < table.sheets.size(); ++i) {
    const ir::Sheet& sheet = table.sheets[i];
    XlsSheetPlan plan;
    plan.sheet = &sheet;
    plan.name = names[i];
    plan.rows = static_cast<std::uint32_t>(std::min<std::size_t>(sheet.rows.size(), max_rows));
    plan.columns = static_cast<std::uint32_t>(std::min<std::size_t>(sheet.column_count(), max_columns));
    if (sheet.rows.size() > max_rows || sheet.column_count() > max_columns) {
      ctx.warn(std::format("Sheet \"{}\" is larger than the {} rows x {} columns an .xls sheet holds; the rest was dropped.",
                           plan.name, max_rows, max_columns));
    }
    plans.push_back(plan);
  }

  // Per-sheet cell streams first (they need string indices), globals after.
  std::vector<Bytes> sheet_streams;
  for (std::size_t s = 0; s < plans.size(); ++s) {
    CVT_TRY(ctx.checkpoint(0.8 * static_cast<double>(s) / static_cast<double>(plans.size())));
    const XlsSheetPlan& plan = plans[s];
    const ir::Sheet& sheet = *plan.sheet;
    util::ByteWriter out;
    {
      util::ByteWriter b;
      b.u16le(0x0600).u16le(0x0010).u16le(0x0DBB).u16le(0x07CC).u32le(0).u32le(0x06);
      record(out, biff::bof, b.view());
    }
    {
      util::ByteWriter b;
      b.u32le(0).u32le(plan.rows).u16le(0).u16le(static_cast<std::uint16_t>(plan.columns)).u16le(0);
      record(out, biff::dimensions, b.view());
    }
    std::uint32_t frozen_rows = std::min<std::uint32_t>(sheet.frozen_rows, max_rows - 1);
    const std::uint32_t frozen_columns = std::min<std::uint32_t>(sheet.frozen_columns, max_columns - 1);
    if (freeze_header && plan.rows > 0) frozen_rows = std::max<std::uint32_t>(frozen_rows, 1);
    const bool frozen = frozen_rows > 0 || frozen_columns > 0;
    {
      util::ByteWriter b;
      std::uint16_t flags = 0x06B6;  // grid, headers, zeros, default header colour, outline symbols, paged
      if (s != 0) flags &= static_cast<std::uint16_t>(~0x0600u);  // only the first sheet is selected/active
      if (frozen) flags |= 0x0108;  // frozen, without split
      b.u16le(flags).u16le(0).u16le(0).u32le(64).u16le(0).u16le(0).u32le(0);
      record(out, biff::window2, b.view());
    }
    if (frozen) {
      util::ByteWriter b;
      const std::uint16_t active = frozen_rows > 0 && frozen_columns > 0 ? 0 : frozen_rows > 0 ? 2 : 1;
      b.u16le(static_cast<std::uint16_t>(frozen_columns)).u16le(static_cast<std::uint16_t>(frozen_rows));
      b.u16le(static_cast<std::uint16_t>(frozen_rows)).u16le(static_cast<std::uint16_t>(frozen_columns)).u16le(active);
      record(out, biff::pane, b.view());
    }
    // Column widths.
    std::vector<double> widths(plan.columns, 0.0);
    for (std::size_t c = 0; c < plan.columns && c < sheet.column_widths.size(); ++c) widths[c] = sheet.column_widths[c];
    if (auto_widths && sheet.column_widths.empty()) {
      const std::size_t sample = std::min<std::size_t>(sheet.rows.size(), 2000);
      for (std::size_t r = 0; r < sample; ++r) {
        for (std::size_t c = 0; c < sheet.rows[r].size() && c < plan.columns; ++c) {
          const ir::Cell& cell = sheet.rows[r][c];
          if (cell.empty()) continue;
          std::size_t width = 0;
          for (const std::string_view line : util::split_lines(cell.text)) width = std::max(width, display_width(line));
          if (cell.type == ir::CellType::DateTime) width = std::max<std::size_t>(width, 10);
          widths[c] = std::max(widths[c], std::clamp(static_cast<double>(width) + 2.0, 8.43, 80.0));
        }
      }
    }
    for (std::uint32_t c = 0; c < plan.columns; ++c) {
      if (widths[c] <= 0) continue;
      util::ByteWriter b;
      b.u16le(static_cast<std::uint16_t>(c)).u16le(static_cast<std::uint16_t>(c));
      b.u16le(static_cast<std::uint16_t>(std::min(widths[c], 255.0) * 256.0)).u16le(xf_default).u16le(0).u16le(0);
      record(out, biff::colinfo, b.view());
    }
    // Rows in blocks of 32: ROW records, then the cells of those rows.
    for (std::uint32_t block = 0; block < plan.rows; block += 32) {
      if ((block & 4095) == 0) CVT_TRY(ctx.checkpoint(0.8 * (static_cast<double>(s) + static_cast<double>(block) / plan.rows) / static_cast<double>(plans.size())));
      const std::uint32_t end = std::min(block + 32, plan.rows);
      for (std::uint32_t r = block; r < end; ++r) {
        const auto& row = sheet.rows[r];
        const auto width = static_cast<std::uint16_t>(std::min<std::size_t>(row.size(), plan.columns));
        if (width == 0) continue;
        util::ByteWriter b;
        b.u16le(static_cast<std::uint16_t>(r)).u16le(0).u16le(width).u16le(300).u16le(0).u16le(0).u32le(0x000F0100u);
        record(out, biff::row, b.view());
      }
      for (std::uint32_t r = block; r < end; ++r) {
        const auto& row = sheet.rows[r];
        for (std::uint32_t c = 0; c < row.size() && c < plan.columns; ++c) {
          const ir::Cell& cell = row[c];
          if (!sheet.formula_at(r, c).empty()) ++formulas;
          if (cell.empty()) continue;
          std::uint16_t xf = xf_default;
          if (header_style && sheet.has_header && r == 0) {
            xf = xf_header;
          } else if (cell.type == ir::CellType::String && cell.text.find('\n') != std::string::npos) {
            xf = xf_wrap;
          } else if (const std::string code = cell_number_format(sheet, r, c); !code.empty()) {
            xf = xf_for(code);
          }
          util::ByteWriter b;
          b.u16le(static_cast<std::uint16_t>(r)).u16le(static_cast<std::uint16_t>(c)).u16le(xf);
          const auto write_label = [&](const std::string& text) {
            std::string value = text;
            if (util::utf8_length(value) > 32767) {
              value = truncate_utf8(value, 32767);
              ++truncated;
            }
            b.u32le(intern(value));
            record(out, biff::labelsst, b.view());
          };
          switch (cell.type) {
            case ir::CellType::Empty: break;
            case ir::CellType::String: write_label(cell.text); break;
            case ir::CellType::Number:
              if (std::isfinite(cell.number)) {
                b.f64le(cell.number);
                record(out, biff::number, b.view());
              } else {
                b.u8(0x24).u8(1);
                record(out, biff::boolerr, b.view());
              }
              break;
            case ir::CellType::Bool:
              b.u8(cell.number != 0 ? 1 : 0).u8(0);
              record(out, biff::boolerr, b.view());
              break;
            case ir::CellType::DateTime: {
              const double serial = unix_to_excel_serial(cell.number, false);
              if (std::isfinite(serial) && serial >= 0.0 && serial < 2958466.0) {
                b.f64le(serial);
                record(out, biff::number, b.view());
              } else {
                ++early_dates;
                write_label(cell.text);
              }
              break;
            }
          }
        }
      }
      CVT_TRY(ctx.limits.check_output(out.size()));
    }
    // Merged cells, at most 1026 per record.
    std::vector<ir::CellRange> merges;
    for (const ir::CellRange& m : sheet.merged) {
      if (m.first_row <= m.last_row && m.first_column <= m.last_column && m.last_row < max_rows &&
          m.last_column < max_columns && (m.first_row != m.last_row || m.first_column != m.last_column)) {
        merges.push_back(m);
      }
    }
    for (std::size_t k = 0; k < merges.size(); k += 1026) {
      util::ByteWriter b;
      const std::size_t n = std::min<std::size_t>(1026, merges.size() - k);
      b.u16le(static_cast<std::uint16_t>(n));
      for (std::size_t i = 0; i < n; ++i) {
        const ir::CellRange& m = merges[k + i];
        b.u16le(static_cast<std::uint16_t>(m.first_row)).u16le(static_cast<std::uint16_t>(m.last_row));
        b.u16le(static_cast<std::uint16_t>(m.first_column)).u16le(static_cast<std::uint16_t>(m.last_column));
      }
      record(out, biff::mergedcells, b.view());
    }
    record(out, biff::eof, {});
    sheet_streams.push_back(out.take());
  }

  // Globals.
  util::ByteWriter globals;
  {
    util::ByteWriter b;
    b.u16le(0x0600).u16le(0x0005).u16le(0x0DBB).u16le(0x07CC).u32le(0).u32le(0x06);
    record(globals, biff::bof, b.view());
  }
  const auto simple = [&](std::uint16_t type, std::uint16_t v) {
    util::ByteWriter b;
    b.u16le(v);
    record(globals, type, b.view());
  };
  simple(0x00E1, 0x04B0);        // INTERFACEHDR: code page 1200
  simple(0x00C1, 0x0000);        // MMS
  record(globals, 0x00E2, {});   // INTERFACEEND
  simple(biff::codepage, 1200);  // UTF-16
  simple(0x0161, 0);             // DSF
  {
    util::ByteWriter b;  // TABID
    for (std::size_t i = 0; i < plans.size(); ++i) b.u16le(static_cast<std::uint16_t>(i + 1));
    record(globals, 0x013D, b.view());
  }
  simple(0x009C, 0x000E);  // FNGROUPCOUNT
  simple(0x0019, 0);       // WINDOWPROTECT
  simple(0x0012, 0);       // PROTECT
  simple(0x0013, 0);       // PASSWORD
  simple(0x01AF, 0);       // PROT4REV
  simple(0x01BC, 0);       // PROT4REVPASS
  {
    util::ByteWriter b;  // WINDOW1
    b.u16le(0).u16le(0).u16le(0x4000).u16le(0x2000).u16le(0x0038).u16le(0).u16le(0).u16le(1).u16le(0x0258);
    record(globals, biff::window1, b.view());
  }
  simple(0x0040, 0);             // BACKUP
  simple(0x008D, 0);             // HIDEOBJ
  simple(biff::datemode, 0);     // 1900 date system
  simple(0x000E, 1);             // PRECISION
  simple(0x01B7, 0);             // REFRESHALL
  simple(0x00DA, 0);             // BOOKBOOL
  // Fonts 0-3 regular, font 5 (index 4 is never used) bold.
  for (int f = 0; f < 5; ++f) {
    util::ByteWriter b;
    const bool bold = f == 4;
    b.u16le(220).u16le(0).u16le(0x7FFF).u16le(bold ? 700 : 400).u16le(0).u8(0).u8(0).u8(0).u8(0);
    put_string(b, "Arial", true);
    record(globals, biff::font, b.view());
  }
  for (const auto& [id, code] : custom_formats) {
    util::ByteWriter b;
    b.u16le(id);
    put_string(b, code, false);
    record(globals, biff::format, b.view());
  }
  const auto xf_record = [&](std::uint16_t font, std::uint16_t format, bool style_xf, std::uint8_t alignment,
                             std::uint32_t borders, std::uint32_t border_colors, std::uint16_t fill) {
    util::ByteWriter b;
    b.u16le(font).u16le(format);
    b.u16le(style_xf ? 0xFFF5 : 0x0001);
    b.u8(alignment).u8(0).u8(0);
    b.u8(style_xf ? 0xF4 : 0xFC);
    b.u32le(borders).u32le(border_colors).u16le(fill);
    record(globals, biff::xf, b.view());
  };
  for (int i = 0; i < 15; ++i) xf_record(0, 0, true, 0x20, 0, 0, 0x20C0);  // style XFs
  xf_record(0, 0, false, 0x20, 0, 0, 0x20C0);                             // 15: default cell
  if (header_style) {
    xf_record(5, 0, false, 0x20, 0x00001000u, 0x04000400u, 0x201F);  // 16: bold, light fill, thin bottom rule
  } else {
    xf_record(0, 0, false, 0x20, 0, 0, 0x20C0);
  }
  xf_record(0, 0, false, 0x08, 0, 0, 0x20C0);                             // 17: wrapped text, top aligned
  for (const std::uint16_t id : format_ids) xf_record(0, id, false, 0x20, 0, 0, 0x20C0);
  {
    util::ByteWriter b;  // STYLE: built-in Normal
    b.u16le(0x8000).u8(0).u8(0xFF);
    record(globals, biff::style, b.view());
  }
  simple(0x0160, 0);  // USESELFS
  std::vector<std::size_t> boundsheet_offsets;
  for (std::size_t i = 0; i < plans.size(); ++i) {
    util::ByteWriter b;
    b.u32le(0);  // patched below
    const ir::SheetVisibility visibility = i == 0 ? ir::SheetVisibility::Visible : plans[i].sheet->visibility;
    b.u8(visibility == ir::SheetVisibility::Hidden ? 1 : visibility == ir::SheetVisibility::VeryHidden ? 2 : 0).u8(0);
    put_string(b, plans[i].name, true);
    boundsheet_offsets.push_back(globals.size() + 4);
    record(globals, biff::boundsheet, b.view());
  }
  {
    util::ByteWriter b;  // COUNTRY
    b.u16le(1).u16le(1);
    record(globals, 0x008C, b.view());
  }
  // Shared strings.
  SstWriter sst{globals, 0, {}, true, {}};
  sst.u32(string_references);
  sst.u32(static_cast<std::uint32_t>(strings.size()));
  const std::size_t per_bucket = std::max<std::size_t>(8, (strings.size() + 1023) / 1024);
  for (std::size_t i = 0; i < strings.size(); ++i) sst.string(*strings[i], i % per_bucket == 0);
  sst.flush();
  {
    util::ByteWriter b;  // EXTSST
    b.u16le(static_cast<std::uint16_t>(per_bucket));
    for (const auto& [offset, in_record] : sst.buckets) b.u32le(offset).u16le(in_record).u16le(0);
    if (b.size() + 4 <= max_record_data) record(globals, biff::extsst, b.view());
  }
  record(globals, biff::eof, {});

  // Assemble: globals, then the sheets; patch the BOUNDSHEET offsets.
  Bytes stream = globals.take();
  std::size_t offset = stream.size();
  for (std::size_t i = 0; i < sheet_streams.size(); ++i) {
    const auto position = static_cast<std::uint32_t>(offset);
    for (int k = 0; k < 4; ++k) stream[boundsheet_offsets[i] + static_cast<std::size_t>(k)] = static_cast<std::byte>(position >> (8 * k));
    append(stream, sheet_streams[i]);
    offset = stream.size();
  }
  // Excel insists on a Workbook stream outside the mini stream.
  if (stream.size() < 4096) stream.resize(4096, std::byte{0});
  CVT_TRY(ctx.limits.check_output(stream.size()));

  if (formulas > 0) ctx.warn(std::format("{} formula(s) were written as their last calculated values.", formulas));
  if (truncated > 0) ctx.warn(std::format("{} text cell(s) were longer than 32,767 characters and were cut.", truncated));
  if (early_dates > 0) ctx.warn(std::format("{} date(s) before 1900 were written as text.", early_dates));

  CfbWriter cfb;
  cfb.set_root_clsid({0x20, 0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46});
  CVT_TRY(cfb.add_stream("Workbook", stream));
  return cfb.finish();
}

}  // namespace cvt::table
