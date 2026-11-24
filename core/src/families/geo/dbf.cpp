// Convertible core - geo family: dBase (.dbf) tables (see dbf.hpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/geo/dbf.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "families/geo/geo.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::geo::dbf {

std::optional<util::TextEncoding> encoding_from_cpg(std::string_view cpg) {
  const std::string t = util::to_lower(util::trim(cpg));
  if (t.empty()) return std::nullopt;
  if (t == "1252" || t == "ansi 1252" || t == "cp1252" || t == "windows-1252" || t == "ldid/87") {
    return util::TextEncoding::Windows1252;
  }
  if (t == "88591" || t == "8859-1" || t == "iso8859-1" || t == "iso-8859-1" || t == "iso 88591" || t == "latin1") {
    return util::TextEncoding::Latin1;
  }
  if (t == "utf-8" || t == "utf8" || t == "65001") return util::TextEncoding::Utf8;
  return util::parse_encoding(t);
}

namespace {

std::optional<util::TextEncoding> encoding_from_ldid(std::uint8_t ldid) {
  switch (ldid) {
    case 0x03:
    case 0x57:
    case 0x58:
    case 0x59: return util::TextEncoding::Windows1252;
    default: return std::nullopt;
  }
}

std::string decode(std::string_view raw, std::optional<util::TextEncoding> encoding) {
  if (!encoding) {
    if (util::is_valid_utf8(raw)) return std::string(raw);
    encoding = util::TextEncoding::Windows1252;
  }
  if (*encoding == util::TextEncoding::Utf8) return util::sanitize_utf8(raw);
  return util::decode_text(as_bytes(raw), *encoding).text;
}

Json value_of(const Field& f, std::string_view raw, std::optional<util::TextEncoding> encoding) {
  switch (f.type) {
    case 'N':
    case 'F': {
      const std::string_view t = util::trim(raw);
      if (t.empty() || t.front() == '*') return Json();
      if (f.decimals == 0 && t.find_first_of(".eE") == std::string_view::npos) {
        if (auto v = util::parse_int(t.front() == '+' ? t.substr(1) : t)) return Json(*v);
      }
      if (auto v = parse_number(t)) return Json(*v);
      return Json();
    }
    case 'L': {
      const char c = util::trim(raw).empty() ? '?' : util::trim(raw).front();
      if (c == 'T' || c == 't' || c == 'Y' || c == 'y') return Json(true);
      if (c == 'F' || c == 'f' || c == 'N' || c == 'n') return Json(false);
      return Json();
    }
    case 'D': {
      const std::string_view t = util::trim(raw);
      if (t.size() != 8 || t.find_first_not_of("0123456789") != std::string_view::npos) return Json();
      return Json(std::string(t.substr(0, 4)) + "-" + std::string(t.substr(4, 2)) + "-" + std::string(t.substr(6, 2)));
    }
    case 'I': {
      if (raw.size() != 4) return Json();
      util::ByteReader r(as_bytes(raw));
      return Json(static_cast<std::int64_t>(r.i32le()));
    }
    case 'O': {
      if (raw.size() != 8) return Json();
      util::ByteReader r(as_bytes(raw));
      return Json(r.f64le());
    }
    case 'M': return Json();  // memo text lives in a .dbt file that is not part of the exchange
    default: {
      std::string text = decode(raw, encoding);
      while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
      return Json(std::move(text));
    }
  }
}

}  // namespace

