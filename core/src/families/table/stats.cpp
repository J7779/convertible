// Convertible core - table family: data files of statistics packages (read).
//
//   * Stata .dta: format 113-115 (binary header) and 117-119 (tagged), both
//     byte orders; byte/int/long/float/double, fixed strings and strLs;
//     missing values; %tc/%tC date-times and %td dates.
//   * SPSS .sav / .zsav: the dictionary (variable records, long names,
//     character encoding) and data stored uncompressed, bytecode-compressed
//     or zlib-compressed; system-missing values; DATE, TIME and DATETIME
//     formats (seconds since 1582-10-14).
//   * SAS transport (XPORT version 5) .xpt: library, member and NAMESTR
//     headers, IBM System/370 floating point, character fields, missing
//     values, DATE/DATETIME/TIME formats (days or seconds since 1960-01-01).
// Each data set becomes a sheet whose first row holds the variable names.
// Value labels are not applied.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util/bytes.hpp"
#include "engine/registry.hpp"
#include "families/table/codepages.hpp"
#include "families/table/common.hpp"
#include "families/table/table.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::table {

namespace {

constexpr double unix_1960 = -315619200.0;     // 1960-01-01, the epoch of Stata and SAS
constexpr double unix_1582 = -12219379200.0;   // 1582-10-14, the epoch of SPSS

std::string file_stem(std::string_view filename) {
  if (const std::size_t slash = filename.find_last_of("/\\"); slash != std::string_view::npos) filename.remove_prefix(slash + 1);
  if (const std::size_t dot = filename.rfind('.'); dot != std::string_view::npos && dot > 0) filename = filename.substr(0, dot);
  return std::string(filename);
}

// substr that never throws: positions past the end give an empty view.
std::string_view slice(std::string_view text, std::size_t at, std::size_t length) {
  return at > text.size() ? std::string_view() : text.substr(at, length);
}

std::string c_string(ByteView bytes) {
  std::string_view text = as_chars(bytes);
  text = text.substr(0, text.find('\0'));
  return util::sanitize_utf8(text);
}

// --------------------------------------------------------------------------- Stata

struct Endian {
  bool little = true;
  [[nodiscard]] std::uint16_t u16(util::ByteReader& r) const { return little ? r.u16le() : r.u16be(); }
  [[nodiscard]] std::uint32_t u32(util::ByteReader& r) const { return little ? r.u32le() : r.u32be(); }
  [[nodiscard]] std::uint64_t u64(util::ByteReader& r) const { return little ? r.u64le() : r.u64be(); }
};

struct StataVariable {
  std::string name;
  std::uint32_t type = 0;  // normalised: 1..2045 strings, 32768 strL, 65526.. numbers (new numbering)
  std::string format;
};

std::size_t stata_width(std::uint32_t type) {
  if (type <= 2045) return type;
  switch (type) {
    case 32768: return 8;
    case 65526: return 8;
    case 65527: return 4;
    case 65528: return 4;
    case 65529: return 2;
    case 65530: return 1;
    default: return 0;
  }
}

ir::Cell stata_number(double value, const std::string& format) {
  if (format.starts_with("%tc") || format.starts_with("%tC")) return datetime_cell(unix_1960 + value / 1000.0, DateStyle::DateTime);
  if (format.starts_with("%td") || format.starts_with("%d")) return datetime_cell(unix_1960 + value * 86400.0, DateStyle::Date);
  return number_cell(value);
}

Result<AnyIR> read_stata(ReadContext& ctx) {
  const ByteView input = ctx.input;
  util::ByteReader r(input);
  Endian e;
  std::uint32_t release = 0;
  std::uint64_t nvar = 0;
  std::uint64_t nobs = 0;
  std::vector<StataVariable> vars;
  std::size_t data_offset = 0;
  std::map<std::pair<std::uint32_t, std::uint64_t>, std::string> strls;
  const bool tagged = as_chars(input).starts_with("<stata_dta>");
  CellBudget budget(ctx.limits);
  if (tagged) {
    const std::string_view text = as_chars(input);
    const auto tag_value = [&](std::string_view tag) -> std::string_view {
      const std::string open = "<" + std::string(tag) + ">";
      const std::string close = "</" + std::string(tag) + ">";
      const std::size_t a = text.find(open);
      if (a == std::string_view::npos) return {};
      const std::size_t b = text.find(close, a + open.size());
      if (b == std::string_view::npos) return {};
      return text.substr(a + open.size(), b - a - open.size());
    };
    release = static_cast<std::uint32_t>(util::parse_int(tag_value("release")).value_or(0));
    if (release < 117 || release > 121) return fail(Status::Decode, "unsupported Stata release {}", release);
    e.little = tag_value("byteorder") == "LSF";
    const std::string_view header = tag_value("header");
    const std::size_t k_at = header.find("<K>");
    if (k_at == std::string_view::npos) return fail(Status::Decode, "damaged Stata header");
    util::ByteReader h(as_bytes(header.substr(k_at + 3)));
    nvar = release >= 119 ? e.u32(h) : e.u16(h);
    h.skip(4);  // "</K>"
    h.skip(3);  // "<N>"
    nobs = release >= 118 ? e.u64(h) : e.u32(h);
    if (!h.ok()) return fail(Status::Decode, "damaged Stata header");
    const std::size_t name_size = release >= 118 ? 129 : 33;
    const std::size_t format_size = release >= 118 ? 57 : 49;
    const std::string_view types = tag_value("variable_types");
    const std::string_view names = tag_value("varnames");
    const std::string_view formats = tag_value("formats");
    if (nvar > 32767 || types.size() < nvar * 2 || names.size() < nvar * name_size || formats.size() < nvar * format_size) {
      return fail(Status::Decode, "damaged Stata variable descriptions");
    }
    util::ByteReader t(as_bytes(types));
    for (std::uint64_t i = 0; i < nvar; ++i) {
      StataVariable var;
      var.type = e.u16(t);
      var.name = c_string(as_bytes(names.substr(static_cast<std::size_t>(i * name_size), name_size)));
      var.format = c_string(as_bytes(formats.substr(static_cast<std::size_t>(i * format_size), format_size)));
      vars.push_back(std::move(var));
    }
    const std::size_t data_tag = text.find("<data>");
    if (data_tag == std::string_view::npos) return fail(Status::Decode, "the Stata file has no data section");
    data_offset = data_tag + 6;
    // strLs: "GSO" v o t len contents.
    if (const std::size_t strls_tag = text.find("<strls>", data_offset); strls_tag != std::string_view::npos) {
      util::ByteReader s(input.subspan(strls_tag + 7));
      while (s.starts_with("GSO")) {
        s.skip(3);
        const std::uint32_t v = e.u32(s);
        const std::uint64_t o = release >= 118 ? e.u64(s) : e.u32(s);
        const std::uint8_t kind = s.u8();
        const std::uint32_t length = e.u32(s);
        const ByteView content = s.bytes(length);
        if (!s.ok()) break;
        CVT_TRY(budget.take(1 + length / 64));
        std::string value = to_string(content);
        if (kind == 130 && !value.empty() && value.back() == '\0') value.pop_back();
        strls[{v, o}] = util::sanitize_utf8(value);
      }
    }
  } else {
    release = r.u8();
    if (release < 113 || release > 115) return fail(Status::Decode, "not a Stata data file (release {})", release);
    const std::uint8_t order = r.u8();
    e.little = order == 2;
    r.skip(2);
    nvar = e.u16(r);
    nobs = e.u32(r);
    r.skip(81 + 18);
    if (!r.ok() || nvar == 0) return fail(Status::Decode, "damaged Stata header");
    const ByteView types = r.bytes(static_cast<std::size_t>(nvar));
    for (std::uint64_t i = 0; i < nvar && r.ok(); ++i) {
      StataVariable var;
      const std::uint8_t type = std::to_integer<std::uint8_t>(types[static_cast<std::size_t>(i)]);
      var.type = type <= 244 ? type : type == 251 ? 65530 : type == 252 ? 65529 : type == 253 ? 65528 : type == 254 ? 65527 : 65526;
      vars.push_back(std::move(var));
    }
    for (StataVariable& var : vars) var.name = c_string(r.bytes(33));
    r.skip(static_cast<std::size_t>(nvar + 1) * 2);
    for (StataVariable& var : vars) var.format = c_string(r.bytes(49));
    r.skip(static_cast<std::size_t>(nvar) * (33 + 81));
    // Expansion fields.
    while (r.ok()) {
      const std::uint8_t type = r.u8();
      const std::uint32_t length = e.u32(r);
      if (type == 0 && length == 0) break;
      r.skip(length);
    }
    if (!r.ok()) return fail(Status::Decode, "the Stata file is truncated");
    data_offset = r.position();
  }
  std::uint64_t row_size = 0;
  for (const StataVariable& var : vars) {
    const std::size_t width = stata_width(var.type);
    if (width == 0) return fail(Status::Decode, "unknown Stata variable type {}", var.type);
    row_size += width;
  }
  if (row_size == 0 || nobs > (input.size() - std::min<std::size_t>(input.size(), data_offset)) / row_size) {
    return fail(Status::Decode, "the Stata file is truncated (declares {} observations)", nobs);
  }
  CVT_TRY(budget.take(nobs * (vars.size() + 1)));
  ir::Sheet sheet;
  sheet.name = file_stem(ctx.filename);
  sheet.has_header = true;
  std::vector<ir::Cell> header;
  for (const StataVariable& var : vars) header.push_back(ir::Cell::string(var.name));
  sheet.rows.push_back(std::move(header));
  util::ByteReader d(input.subspan(data_offset));
  for (std::uint64_t i = 0; i < nobs; ++i) {
    if ((i & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(nobs)));
    std::vector<ir::Cell> row;
    for (const StataVariable& var : vars) {
      ir::Cell cell;
      if (var.type <= 2045) {
        std::string text = c_string(d.bytes(var.type));
        if (!text.empty()) cell = ir::Cell::string(std::move(text));
      } else if (var.type == 32768) {
        std::uint32_t v = 0;
        std::uint64_t o = 0;
        if (release >= 118) {
          const std::uint64_t packed = e.u64(d);
          v = e.little ? static_cast<std::uint32_t>(packed & 0xFFFF) : static_cast<std::uint32_t>(packed >> 48);
          o = e.little ? packed >> 16 : packed & 0xFFFFFFFFFFFFull;
        } else {
          v = e.u32(d);
          o = e.u32(d);
        }
        if (const auto it = strls.find({v, o}); it != strls.end() && !it->second.empty()) cell = ir::Cell::string(it->second);
      } else if (var.type == 65526) {
        const double value = std::bit_cast<double>(e.u64(d));
        if (std::isfinite(value) && value <= 8.988465674311579e307) cell = stata_number(value, var.format);
      } else if (var.type == 65527) {
        const float value = std::bit_cast<float>(e.u32(d));
        if (std::isfinite(value) && value <= 1.7014117e38f) cell = stata_number(value, var.format);
      } else if (var.type == 65528) {
        const auto value = static_cast<std::int32_t>(e.u32(d));
        if (value <= 2147483620) cell = stata_number(value, var.format);
      } else if (var.type == 65529) {
        const auto value = static_cast<std::int16_t>(e.u16(d));
        if (value <= 32740) cell = stata_number(value, var.format);
      } else {
        const auto value = static_cast<std::int8_t>(d.u8());
        if (value <= 100) cell = stata_number(value, var.format);
      }
      row.push_back(std::move(cell));
    }
    sheet.rows.push_back(std::move(row));
  }
  if (!d.ok()) return fail(Status::Decode, "the Stata data section is truncated");
  trim_sheet(sheet);
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

// --------------------------------------------------------------------------- SPSS

struct SpssVariable {
  std::string short_name;
  std::string name;
  int width = 0;           // 0 numeric, else string width in bytes
  std::uint32_t format = 0;
  std::size_t segments = 1;  // 8-byte slots
};

ir::Cell spss_number(double value, std::uint32_t format) {
  const std::uint32_t type = (format >> 16) & 0xFF;
  switch (type) {
    case 20: case 23: case 24: case 28: case 29: case 30: case 38: case 39:
      return datetime_cell(unix_1582 + value, DateStyle::Date);
    case 22: case 41:
      return datetime_cell(unix_1582 + value, DateStyle::DateTime);
    case 21:
      if (value >= 0 && value < 86400) return datetime_cell(excel_1900_epoch_unix + value, DateStyle::Time);
      break;
    default: break;
  }
  return number_cell(value);
}

Result<AnyIR> read_spss(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  const std::string_view magic = r.chars(4);
  if (magic != "$FL2" && magic != "$FL3") return fail(Status::Decode, "not an SPSS data file");
  r.skip(60);
  Endian e;
  const std::uint32_t layout = r.u32le();
  e.little = layout == 2 || layout == 3;
  if (!e.little && std::byteswap(layout) != 2 && std::byteswap(layout) != 3) return fail(Status::Decode, "unknown SPSS layout code");
  (void)e.u32(r);  // nominal case size
  const std::uint32_t compression = e.u32(r);
  (void)e.u32(r);  // weight index
  const auto ncases = static_cast<std::int32_t>(e.u32(r));
  const double bias = std::bit_cast<double>(e.u64(r));
  r.skip(9 + 8 + 64 + 3);
  if (!r.ok()) return fail(Status::Decode, "the SPSS header is truncated");
  std::vector<SpssVariable> vars;
  std::string encoding = "windows-1252";
  std::map<std::string, std::string> long_names;
  while (true) {
    const std::uint32_t type = e.u32(r);
    if (!r.ok()) return fail(Status::Decode, "the SPSS dictionary is truncated");
    if (type == 2) {
      const auto width = static_cast<std::int32_t>(e.u32(r));
      const std::uint32_t has_label = e.u32(r);
      const auto missing = static_cast<std::int32_t>(e.u32(r));
      const std::uint32_t print = e.u32(r);
      (void)e.u32(r);
      const std::string name(util::trim(c_string(r.bytes(8))));
      if (has_label != 0) {
        const std::uint32_t length = e.u32(r);
        if (length > 65536) return fail(Status::Decode, "damaged SPSS variable label");
        r.skip((length + 3) / 4 * 4);
      }
      r.skip(static_cast<std::size_t>(std::abs(missing)) * 8);
      if (width == -1) {
        if (!vars.empty()) ++vars.back().segments;
        continue;
      }
      if (vars.size() >= 100'000) return fail(Status::Limit, "the SPSS file has too many variables");
      SpssVariable var;
      var.short_name = name;
      var.name = name;
      var.width = std::max(0, width);
      var.format = print;
      vars.push_back(std::move(var));
    } else if (type == 3) {
      const std::uint32_t count = e.u32(r);
      for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        r.skip(8);
        const std::uint8_t length = r.u8();
        r.skip(static_cast<std::size_t>((length + 8) / 8 * 8 - 1));
      }
      if (e.u32(r) != 4) return fail(Status::Decode, "damaged SPSS value labels");
      const std::uint32_t indexes = e.u32(r);
      r.skip(std::size_t{indexes} * 4);
    } else if (type == 6) {
      r.skip(std::size_t{e.u32(r)} * 80);
    } else if (type == 7) {
      const std::uint32_t subtype = e.u32(r);
      const std::uint32_t size = e.u32(r);
      const std::uint32_t count = e.u32(r);
      const std::uint64_t bytes = std::uint64_t{size} * count;
      if (bytes > r.remaining()) return fail(Status::Decode, "damaged SPSS extension record");
      const std::string_view data = r.chars(static_cast<std::size_t>(bytes));
      if (subtype == 13) {
        for (const std::string_view pair : util::split(data, '\t', false)) {
          const std::size_t eq = pair.find('=');
          if (eq != std::string_view::npos) long_names[std::string(pair.substr(0, eq))] = std::string(pair.substr(eq + 1));
        }
      } else if (subtype == 20) {
        encoding = std::string(util::trim(data));
      }
    } else if (type == 999) {
      (void)e.u32(r);
      break;
    } else {
      return fail(Status::Decode, "unknown SPSS record type {}", type);
    }
  }
  const bool utf8 = util::iequals(encoding, "UTF-8") || util::iequals(encoding, "UTF8");
  const CodePageInfo* page = utf8 ? nullptr : find_code_page(encoding);
  if (page == nullptr && !utf8) page = find_code_page(1252);
  const auto decode = [&](ByteView bytes) {
    std::string text = utf8 || page == nullptr ? util::sanitize_utf8(as_chars(bytes)) : decode_code_page(bytes, *page);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
    return text;
  };
  for (SpssVariable& var : vars) {
    if (const auto it = long_names.find(var.short_name); it != long_names.end()) var.name = decode(as_bytes(it->second));
  }
  std::size_t slots = 0;
  for (const SpssVariable& var : vars) slots += var.segments;
  if (slots == 0) return fail(Status::Decode, "the SPSS file has no variables");

  // The data as a stream of 8-byte slots.
  Bytes inflated;
  ByteView data = r.peek_rest();
  if (compression == 2) {
    // ZSAV: a header with the offsets of the zlib blocks' trailer.
    util::ByteReader z(data);
    (void)e.u64(z);
    const std::uint64_t trailer_offset = e.u64(z);
    const std::uint64_t trailer_length = e.u64(z);
    if (!z.ok() || trailer_offset > ctx.input.size() || trailer_length > ctx.input.size() - trailer_offset) {
      return fail(Status::Decode, "damaged SPSS zlib header");
    }
    util::ByteReader t(ctx.input.subspan(static_cast<std::size_t>(trailer_offset), static_cast<std::size_t>(trailer_length)));
    (void)e.u64(t);  // bias
    (void)e.u64(t);  // zero
    (void)e.u32(t);  // block size
    const std::uint32_t blocks = e.u32(t);
    for (std::uint32_t b = 0; b < blocks && t.ok(); ++b) {
      (void)e.u64(t);
      const std::uint64_t offset = e.u64(t);
      (void)e.u32(t);
      const std::uint32_t length = e.u32(t);
      if (offset > ctx.input.size() || length > ctx.input.size() - offset) return fail(Status::Decode, "damaged SPSS zlib block");
      CVT_TRY_ASSIGN(const Bytes block, util::zlib_decompress(ctx.input.subspan(static_cast<std::size_t>(offset), length),
                                                              ctx.limits.max_decompressed_bytes - std::min<std::uint64_t>(inflated.size(), ctx.limits.max_decompressed_bytes)));
      inflated.insert(inflated.end(), block.begin(), block.end());
    }
    data = inflated;
  }
  CellBudget budget(ctx.limits);
  ir::Sheet sheet;
  sheet.name = file_stem(ctx.filename);
  sheet.has_header = true;
  std::vector<ir::Cell> header;
  for (const SpssVariable& var : vars) header.push_back(ir::Cell::string(var.name));
  sheet.rows.push_back(std::move(header));
  util::ByteReader d(data);
  // Bytecode state.
  std::uint8_t commands[8] = {};
  std::size_t command = 8;
  bool finished = false;
  const auto next_slot = [&](Bytes& slot, bool& missing) -> bool {
    slot.assign(8, std::byte{0});
    missing = false;
    if (compression == 0) {
      const ByteView raw = d.bytes(8);
      if (!d.ok()) return false;
      ::cvt::util::copy_bytes(slot.data(), raw.data(), 8);
      return true;
    }
    while (true) {
      if (finished) return false;
      if (command == 8) {
        const ByteView block = d.bytes(8);
        if (!d.ok()) return false;
        for (int k = 0; k < 8; ++k) commands[k] = std::to_integer<std::uint8_t>(block[static_cast<std::size_t>(k)]);
        command = 0;
      }
      const std::uint8_t code = commands[command++];
      switch (code) {
        case 0: continue;
        case 252: finished = true; return false;
        case 253: {
          const ByteView raw = d.bytes(8);
          if (!d.ok()) return false;
          ::cvt::util::copy_bytes(slot.data(), raw.data(), 8);
          return true;
        }
        case 254: slot.assign(8, std::byte{' '}); return true;
        case 255: missing = true; return true;
        default: {
          const double value = code - bias;
          const auto bits = std::bit_cast<std::uint64_t>(value);
          for (int k = 0; k < 8; ++k) slot[static_cast<std::size_t>(k)] = static_cast<std::byte>(e.little ? bits >> (8 * k) : bits >> (56 - 8 * k));
          return true;
        }
      }
    }
  };
  Bytes slot;
  std::uint64_t cases = 0;
  while (ncases < 0 || cases < static_cast<std::uint64_t>(ncases)) {
    if ((cases & 4095) == 0) CVT_TRY(ctx.checkpoint(0.5));
    std::vector<ir::Cell> row;
    bool complete = true;
    for (const SpssVariable& var : vars) {
      if (var.width == 0) {
        bool missing = false;
        if (!next_slot(slot, missing)) {
          complete = false;
          break;
        }
        util::ByteReader v(slot);
        const double value = std::bit_cast<double>(e.u64(v));
        const bool system_missing = missing || std::bit_cast<std::uint64_t>(value) == 0xFFEFFFFFFFFFFFFFull;
        row.push_back(system_missing || !std::isfinite(value) ? ir::Cell{} : spss_number(value, var.format));
        for (std::size_t k = 1; k < var.segments && complete; ++k) complete = next_slot(slot, missing);
      } else {
        Bytes text;
        for (std::size_t k = 0; k < var.segments; ++k) {
          bool missing = false;
          if (!next_slot(slot, missing)) {
            complete = false;
            break;
          }
          text.insert(text.end(), slot.begin(), slot.end());
        }
        if (!complete) break;
        text.resize(std::min<std::size_t>(text.size(), static_cast<std::size_t>(var.width)));
        std::string value = decode(text);
        row.push_back(value.empty() ? ir::Cell{} : ir::Cell::string(std::move(value)));
      }
    }
    if (!complete) break;
    CVT_TRY(budget.take(row.size() + 1));
    sheet.rows.push_back(std::move(row));
    ++cases;
  }
  if (ncases >= 0 && cases < static_cast<std::uint64_t>(ncases)) {
    ctx.warn(std::format("The file announces {} cases but holds {}.", ncases, cases));
  }
  trim_sheet(sheet);
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

// --------------------------------------------------------------------------- SAS XPORT

// IBM System/370 floating point (big-endian, base 16, excess-64 exponent),
// possibly truncated to fewer than 8 bytes.
std::optional<double> ibm_to_double(ByteView bytes) {
  std::uint8_t raw[8] = {};
  for (std::size_t i = 0; i < bytes.size() && i < 8; ++i) raw[i] = std::to_integer<std::uint8_t>(bytes[i]);
  bool rest_zero = true;
  for (int i = 1; i < 8; ++i) rest_zero = rest_zero && raw[i] == 0;
  if (rest_zero && (raw[0] == '.' || raw[0] == '_' || (raw[0] >= 'A' && raw[0] <= 'Z'))) return std::nullopt;  // missing
  std::uint64_t mantissa = 0;
  for (int i = 1; i < 8; ++i) mantissa = (mantissa << 8) | raw[i];
  if (mantissa == 0) return 0.0;
  const int exponent = (raw[0] & 0x7F) - 64;
  double value = std::ldexp(static_cast<double>(mantissa), 4 * exponent - 56);
  if ((raw[0] & 0x80) != 0) value = -value;
  return value;
}

ir::Cell sas_number(double value, std::string_view format) {
  const std::string upper = util::to_upper(util::trim(format));
  if (upper.starts_with("DATETIME") || upper.starts_with("E8601DT") || upper.starts_with("IS8601DT")) {
    return datetime_cell(unix_1960 + value, DateStyle::DateTime);
  }
  if (upper.starts_with("TIME") || upper.starts_with("HHMM") || upper.starts_with("E8601TM")) {
    if (value >= 0 && value < 86400) return datetime_cell(excel_1900_epoch_unix + value, DateStyle::Time);
  }
  if (upper.starts_with("DATE") || upper.starts_with("YYMMDD") || upper.starts_with("MMDDYY") || upper.starts_with("DDMMYY") ||
      upper.starts_with("E8601DA") || upper.starts_with("IS8601DA") || upper.starts_with("WORDDATE") || upper.starts_with("MONYY")) {
    return datetime_cell(unix_1960 + value * 86400.0, DateStyle::Date);
  }
  return number_cell(value);
}

Result<AnyIR> read_xport(ReadContext& ctx) {
  const std::string_view text = as_chars(ctx.input);
  if (!text.starts_with("HEADER RECORD*******LIBRARY HEADER RECORD!!!!!!!")) return fail(Status::Decode, "not a SAS transport file");
  ir::Table table;
  CellBudget budget(ctx.limits);
  std::size_t at = 0;
  while (true) {
    const std::size_t member = text.find("HEADER RECORD*******MEMBER  HEADER RECORD!!!!!!!", at);
    if (member == std::string_view::npos || member % 80 != 0) break;
    const auto namestr_size = util::parse_int(util::trim(slice(text, member + 74, 4))).value_or(140);
    if (namestr_size != 140 && namestr_size != 136) return fail(Status::Decode, "unsupported SAS NAMESTR size");
    // Data set name: second record after the DSCRPTR header, bytes 8..16.
    // The first record after the DSCRPTR header: "SAS     " then the data set name.
    std::string name = member + 176 <= text.size() ? std::string(util::trim(slice(text, member + 168, 8))) : std::string();
    const std::size_t namestr = text.find("HEADER RECORD*******NAMESTR HEADER RECORD!!!!!!!", member);
    if (namestr == std::string_view::npos) return fail(Status::Decode, "the SAS transport file has no NAMESTR header");
    const auto count = util::parse_int(util::trim(slice(text, namestr + 54, 4)));
    if (!count || *count <= 0 || *count > 9999) return fail(Status::Decode, "damaged SAS NAMESTR header");
    struct Variable {
      bool numeric = true;
      std::size_t length = 8;
      std::size_t position = 0;
      std::string name;
      std::string format;
    };
    std::vector<Variable> vars;
    std::size_t row_size = 0;
    for (std::int64_t i = 0; i < *count; ++i) {
      const std::size_t offset = namestr + 80 + static_cast<std::size_t>(i) * static_cast<std::size_t>(namestr_size);
      if (offset + 140 > ctx.input.size()) return fail(Status::Decode, "the SAS transport file is truncated");
      util::ByteReader n(ctx.input.subspan(offset, 140));
      Variable var;
      var.numeric = n.u16be() == 1;
      (void)n.u16be();
      var.length = n.u16be();
      (void)n.u16be();
      var.name = std::string(util::trim(c_string(n.bytes(8))));
      const std::string label(util::trim(c_string(n.bytes(40))));
      var.format = std::string(util::trim(c_string(n.bytes(8))));
      n.skip(2 + 2 + 2 + 2 + 8 + 2 + 2);
      var.position = n.u32be();
      if (!label.empty() && var.name.empty()) var.name = label;
      if (var.length == 0 || var.length > 32767 || (var.numeric && var.length > 8)) return fail(Status::Decode, "damaged SAS variable description");
      row_size = std::max(row_size, var.position + var.length);
      vars.push_back(std::move(var));
    }
    const std::size_t obs = text.find("HEADER RECORD*******OBS     HEADER RECORD!!!!!!!", namestr);
    if (obs == std::string_view::npos) return fail(Status::Decode, "the SAS transport file has no observations header");
    const std::size_t data_start = std::min(obs + 80, text.size());
    const std::size_t next_member = text.find("HEADER RECORD*******MEMBER  HEADER RECORD!!!!!!!", data_start);
    const std::size_t data_end = next_member == std::string_view::npos ? text.size() : next_member;
    ir::Sheet sheet;
    sheet.name = name.empty() ? file_stem(ctx.filename) : name;
    sheet.has_header = true;
    std::vector<ir::Cell> header;
    for (const Variable& var : vars) header.push_back(ir::Cell::string(var.name));
    sheet.rows.push_back(std::move(header));
    if (row_size == 0) return fail(Status::Decode, "the SAS data set has no width");
    std::size_t rows = (data_end - data_start) / row_size;
    // Trailing padding to the 80-byte record boundary is made of spaces.
    while (rows > 0) {
      const std::string_view last = slice(text, data_start + (rows - 1) * row_size, row_size);
      if (last.find_first_not_of(' ') != std::string_view::npos) break;
      if (data_start + rows * row_size + 80 <= data_end) break;
      --rows;
    }
    CVT_TRY(budget.take(std::uint64_t{rows} * (vars.size() + 1)));
    for (std::size_t r = 0; r < rows; ++r) {
      if ((r & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(rows)));
      std::vector<ir::Cell> row;
      for (const Variable& var : vars) {
        const ByteView field = ctx.input.subspan(data_start + r * row_size + var.position, var.length);
        if (var.numeric) {
          const auto value = ibm_to_double(field);
          row.push_back(value ? sas_number(*value, var.format) : ir::Cell{});
        } else {
          std::string value = decode_code_page(field, *find_code_page(1252));
          while (!value.empty() && value.back() == ' ') value.pop_back();
          row.push_back(value.empty() ? ir::Cell{} : ir::Cell::string(std::move(value)));
        }
      }
      sheet.rows.push_back(std::move(row));
    }
    trim_sheet(sheet);
    CVT_TRY(ctx.limits.check_archive_entries(table.sheets.size() + 1));
    table.sheets.push_back(std::move(sheet));
    at = data_end;
    if (next_member == std::string_view::npos) break;
  }
  if (table.sheets.empty()) return fail(Status::Decode, "the SAS transport file holds no data set");
  return table;
}

int sniff_stata(ByteView data, std::string_view) {
  const std::string_view head = as_chars(data);
  if (head.starts_with("<stata_dta><header><release>")) return 95;
  if (data.size() >= 4 && (head[0] == 113 || head[0] == 114 || head[0] == 115) && (head[1] == 1 || head[1] == 2) && head[2] == 1) return 60;
  return 0;
}

}  // namespace

void register_statistics(Registry& registry) {
  {
    FormatInfo info;
    info.id = "dta";
    info.name = "Stata data file";
    info.description = "Data set of the Stata statistics package (read only).";
    info.category = Category::Data;
    info.extensions = {"dta"};
    info.mime = "application/x-stata-dta";
    info.popularity = 15;
    info.sniff = sniff_stata;
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = "dta";
    codec.kind = Kind::Table;
    codec.read = read_stata;
    registry.add_codec(std::move(codec));
  }
  {
    FormatInfo info;
    info.id = "sav";
    info.name = "SPSS data file";
    info.description = "Data set of IBM SPSS Statistics (read only).";
    info.category = Category::Data;
    info.extensions = {"sav", "zsav"};
    info.mime = "application/x-spss-sav";
    info.popularity = 15;
    info.signatures = {Signature::at(0, "'$FL2'"), Signature::at(0, "'$FL3'")};
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = "sav";
    codec.kind = Kind::Table;
    codec.read = read_spss;
    registry.add_codec(std::move(codec));
  }
  {
    FormatInfo info;
    info.id = "xpt";
    info.name = "SAS transport file";
    info.description = "SAS XPORT (version 5) data set transport file (read only).";
    info.category = Category::Data;
    info.extensions = {"xpt"};
    info.mime = "application/x-sas-xport";
    info.popularity = 10;
    info.signatures = {Signature::at(0, "'HEADER RECORD*******LIBRARY HEADER RECORD!!!!!!!'")};
    registry.add_format(std::move(info));
    Codec codec;
    codec.format_id = "xpt";
    codec.kind = Kind::Table;
    codec.read = read_xport;
    registry.add_codec(std::move(codec));
  }
}

}  // namespace cvt::table