Result<Table> read(ByteView data, std::optional<util::TextEncoding> encoding, const Context& ctx) {
  util::ByteReader r(data);
  const std::uint8_t version = r.u8();
  r.skip(3);
  const std::uint32_t count = r.u32le();
  const std::uint16_t header_size = r.u16le();
  const std::uint16_t record_size = r.u16le();
  r.skip(17);
  const std::uint8_t ldid = r.u8();
  if (!r.ok() || (version & 0x07) == 0 || header_size < 33 || record_size == 0) {
    return fail(Status::Decode, "not a valid dBase attribute table");
  }
  if (!encoding) encoding = encoding_from_ldid(ldid);
  Table table;
  r.seek(32);
  std::uint32_t offset = 1;
  while (r.position() + 32 <= header_size && r.remaining() >= 1 && r.peek_u8() != 0x0D) {
    Field f;
    const std::string_view name = r.chars(11);
    f.name = decode(name.substr(0, std::min(name.find('\0'), name.size())), encoding);
    f.type = static_cast<char>(r.u8());
    r.skip(4);
    f.length = r.u8();
    f.decimals = r.u8();
    r.skip(14);
    if (!r.ok()) return fail(Status::Decode, "truncated dBase field descriptors");
    offset += f.length;
    table.fields.push_back(std::move(f));
    if (table.fields.size() > 2048) return fail(Status::Decode, "the dBase table declares too many fields");
  }
  if (offset > record_size) return fail(Status::Decode, "dBase fields are longer than the record size");
  if (header_size > data.size()) return fail(Status::Decode, "the dBase header is truncated");
  const std::uint64_t available = (data.size() - header_size) / record_size;
  if (count > available) ctx.warn("The attribute table is shorter than it declares; missing records were left empty.");
  const std::uint64_t records = std::min<std::uint64_t>(count, available);
  table.records.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(records, 1u << 20)));
  for (std::uint64_t i = 0; i < records; ++i) {
    if ((i & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(records)));
    const std::size_t base = header_size + static_cast<std::size_t>(i) * record_size;
    const std::string_view record = as_chars(data.subspan(base, record_size));
    if (record[0] == '*') continue;  // deleted
    Json row = Json::object();
    std::size_t at = 1;
    for (const Field& f : table.fields) {
      row.set(f.name, value_of(f, record.substr(at, f.length), encoding));
      at += f.length;
    }
    table.records.push_back(std::move(row));
  }
  return table;
}

namespace {

std::size_t integer_digits(double v) {
  const double a = std::fabs(v);
  return a < 1 ? 1 : static_cast<std::size_t>(std::floor(std::log10(a))) + 1;
}

std::size_t decimals_needed(double v) {
  const std::string text = format_number(v);
  if (text.find_first_of("eE") != std::string::npos) return 15;
  const std::size_t dot = text.find('.');
  return dot == std::string::npos ? 0 : text.size() - dot - 1;
}

std::string truncate_utf8(std::string_view text, std::size_t bytes) {
  if (text.size() <= bytes) return std::string(text);
  std::size_t end = bytes;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return std::string(text.substr(0, end));
}

}  // namespace

Result<Bytes> write(const std::vector<const Json*>& rows, const Context& ctx, std::vector<std::string>* names) {
  // Collect keys in order of first appearance and infer their types.
  struct Column {
    std::string key;
    bool any_bool = false, any_int = false, any_double = false, any_text = false;
    std::size_t width = 1, int_digits = 1, decimals = 0;
  };
  std::vector<Column> columns;
  std::map<std::string, std::size_t, std::less<>> index;
  for (const Json* row : rows) {
    if (row == nullptr) continue;
    for (const auto& m : row->items()) {
      auto [it, inserted] = index.try_emplace(std::string(m.key()), columns.size());
      if (inserted) {
        columns.emplace_back();
        columns.back().key = std::string(m.key());
      }
      Column& c = columns[it->second];
      const Json& v = m.value();
      if (v.is_null()) continue;
      if (v.is_bool()) {
        c.any_bool = true;
      } else if (v.is_integer()) {
        c.any_int = true;
        c.int_digits = std::max(c.int_digits, json_to_text(v).size());
      } else if (v.is_float()) {
        c.any_double = true;
        c.int_digits = std::max(c.int_digits, integer_digits(v.as_double()) + (v.as_double() < 0 ? 1 : 0));
        c.decimals = std::max(c.decimals, decimals_needed(v.as_double()));
      } else {
        c.any_text = true;
        c.width = std::max(c.width, json_to_text(v).size());
      }
    }
  }
  std::vector<Field> fields;
  std::vector<std::string> used;
  bool truncated = false;
  for (Column& c : columns) {
    Field f;
    // A 10-byte ASCII name, unique.
    std::string name;
    for (char ch : c.key) {
      if (name.size() == 10) break;
      name += (util::is_ascii_alpha(ch) || util::is_ascii_digit(ch) || ch == '_') ? ch : '_';
    }
    if (name.empty()) name = "field";
    std::string unique = name;
    for (int n = 1; std::find(used.begin(), used.end(), util::to_lower(unique)) != used.end(); ++n) {
      const std::string suffix = std::to_string(n);
      unique = name.substr(0, std::min<std::size_t>(name.size(), 10 - suffix.size())) + suffix;
    }
    used.push_back(util::to_lower(unique));
    f.name = unique;
    const int kinds = int(c.any_bool) + int(c.any_int || c.any_double) + int(c.any_text);
    if (kinds > 1 || c.any_text || kinds == 0) {
      f.type = 'C';
      if (c.width > 254) truncated = true;
      // Mixed columns are written as text; numbers and booleans need their text width too.
      std::size_t width = c.width;
      if (c.any_int || c.any_double) width = std::max(width, c.int_digits + c.decimals + 2);
      f.length = static_cast<std::uint8_t>(std::clamp<std::size_t>(width, 1, 254));
    } else if (c.any_bool) {
      f.type = 'L';
      f.length = 1;
    } else if (c.any_double) {
      f.type = 'N';
      std::size_t decimals = std::min<std::size_t>(c.decimals, 15);
      const std::size_t width = std::min<std::size_t>(c.int_digits + 1 + std::max<std::size_t>(decimals, 1), 32);
      f.length = static_cast<std::uint8_t>(width);
      f.decimals = static_cast<std::uint8_t>(std::max<std::size_t>(decimals, 1));
    } else {
      f.type = 'N';
      f.length = static_cast<std::uint8_t>(std::clamp<std::size_t>(c.int_digits, 1, 20));
    }
    fields.push_back(f);
  }
  if (truncated) ctx.warn("Attribute text longer than 254 bytes was cut to fit the dBase format.");
  std::size_t record_size = 1;
  for (const Field& f : fields) record_size += f.length;
  if (record_size > 65535 || fields.size() > 255) {
    return fail(Status::Encode, "the attributes need {} fields of {} bytes, more than a dBase table can hold",
                fields.size(), record_size);
  }
  util::ByteWriter out(32 + fields.size() * 32 + 2 + rows.size() * record_size);
  out.u8(0x03).u8(0).u8(1).u8(1);  // dBase III, last update fixed at 1900-01-01 (no current date)
  out.u32le(static_cast<std::uint32_t>(rows.size()));
  out.u16le(static_cast<std::uint16_t>(32 + fields.size() * 32 + 1));
  out.u16le(static_cast<std::uint16_t>(record_size));
  out.zeros(17);
  out.u8(0);  // language driver: none (the .cpg file says UTF-8)
  out.zeros(2);
  for (const Field& f : fields) {
    std::string name = f.name;
    name.resize(11, '\0');
    out.text(name);
    out.u8(static_cast<std::uint8_t>(f.type));
    out.zeros(4);
    out.u8(f.length);
    out.u8(f.decimals);
    out.zeros(14);
  }
  out.u8(0x0D);
  for (std::size_t r = 0; r < rows.size(); ++r) {
    if ((r & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(rows.size())));
    out.u8(' ');
    for (std::size_t i = 0; i < fields.size(); ++i) {
      const Field& f = fields[i];
      const Json& v = rows[r] != nullptr ? (*rows[r])[columns[i].key] : Json();
      std::string text;
      if (!v.is_null()) {
        if (f.type == 'L') {
          text = v.as_bool() ? "T" : "F";
        } else if (f.type == 'N' && f.decimals > 0) {
          text = format_number(v.as_double(), f.decimals);
          // Pad the decimals so that every value has the declared form.
          const std::size_t dot = text.find('.');
          const std::size_t have = dot == std::string::npos ? 0 : text.size() - dot - 1;
          if (dot == std::string::npos) text += '.';
          text.append(f.decimals - have, '0');
        } else {
          text = json_to_text(v);
        }
      }
      if (f.type == 'N' || f.type == 'L') {
        if (text.size() > f.length) text = std::string(f.length, '*');
        out.text(std::string(f.length - text.size(), ' '));
        out.text(text);
      } else {
        text = truncate_utf8(text, f.length);
        out.text(text);
        out.text(std::string(f.length - text.size(), ' '));
      }
    }
  }
  out.u8(0x1A);
  if (names != nullptr) {
    names->clear();
    for (const Column& c : columns) names->push_back(c.key);
  }
  return out.take();
}

}  // namespace cvt::geo::dbf
