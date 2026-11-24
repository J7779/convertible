// Convertible core - font family: CFF and CFF2 (Type 2 charstrings).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/cff.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"
#include "util/text.hpp"

namespace cvt::font {

namespace {

constexpr std::size_t max_index_count = 65536 * 4;
constexpr int max_subr_depth = 10;
constexpr std::size_t max_charstring_ops = 200000;

// --------------------------------------------------------------------------- INDEX

Result<std::vector<ByteView>> read_index(util::ByteReader& reader, bool cff2) {
  const std::uint32_t count = cff2 ? reader.u32be() : reader.u16be();
  if (!reader.ok()) return fail(Status::Decode, "CFF INDEX is truncated");
  std::vector<ByteView> items;
  if (count == 0) return items;
  if (count > max_index_count) return fail(Status::Decode, "CFF INDEX has {} entries", count);
  const std::uint8_t off_size = reader.u8();
  if (off_size < 1 || off_size > 4) return fail(Status::Decode, "CFF INDEX offset size {} is invalid", off_size);
  if ((std::uint64_t{count} + 1) * off_size > reader.remaining()) return fail(Status::Decode, "CFF INDEX is truncated");
  std::vector<std::uint32_t> offsets(count + 1);
  for (std::uint32_t& offset : offsets) {
    std::uint32_t value = 0;
    for (int i = 0; i < off_size; ++i) value = (value << 8) | reader.u8();
    offset = value;
  }
  if (offsets[0] != 1) return fail(Status::Decode, "CFF INDEX does not start at offset 1");
  const std::size_t data_start = reader.position() - 1;
  const std::uint64_t data_size = std::uint64_t{offsets.back()} - 1;
  if (data_size > reader.remaining()) return fail(Status::Decode, "CFF INDEX data is truncated");
  items.reserve(count);
  util::ByteReader all(reader.peek_rest());
  (void)all;
  const ByteView base = reader.bytes(static_cast<std::size_t>(data_size));
  (void)data_start;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (offsets[i + 1] < offsets[i]) return fail(Status::Decode, "CFF INDEX offsets are not ordered");
    items.push_back(base.subspan(offsets[i] - 1, offsets[i + 1] - offsets[i]));
  }
  return items;
}

// --------------------------------------------------------------------------- DICT

Result<CffDict> read_dict(ByteView data, std::size_t max_operands = 513) {
  CffDict dict;
  std::vector<double> operands;
  util::ByteReader reader(data);
  while (!reader.at_end()) {
    const std::uint8_t b0 = reader.u8();
    if (b0 <= 21 || b0 == 22 || b0 == 23 || b0 == 24) {
      int op = b0;
      if (b0 == 12) op = 1200 + reader.u8();
      if (op == 23) {  // CFF2 blend inside a DICT: keep the default values
        if (operands.empty()) return fail(Status::Decode, "CFF2 blend without operands");
        const auto n = static_cast<std::size_t>(operands.back());
        operands.pop_back();
        if (n == 0 || n > operands.size()) return fail(Status::Decode, "CFF2 blend operand count is invalid");
        const std::size_t total = operands.size();
        const std::size_t k = total / n - 1;
        if ((k + 1) * n > total) return fail(Status::Decode, "CFF2 blend operand count is invalid");
        const std::size_t first = total - (k + 1) * n;
        operands.resize(first + n);
        continue;
      }
      dict[op] = operands;
      operands.clear();
    } else if (b0 == 28) {
      operands.push_back(reader.i16be());
    } else if (b0 == 29) {
      operands.push_back(reader.i32be());
    } else if (b0 == 30) {
      std::string text;
      bool done = false;
      while (!done && reader.ok() && !reader.at_end() && text.size() < 64) {
        const std::uint8_t byte = reader.u8();
        for (const int nibble : {byte >> 4, byte & 0xF}) {
          if (done) break;
          if (nibble <= 9) text.push_back(static_cast<char>('0' + nibble));
          else if (nibble == 0xA) text.push_back('.');
          else if (nibble == 0xB) text.push_back('E');
          else if (nibble == 0xC) text += "E-";
          else if (nibble == 0xE) text.push_back('-');
          else if (nibble == 0xF) done = true;
        }
      }
      operands.push_back(util::parse_double(text).value_or(0.0));
    } else if (b0 >= 32 && b0 <= 246) {
      operands.push_back(b0 - 139);
    } else if (b0 >= 247 && b0 <= 250) {
      operands.push_back((b0 - 247) * 256 + reader.u8() + 108);
    } else if (b0 >= 251 && b0 <= 254) {
      operands.push_back(-(b0 - 251) * 256 - reader.u8() - 108);
    } else {
      return fail(Status::Decode, "CFF DICT contains the reserved byte {}", b0);
    }
    if (operands.size() > max_operands) return fail(Status::Decode, "CFF DICT has too many operands");
    if (!reader.ok()) return fail(Status::Decode, "CFF DICT is truncated");
  }
  return dict;
}

std::uint32_t subr_bias(std::size_t count) {
  if (count < 1240) return 107;
  if (count < 33900) return 1131;
  return 32768;
}

Result<CffPrivate> read_private(ByteView file, const std::vector<double>& where, bool cff2) {
  CffPrivate result;
  if (where.size() < 2) return result;
  const double size = where[0], offset = where[1];
  if (size < 0 || offset < 0 || offset + size > static_cast<double>(file.size())) {
    return fail(Status::Decode, "CFF Private DICT is out of range");
  }
  const auto start = static_cast<std::size_t>(offset);
  CVT_TRY_ASSIGN(result.dict, read_dict(file.subspan(start, static_cast<std::size_t>(size))));
  if (const auto it = result.dict.find(20); it != result.dict.end() && !it->second.empty()) {
    result.default_width = it->second[0];
  }
  if (const auto it = result.dict.find(21); it != result.dict.end() && !it->second.empty()) {
    result.nominal_width = it->second[0];
  }
  if (const auto it = result.dict.find(19); it != result.dict.end() && !it->second.empty()) {
    const double subrs = static_cast<double>(start) + it->second[0];
    if (it->second[0] <= 0 || subrs >= static_cast<double>(file.size())) {
      return fail(Status::Decode, "CFF local subroutines are out of range");
    }
    util::ByteReader reader(file);
    reader.seek(static_cast<std::size_t>(subrs));
    CVT_TRY_ASSIGN(result.subrs, read_index(reader, cff2));
  }
  return result;
}

}  // namespace

std::string CffFont::string_for(int sid) const {
  const auto standard = cff_standard_strings();
  if (sid >= 0 && static_cast<std::size_t>(sid) < standard.size()) return std::string(standard[static_cast<std::size_t>(sid)]);
  const std::size_t index = static_cast<std::size_t>(sid) - standard.size();
  if (sid >= 0 && index < strings.size()) return strings[index];
  return {};
}

std::vector<double> CffFont::top_value(int op, std::vector<double> fallback) const {
  const auto it = top.find(op);
  return it != top.end() ? it->second : std::move(fallback);
}

Result<CffFont> parse_cff(ByteView data) {
  CffFont font;
  util::ByteReader reader(data);
  const std::uint8_t major = reader.u8();
  (void)reader.u8();
  const std::uint8_t header_size = reader.u8();
  if (!reader.ok() || (major != 1 && major != 2)) return fail(Status::Decode, "not a CFF table (version {})", major);
  font.cff2 = major == 2;
  if (font.cff2) {
    const std::uint16_t top_length = reader.u16be();
    if (!reader.ok() || !reader.seek(header_size)) return fail(Status::Decode, "CFF2 header is truncated");
    const ByteView top = reader.bytes(top_length);
    if (!reader.ok()) return fail(Status::Decode, "CFF2 Top DICT is truncated");
    CVT_TRY_ASSIGN(font.top, read_dict(top));
    CVT_TRY_ASSIGN(font.global_subrs, read_index(reader, true));
  } else {
    if (!reader.seek(header_size)) return fail(Status::Decode, "CFF header is truncated");
    CVT_TRY_ASSIGN(const std::vector<ByteView> names, read_index(reader, false));
    CVT_TRY_ASSIGN(const std::vector<ByteView> tops, read_index(reader, false));
    CVT_TRY_ASSIGN(const std::vector<ByteView> strings, read_index(reader, false));
    CVT_TRY_ASSIGN(font.global_subrs, read_index(reader, false));
    if (names.empty() || tops.empty()) return fail(Status::Decode, "the CFF table holds no font");
    if (names.size() > 1) {
      // CFF tables in OpenType hold exactly one font; use the first of a FontSet.
    }
    font.name = std::string(as_chars(names[0]));
    for (const ByteView s : strings) font.strings.emplace_back(as_chars(s));
    CVT_TRY_ASSIGN(font.top, read_dict(tops[0]));
  }
  const auto matrix = font.top_value(1207);
  if (matrix.size() == 6 && std::abs(matrix[0]) > 1e-9) {
    font.font_matrix = matrix;
    font.units_per_em = std::round(1.0 / std::abs(matrix[0]));
    if (font.units_per_em < 16 || font.units_per_em > 16384) font.units_per_em = 1000;
  }
  const auto charstrings_at = font.top_value(17);
  if (charstrings_at.empty() || charstrings_at[0] <= 0 || charstrings_at[0] >= static_cast<double>(data.size())) {
    return fail(Status::Decode, "the CFF font has no CharStrings");
  }
  {
    util::ByteReader cs(data);
    cs.seek(static_cast<std::size_t>(charstrings_at[0]));
    CVT_TRY_ASSIGN(font.charstrings, read_index(cs, font.cff2));
  }
  if (font.charstrings.empty()) return fail(Status::Decode, "the CFF font has no glyphs");
  if (font.charstrings.size() > 65535) return fail(Status::Decode, "the CFF font has too many glyphs");
  const std::size_t count = font.charstrings.size();
  const auto fd_array_at = font.top_value(1236);
  font.cid_keyed = font.top.contains(1230);
  if (!fd_array_at.empty()) {
    if (fd_array_at[0] <= 0 || fd_array_at[0] >= static_cast<double>(data.size())) {
      return fail(Status::Decode, "CFF FDArray is out of range");
    }
    util::ByteReader fr(data);
    fr.seek(static_cast<std::size_t>(fd_array_at[0]));
    CVT_TRY_ASSIGN(const std::vector<ByteView> fds, read_index(fr, font.cff2));
    if (fds.empty() || fds.size() > 256) return fail(Status::Decode, "CFF FDArray has {} entries", fds.size());
    for (const ByteView fd : fds) {
      CVT_TRY_ASSIGN(const CffDict dict, read_dict(fd));
      font.fd_dicts.push_back(dict);
      const auto it = dict.find(18);
      CVT_TRY_ASSIGN(CffPrivate priv, read_private(data, it != dict.end() ? it->second : std::vector<double>{}, font.cff2));
      font.privates.push_back(std::move(priv));
    }
    const auto select_at = font.top_value(1237);
    if (!select_at.empty()) {
      if (select_at[0] <= 0 || select_at[0] >= static_cast<double>(data.size())) {
        return fail(Status::Decode, "CFF FDSelect is out of range");
      }
      util::ByteReader sr(data);
      sr.seek(static_cast<std::size_t>(select_at[0]));
      const std::uint8_t format = sr.u8();
      font.fd_select.assign(count, 0);
      if (format == 0) {
        for (std::size_t g = 0; g < count; ++g) font.fd_select[g] = sr.u8();
      } else if (format == 3 || format == 4) {
        const std::uint32_t ranges = format == 3 ? sr.u16be() : sr.u32be();
        std::uint32_t first = format == 3 ? sr.u16be() : sr.u32be();
        for (std::uint32_t r = 0; r < ranges && sr.ok(); ++r) {
          const std::uint16_t fd = format == 3 ? sr.u8() : sr.u16be();
          const std::uint32_t next = format == 3 ? sr.u16be() : sr.u32be();
          for (std::uint32_t g = first; g < next && g < count; ++g) font.fd_select[g] = static_cast<std::uint8_t>(fd);
          first = next;
        }
      } else {
        return fail(Status::Decode, "CFF FDSelect format {} is not supported", format);
      }
      if (!sr.ok()) return fail(Status::Decode, "CFF FDSelect is truncated");
      for (const std::uint8_t fd : font.fd_select) {
        if (fd >= font.privates.size()) return fail(Status::Decode, "CFF FDSelect refers to a missing FD");
      }
    }
  } else {
    CVT_TRY_ASSIGN(CffPrivate priv, read_private(data, font.top_value(18), font.cff2));
    font.privates.push_back(std::move(priv));
  }
  if (font.cff2) {
    const auto vstore = font.top_value(24);
    if (!vstore.empty() && vstore[0] > 0 && vstore[0] < static_cast<double>(data.size())) {
      const auto at = static_cast<std::size_t>(vstore[0]) + 2;  // skip the length field
      const std::uint16_t data_count = get_u16(data, at + 6);
      for (std::uint16_t i = 0; i < data_count && i < 1024; ++i) {
        const std::uint32_t offset = get_u32(data, at + 8 + 4u * i);
        font.region_counts.push_back(get_u16(data, at + offset + 4));
      }
    }
  } else {
    const auto charset_at = font.top_value(15, {0});
    font.charset.assign(count, 0);
    const double where = charset_at.empty() ? 0 : charset_at[0];
    if (where > 2) {
      if (where >= static_cast<double>(data.size())) return fail(Status::Decode, "CFF charset is out of range");
      util::ByteReader cr(data);
      cr.seek(static_cast<std::size_t>(where));
      const std::uint8_t format = cr.u8();
      std::size_t g = 1;
      if (format == 0) {
        for (; g < count && cr.ok(); ++g) font.charset[g] = cr.u16be();
      } else if (format == 1 || format == 2) {
        while (g < count && cr.ok()) {
          const std::uint16_t first = cr.u16be();
          const std::uint32_t left = format == 1 ? cr.u8() : cr.u16be();
          for (std::uint32_t i = 0; i <= left && g < count; ++i) font.charset[g++] = static_cast<std::uint16_t>(first + i);
        }
      } else {
        return fail(Status::Decode, "CFF charset format {} is not supported", format);
      }
      if (!cr.ok()) return fail(Status::Decode, "CFF charset is truncated");
    } else {
      // Predefined charsets (ISOAdobe = SID i): only ISOAdobe is common.
      for (std::size_t g = 0; g < count; ++g) font.charset[g] = static_cast<std::uint16_t>(std::min<std::size_t>(g, 228));
    }
  }
  return font;
}

std::vector<std::string> cff_glyph_names(ByteView cff, std::uint32_t count) {
  auto font = parse_cff(cff);
  if (!font || font->cff2 || font->charset.empty()) return {};
  std::vector<std::string> names(count);
  for (std::uint32_t g = 0; g < count && g < font->charset.size(); ++g) {
    names[g] = font->cid_keyed ? std::format("cid{:05}", font->charset[g]) : font->string_for(font->charset[g]);
  }
  return names;
}

// --------------------------------------------------------------------------- Type 2 interpreter

namespace {

class Interpreter {
 public:
  Interpreter(const CffFont& font, const CffPrivate& priv) : font_(font), priv_(priv) {
    max_stack_ = font.cff2 ? 513 : 48;
  }

  Result<CffGlyph> run(ByteView charstring) {
    CVT_TRY(execute(charstring, 0));
    if (open_) path_.close();
    CffGlyph glyph;
    glyph.path = std::move(path_);
    if (has_width_) glyph.width = width_;
    else if (!font_.cff2) glyph.width = priv_.default_width;
    return glyph;
  }

  bool seac = false;
  double seac_adx = 0, seac_ady = 0;
  int seac_base = 0, seac_accent = 0;

 private:
  Result<void> push(double v) {
    if (stack_.size() >= max_stack_) return fail(Status::Decode, "charstring stack overflow");
    stack_.push_back(v);
    return {};
  }

  void take_width(std::size_t expected_even) {
    if (width_taken_ || font_.cff2) return;
    width_taken_ = true;
    if (stack_.size() > expected_even) {
      has_width_ = true;
      width_ = priv_.nominal_width + stack_.front();
      stack_.erase(stack_.begin());
    }
  }

  void move(double dx, double dy) {
    if (open_) path_.close();
    x_ += dx;
    y_ += dy;
    path_.move_to({x_, y_});
    open_ = true;
  }
  void line(double dx, double dy) {
    if (!open_) move(0, 0);
    x_ += dx;
    y_ += dy;
    path_.line_to({x_, y_});
  }
  void curve(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
    if (!open_) move(0, 0);
    const Point c1{x_ + dx1, y_ + dy1};
    const Point c2{c1.x + dx2, c1.y + dy2};
    x_ = c2.x + dx3;
    y_ = c2.y + dy3;
    path_.cubic_to(c1, c2, {x_, y_});
  }

  std::size_t stems_ = 0;

  Result<void> execute(ByteView code, int depth) {
    if (depth > max_subr_depth) return fail(Status::Decode, "charstring subroutines nest too deeply");
    util::ByteReader r(code);
    while (!r.at_end()) {
      if (++ops_ > max_charstring_ops) return fail(Status::Decode, "charstring is too long");
      const std::uint8_t b0 = r.u8();
      if (b0 >= 32 || b0 == 28) {
        double v = 0;
        if (b0 == 28) v = r.i16be();
        else if (b0 <= 246) v = b0 - 139;
        else if (b0 <= 250) v = (b0 - 247) * 256 + r.u8() + 108;
        else if (b0 <= 254) v = -(b0 - 251) * 256 - r.u8() - 108;
        else v = r.i32be() / 65536.0;
        if (!r.ok()) return fail(Status::Decode, "charstring is truncated");
        CVT_TRY(push(v));
        continue;
      }
      std::vector<double>& s = stack_;
      switch (b0) {
        case 1: case 3: case 18: case 23:  // stems
          take_width(s.size() % 2 == 0 ? s.size() : s.size() - 1);
          stems_ += s.size() / 2;
          s.clear();
          break;
        case 19: case 20: {  // hintmask / cntrmask
          take_width(s.size() % 2 == 0 ? s.size() : s.size() - 1);
          stems_ += s.size() / 2;
          s.clear();
          r.skip((stems_ + 7) / 8);
          if (!r.ok()) return fail(Status::Decode, "charstring hint mask is truncated");
          break;
        }
        case 21:
          take_width(2);
          if (s.size() < 2) return fail(Status::Decode, "rmoveto needs two arguments");
          move(s[s.size() - 2], s[s.size() - 1]);
          s.clear();
          break;
        case 22:
          take_width(1);
          if (s.empty()) return fail(Status::Decode, "hmoveto needs an argument");
          move(s.back(), 0);
          s.clear();
          break;
        case 4:
          take_width(1);
          if (s.empty()) return fail(Status::Decode, "vmoveto needs an argument");
          move(0, s.back());
          s.clear();
          break;
        case 5:
          for (std::size_t i = 0; i + 1 < s.size(); i += 2) line(s[i], s[i + 1]);
          s.clear();
          break;
        case 6: case 7: {
          bool horizontal = b0 == 6;
          for (const double v : s) {
            if (horizontal) line(v, 0);
            else line(0, v);
            horizontal = !horizontal;
          }
          s.clear();
          break;
        }
        case 8:
          for (std::size_t i = 0; i + 5 < s.size(); i += 6) curve(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
          s.clear();
          break;
        case 24: {  // rcurveline
          std::size_t i = 0;
          for (; i + 6 + 2 <= s.size(); i += 6) curve(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
          if (i + 1 < s.size()) line(s[i], s[i + 1]);
          s.clear();
          break;
        }
        case 25: {  // rlinecurve
          std::size_t i = 0;
          for (; i + 2 + 6 <= s.size(); i += 2) line(s[i], s[i + 1]);
          if (i + 5 < s.size()) curve(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
          s.clear();
          break;
        }
        case 26: {  // vvcurveto
          std::size_t i = 0;
          double dx1 = 0;
          if (s.size() % 4 == 1) dx1 = s[i++];
          for (; i + 3 < s.size(); i += 4) {
            curve(dx1, s[i], s[i + 1], s[i + 2], 0, s[i + 3]);
            dx1 = 0;
          }
          s.clear();
          break;
        }
        case 27: {  // hhcurveto
          std::size_t i = 0;
          double dy1 = 0;
          if (s.size() % 4 == 1) dy1 = s[i++];
          for (; i + 3 < s.size(); i += 4) {
            curve(s[i], dy1, s[i + 1], s[i + 2], s[i + 3], 0);
            dy1 = 0;
          }
          s.clear();
          break;
        }
        case 30: case 31: {  // vhcurveto / hvcurveto
          bool horizontal = b0 == 31;
          std::size_t i = 0;
          while (i + 3 < s.size()) {
            const bool last = s.size() - i == 5;
            const double extra = last ? s[i + 4] : 0;
            if (horizontal) curve(s[i], 0, s[i + 1], s[i + 2], extra, s[i + 3]);
            else curve(0, s[i], s[i + 1], s[i + 2], s[i + 3], extra);
            horizontal = !horizontal;
            i += last ? 5 : 4;
          }
          s.clear();
          break;
        }
        case 10: case 29: {  // callsubr / callgsubr
          if (s.empty()) return fail(Status::Decode, "subroutine call without an index");
          const std::vector<ByteView>& subrs = b0 == 10 ? priv_.subrs : font_.global_subrs;
          const double index = s.back() + subr_bias(subrs.size());
          s.pop_back();
          if (index < 0 || index >= static_cast<double>(subrs.size())) {
            return fail(Status::Decode, "charstring calls a missing subroutine");
          }
          CVT_TRY(execute(subrs[static_cast<std::size_t>(index)], depth + 1));
          if (ended_) return {};
          break;
        }
        case 11:  // return
          return {};
        case 14: {  // endchar
          if (font_.cff2) break;  // CFF2 has no endchar
          take_width(s.size() >= 4 ? 4 : 0);
          if (s.size() >= 4) {
            seac = true;
            seac_adx = s[s.size() - 4];
            seac_ady = s[s.size() - 3];
            seac_base = static_cast<int>(s[s.size() - 2]);
            seac_accent = static_cast<int>(s[s.size() - 1]);
          }
          if (open_) path_.close();
          open_ = false;
          ended_ = true;
          s.clear();
          return {};
        }
        case 15:  // vsindex (CFF2)
          if (!s.empty()) vsindex_ = static_cast<std::size_t>(std::max(0.0, s.back()));
          s.clear();
          break;
        case 16: {  // blend (CFF2): keep the default values
          if (s.empty()) return fail(Status::Decode, "blend without operands");
          const auto n = static_cast<std::size_t>(std::max(0.0, s.back()));
          s.pop_back();
          const std::size_t regions = vsindex_ < font_.region_counts.size() ? font_.region_counts[vsindex_] : 0;
          const std::size_t total = n * (regions + 1);
          if (total > s.size()) return fail(Status::Decode, "blend has too few operands");
          const std::size_t first = s.size() - total;
          s.resize(first + n);
          break;
        }
        case 12: {
          const std::uint8_t b1 = r.u8();
          if (!r.ok()) return fail(Status::Decode, "charstring is truncated");
          CVT_TRY(escape(b1));
          break;
        }
        default:
          return fail(Status::Decode, "charstring uses the reserved operator {}", b0);
      }
    }
    return {};
  }

  Result<void> escape(std::uint8_t op) {
    std::vector<double>& s = stack_;
    const auto need = [&](std::size_t n) -> Result<void> {
      if (s.size() < n) return fail(Status::Decode, "charstring operator 12 {} needs {} arguments", op, n);
      return {};
    };
    switch (op) {
      case 35: {  // flex
        CVT_TRY(need(13));
        const std::size_t b = s.size() - 13;
        curve(s[b], s[b + 1], s[b + 2], s[b + 3], s[b + 4], s[b + 5]);
        curve(s[b + 6], s[b + 7], s[b + 8], s[b + 9], s[b + 10], s[b + 11]);
        s.clear();
        break;
      }
      case 34: {  // hflex
        CVT_TRY(need(7));
        const std::size_t b = s.size() - 7;
        const double y0 = y_;
        curve(s[b], 0, s[b + 1], s[b + 2], s[b + 3], 0);
        curve(s[b + 4], 0, s[b + 5], y0 - y_, s[b + 6], 0);
        s.clear();
        break;
      }
      case 36: {  // hflex1
        CVT_TRY(need(9));
        const std::size_t b = s.size() - 9;
        const double y0 = y_;
        curve(s[b], s[b + 1], s[b + 2], s[b + 3], s[b + 4], 0);
        curve(s[b + 5], 0, s[b + 6], s[b + 7], s[b + 8], 0);
        // the final point returns to the starting y
        path_.points.back().y = y0;
        y_ = y0;
        s.clear();
        break;
      }
      case 37: {  // flex1
        CVT_TRY(need(11));
        const std::size_t b = s.size() - 11;
        double dx = 0, dy = 0;
        for (std::size_t i = 0; i < 10; i += 2) {
          dx += s[b + i];
          dy += s[b + i + 1];
        }
        curve(s[b], s[b + 1], s[b + 2], s[b + 3], s[b + 4], s[b + 5]);
        if (std::abs(dx) > std::abs(dy)) curve(s[b + 6], s[b + 7], s[b + 8], s[b + 9], s[b + 10], -dy);
        else curve(s[b + 6], s[b + 7], s[b + 8], s[b + 9], -dx, s[b + 10]);
        s.clear();
        break;
      }
      case 0: s.clear(); break;  // dotsection (obsolete)
      case 3: CVT_TRY(need(2)); s[s.size() - 2] = (s[s.size() - 2] != 0 && s.back() != 0); s.pop_back(); break;
      case 4: CVT_TRY(need(2)); s[s.size() - 2] = (s[s.size() - 2] != 0 || s.back() != 0); s.pop_back(); break;
      case 5: CVT_TRY(need(1)); s.back() = s.back() == 0; break;
      case 9: CVT_TRY(need(1)); s.back() = std::abs(s.back()); break;
      case 10: CVT_TRY(need(2)); s[s.size() - 2] += s.back(); s.pop_back(); break;
      case 11: CVT_TRY(need(2)); s[s.size() - 2] -= s.back(); s.pop_back(); break;
      case 12:
        CVT_TRY(need(2));
        s[s.size() - 2] = s.back() != 0 ? s[s.size() - 2] / s.back() : 0;
        s.pop_back();
        break;
      case 14: CVT_TRY(need(1)); s.back() = -s.back(); break;
      case 15: CVT_TRY(need(2)); s[s.size() - 2] = s[s.size() - 2] == s.back(); s.pop_back(); break;
      case 18: CVT_TRY(need(1)); s.pop_back(); break;
      case 20: {  // put
        CVT_TRY(need(2));
        const auto i = static_cast<std::size_t>(std::max(0.0, s.back()));
        if (i < transient_.size()) transient_[i] = s[s.size() - 2];
        s.resize(s.size() - 2);
        break;
      }
      case 21: {  // get
        CVT_TRY(need(1));
        const auto i = static_cast<std::size_t>(std::max(0.0, s.back()));
        s.back() = i < transient_.size() ? transient_[i] : 0;
        break;
      }
      case 22: {  // ifelse
        CVT_TRY(need(4));
        const double v2 = s[s.size() - 1], v1 = s[s.size() - 2], s2 = s[s.size() - 3], s1 = s[s.size() - 4];
        s.resize(s.size() - 4);
        s.push_back(v1 <= v2 ? s1 : s2);
        break;
      }
      case 23: CVT_TRY(push(0.5)); break;  // random: deterministic by design
      case 24: CVT_TRY(need(2)); s[s.size() - 2] *= s.back(); s.pop_back(); break;
      case 26: CVT_TRY(need(1)); s.back() = std::sqrt(std::max(0.0, s.back())); break;
      case 27: CVT_TRY(need(1)); CVT_TRY(push(s.back())); break;
      case 28: CVT_TRY(need(2)); std::swap(s[s.size() - 1], s[s.size() - 2]); break;
      case 29: {  // index
        CVT_TRY(need(1));
        double i = s.back();
        s.pop_back();
        if (i < 0) i = 0;
        const auto at = static_cast<std::size_t>(i);
        if (at >= s.size()) return fail(Status::Decode, "charstring index is out of range");
        s.push_back(s[s.size() - 1 - at]);
        break;
      }
      case 30: {  // roll
        CVT_TRY(need(2));
        const auto j = static_cast<long long>(s.back());
        const auto n = static_cast<long long>(s[s.size() - 2]);
        s.resize(s.size() - 2);
        if (n <= 0 || static_cast<std::size_t>(n) > s.size()) return fail(Status::Decode, "charstring roll is invalid");
        const auto first = s.end() - n;
        const long long shift = ((j % n) + n) % n;
        std::rotate(first, s.end() - shift, s.end());
        break;
      }
      default:
        s.clear();  // unknown operators (hints of old Type 2 variants) are ignored
        break;
    }
    return {};
  }

  const CffFont& font_;
  const CffPrivate& priv_;
  std::vector<double> stack_;
  std::size_t max_stack_ = 48;
  std::array<double, 32> transient_{};
  Path path_;
  double x_ = 0, y_ = 0;
  bool open_ = false;
  bool ended_ = false;
  bool width_taken_ = false;
  bool has_width_ = false;
  double width_ = 0;
  std::size_t ops_ = 0;
  std::size_t vsindex_ = 0;
};

std::optional<std::uint32_t> glyph_for_standard_code(const CffFont& font, int code) {
  if (code < 0 || code > 255) return std::nullopt;
  const std::string_view name = standard_encoding(static_cast<std::uint8_t>(code));
  if (name.empty()) return std::nullopt;
  for (std::size_t g = 0; g < font.charset.size(); ++g) {
    if (font.string_for(font.charset[g]) == name) return static_cast<std::uint32_t>(g);
  }
  return std::nullopt;
}

Result<CffGlyph> run_glyph(const CffFont& font, std::uint32_t glyph, int depth) {
  if (glyph >= font.charstrings.size()) return fail(Status::Decode, "glyph {} does not exist", glyph);
  const std::size_t fd = font.fd_select.empty() ? 0 : font.fd_select[glyph];
  static const CffPrivate empty_private;
  const CffPrivate& priv = fd < font.privates.size() ? font.privates[fd] : empty_private;
  Interpreter interpreter(font, priv);
  CVT_TRY_ASSIGN(CffGlyph result, interpreter.run(font.charstrings[glyph]));
  if (interpreter.seac) {
    if (depth > 0) return fail(Status::Decode, "accented glyphs are nested");
    const auto base = glyph_for_standard_code(font, interpreter.seac_base);
    const auto accent = glyph_for_standard_code(font, interpreter.seac_accent);
    if (!base || !accent) return fail(Status::Decode, "accented glyph {} refers to missing glyphs", glyph);
    CVT_TRY_ASSIGN(CffGlyph base_glyph, run_glyph(font, *base, depth + 1));
    CVT_TRY_ASSIGN(CffGlyph accent_glyph, run_glyph(font, *accent, depth + 1));
    accent_glyph.path.transform(1, 0, 0, 1, interpreter.seac_adx, interpreter.seac_ady);
    result.path = std::move(base_glyph.path);
    result.path.append(accent_glyph.path);
  }
  return result;
}

}  // namespace

Result<CffGlyph> cff_glyph(const CffFont& font, std::uint32_t glyph) { return run_glyph(font, glyph, 0); }

// --------------------------------------------------------------------------- writer

void cff_encode_dict_int(Bytes& out, std::int32_t v) {
  if (v >= -107 && v <= 107) {
    out.push_back(static_cast<std::byte>(v + 139));
  } else if (v >= 108 && v <= 1131) {
    v -= 108;
    out.push_back(static_cast<std::byte>((v >> 8) + 247));
    out.push_back(static_cast<std::byte>(v & 0xFF));
  } else if (v >= -1131 && v <= -108) {
    v = -v - 108;
    out.push_back(static_cast<std::byte>((v >> 8) + 251));
    out.push_back(static_cast<std::byte>(v & 0xFF));
  } else if (v >= -32768 && v <= 32767) {
    out.push_back(std::byte{28});
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(v & 0xFF));
  } else {
    out.push_back(std::byte{29});
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::byte>((v >> shift) & 0xFF));
  }
}

namespace {

void encode_int32_fixed_width(Bytes& out, std::int32_t v) {
  out.push_back(std::byte{29});
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::byte>((v >> shift) & 0xFF));
}

void encode_dict_real(Bytes& out, double value) {
  if (value == std::floor(value) && std::abs(value) < 1e9) {
    cff_encode_dict_int(out, static_cast<std::int32_t>(value));
    return;
  }
  std::string text = std::format("{:.8g}", value);
  std::vector<int> nibbles;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c >= '0' && c <= '9') nibbles.push_back(c - '0');
    else if (c == '.') nibbles.push_back(0xA);
    else if (c == '-') nibbles.push_back(0xE);
    else if (c == 'e' || c == 'E') {
      if (i + 1 < text.size() && text[i + 1] == '-') {
        nibbles.push_back(0xC);
        ++i;
      } else {
        nibbles.push_back(0xB);
        if (i + 1 < text.size() && text[i + 1] == '+') ++i;
      }
    }
  }
  nibbles.push_back(0xF);
  if (nibbles.size() % 2) nibbles.push_back(0xF);
  out.push_back(std::byte{30});
  for (std::size_t i = 0; i < nibbles.size(); i += 2) out.push_back(static_cast<std::byte>((nibbles[i] << 4) | nibbles[i + 1]));
}

void encode_op(Bytes& out, int op) {
  if (op >= 1200) {
    out.push_back(std::byte{12});
    out.push_back(static_cast<std::byte>(op - 1200));
  } else {
    out.push_back(static_cast<std::byte>(op));
  }
}

Bytes build_index(const std::vector<Bytes>& items) {
  util::ByteWriter out;
  out.u16be(static_cast<std::uint16_t>(items.size()));
  if (items.empty()) return out.take();
  std::uint64_t total = 1;
  for (const Bytes& item : items) total += item.size();
  const std::uint8_t off_size = total <= 0xFF ? 1 : total <= 0xFFFF ? 2 : total <= 0xFFFFFF ? 3 : 4;
  out.u8(off_size);
  std::uint64_t offset = 1;
  const auto put = [&](std::uint64_t value) {
    for (int i = off_size - 1; i >= 0; --i) out.u8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
  };
  put(offset);
  for (const Bytes& item : items) {
    offset += item.size();
    put(offset);
  }
  for (const Bytes& item : items) out.bytes(item);
  return out.take();
}

// Charstring number in Type 2 encoding (integers or 16.16 fixed).
void cs_number(Bytes& out, double value) {
  const double rounded = std::round(value);
  if (std::abs(value - rounded) < 1e-9 && std::abs(rounded) <= 32767) {
    const auto v = static_cast<std::int32_t>(rounded);
    if (v >= -107 && v <= 107) out.push_back(static_cast<std::byte>(v + 139));
    else if (v >= 108 && v <= 1131) {
      out.push_back(static_cast<std::byte>(((v - 108) >> 8) + 247));
      out.push_back(static_cast<std::byte>((v - 108) & 0xFF));
    } else if (v >= -1131 && v <= -108) {
      out.push_back(static_cast<std::byte>(((-v - 108) >> 8) + 251));
      out.push_back(static_cast<std::byte>((-v - 108) & 0xFF));
    } else {
      out.push_back(std::byte{28});
      out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
      out.push_back(static_cast<std::byte>(v & 0xFF));
    }
    return;
  }
  const auto fixed = static_cast<std::int32_t>(std::lround(value * 65536.0));
  out.push_back(std::byte{255});
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::byte>((fixed >> shift) & 0xFF));
}

}  // namespace

Bytes cff_charstring(const Path& input, std::optional<double> width, bool round_coordinates) {
  const Path path = input.to_cubic();
  Bytes out;
  std::size_t args = 0;
  bool first = true;
  if (width) {
    cs_number(out, *width);
    args = 1;
  }
  double x = 0, y = 0;
  const auto q = [&](double v) {
    return round_coordinates ? std::round(v) : std::round(v * 65536.0) / 65536.0;
  };
  // Pending batched operator: 5 = rlineto, 8 = rrcurveto.
  int pending = 0;
  const auto flush = [&]() {
    if (pending) out.push_back(static_cast<std::byte>(pending));
    pending = 0;
    args = 0;
  };
  std::size_t pi = 0;
  for (const Verb verb : path.verbs) {
    switch (verb) {
      case Verb::Move: {
        flush();
        const Point p{q(path.points[pi].x), q(path.points[pi].y)};
        ++pi;
        cs_number(out, p.x - x);
        cs_number(out, p.y - y);
        out.push_back(std::byte{21});
        if (first) first = false;
        args = 0;
        x = p.x;
        y = p.y;
        break;
      }
      case Verb::Line: {
        const Point p{q(path.points[pi].x), q(path.points[pi].y)};
        ++pi;
        if (pending != 5 || args + 2 > 46) flush();
        pending = 5;
        cs_number(out, p.x - x);
        cs_number(out, p.y - y);
        args += 2;
        x = p.x;
        y = p.y;
        break;
      }
      case Verb::Cubic: {
        if (pending != 8 || args + 6 > 42) flush();
        pending = 8;
        for (int k = 0; k < 3; ++k) {
          const Point p{q(path.points[pi + static_cast<std::size_t>(k)].x), q(path.points[pi + static_cast<std::size_t>(k)].y)};
          cs_number(out, p.x - x);
          cs_number(out, p.y - y);
          x = p.x;
          y = p.y;
        }
        pi += 3;
        args += 6;
        break;
      }
      case Verb::Quad: pi += 2; break;  // converted to cubic above
      case Verb::Close: flush(); break;
    }
  }
  flush();
  out.push_back(std::byte{14});  // endchar
  (void)first;
  return out;
}

Result<Bytes> build_cff(const CffBuildInput& input) {
  const std::size_t count = input.glyph_names.size();
  if (count == 0 || input.outlines.size() != count || input.advances.size() != count) {
    return fail(Status::Encode, "inconsistent glyph data for CFF output");
  }
  if (count > 65535) return fail(Status::Encode, "too many glyphs for CFF");
  // Strings.
  const auto standard = cff_standard_strings();
  std::map<std::string, int, std::less<>> standard_sid;
  for (std::size_t i = 0; i < standard.size(); ++i) standard_sid.emplace(std::string(standard[i]), static_cast<int>(i));
  std::vector<Bytes> custom_strings;
  std::map<std::string, int, std::less<>> custom_sid;
  const auto sid_of = [&](std::string_view text) {
    if (const auto it = standard_sid.find(text); it != standard_sid.end()) return it->second;
    if (const auto it = custom_sid.find(text); it != custom_sid.end()) return it->second;
    const int sid = static_cast<int>(standard.size() + custom_strings.size());
    custom_strings.push_back(to_bytes(text));
    custom_sid.emplace(std::string(text), sid);
    return sid;
  };
  std::vector<int> charset;
  for (std::size_t g = 1; g < count; ++g) charset.push_back(sid_of(input.glyph_names[g]));
  // Widths: the most common advance is the default; nominal = default.
  std::map<double, int> frequency;
  for (const double a : input.advances) ++frequency[std::round(a)];
  double default_width = 0;
  int best = -1;
  for (const auto& [value, n] : frequency) {
    if (n > best) {
      best = n;
      default_width = value;
    }
  }
  const double nominal_width = default_width;
  std::vector<Bytes> charstrings;
  charstrings.reserve(count);
  Bounds bbox;
  for (std::size_t g = 0; g < count; ++g) {
    const double advance = std::round(input.advances[g]);
    const std::optional<double> width =
        advance == default_width ? std::nullopt : std::optional<double>(advance - nominal_width);
    charstrings.push_back(cff_charstring(input.outlines[g], width, input.round_coordinates));
    const Bounds b = input.outlines[g].exact_bounds();
    if (!b.empty) {
      bbox.add({b.x_min, b.y_min});
      bbox.add({b.x_max, b.y_max});
    }
  }
  // Private DICT.
  Bytes priv;
  for (const auto& [op, values] : input.private_hints) {
    if (op == 19 || op == 20 || op == 21) continue;
    for (const double v : values) encode_dict_real(priv, v);
    encode_op(priv, op);
  }
  cff_encode_dict_int(priv, static_cast<std::int32_t>(default_width));
  encode_op(priv, 20);
  cff_encode_dict_int(priv, static_cast<std::int32_t>(nominal_width));
  encode_op(priv, 21);
  // Top DICT with fixed-width offsets so its size is known before layout.
  const auto make_top = [&](std::uint32_t charset_at, std::uint32_t charstrings_at, std::uint32_t private_at) {
    Bytes top;
    const auto string_entry = [&](const std::string& text, int op) {
      if (text.empty()) return;
      cff_encode_dict_int(top, sid_of(text));
      encode_op(top, op);
    };
    string_entry(input.version, 0);
    string_entry(input.notice, 1);
    string_entry(input.copyright, 1200);
    string_entry(input.full_name, 2);
    string_entry(input.family_name, 3);
    string_entry(input.weight, 4);
    if (input.fixed_pitch) {
      cff_encode_dict_int(top, 1);
      encode_op(top, 1201);
    }
    if (input.italic_angle != 0) {
      encode_dict_real(top, input.italic_angle);
      encode_op(top, 1202);
    }
    encode_dict_real(top, std::round(input.underline_position));
    encode_op(top, 1203);
    encode_dict_real(top, std::round(input.underline_thickness));
    encode_op(top, 1204);
    if (std::abs(input.units_per_em - 1000) > 1e-9) {
      encode_dict_real(top, 1.0 / input.units_per_em);
      for (int i = 0; i < 2; ++i) encode_dict_real(top, 0);
      encode_dict_real(top, 1.0 / input.units_per_em);
      for (int i = 0; i < 2; ++i) encode_dict_real(top, 0);
      encode_op(top, 1207);
    }
    for (const double v : {bbox.x_min, bbox.y_min, bbox.x_max, bbox.y_max}) {
      cff_encode_dict_int(top, static_cast<std::int32_t>(bbox.empty ? 0 : (v < 0 ? std::floor(v) : std::ceil(v))));
    }
    encode_op(top, 5);
    encode_int32_fixed_width(top, static_cast<std::int32_t>(charset_at));
    encode_op(top, 15);
    encode_int32_fixed_width(top, static_cast<std::int32_t>(charstrings_at));
    encode_op(top, 17);
    encode_int32_fixed_width(top, static_cast<std::int32_t>(priv.size()));
    encode_int32_fixed_width(top, static_cast<std::int32_t>(private_at));
    encode_op(top, 18);
    return top;
  };
  // First pass registers every string, so the String INDEX is final.
  (void)make_top(0, 0, 0);
  const Bytes name_index = build_index({to_bytes(input.postscript_name)});
  const Bytes string_index = build_index(custom_strings);
  const Bytes gsubr_index = build_index({});
  Bytes charset_bytes{std::byte{0}};
  for (const int sid : charset) {
    charset_bytes.push_back(static_cast<std::byte>(sid >> 8));
    charset_bytes.push_back(static_cast<std::byte>(sid & 0xFF));
  }
  const Bytes charstring_index = build_index(charstrings);
  const std::size_t top_size = make_top(0, 0, 0).size();
  const Bytes probe_top_index = build_index({Bytes(top_size)});
  const std::uint64_t charset_at = 4 + name_index.size() + probe_top_index.size() + string_index.size() + gsubr_index.size();
  const std::uint64_t charstrings_at = charset_at + charset_bytes.size();
  const std::uint64_t private_at = charstrings_at + charstring_index.size();
  if (private_at + priv.size() > 0x7FFFFFFF) return fail(Status::Encode, "the CFF table is too large");
  const Bytes top_index = build_index({make_top(static_cast<std::uint32_t>(charset_at),
                                                static_cast<std::uint32_t>(charstrings_at),
                                                static_cast<std::uint32_t>(private_at))});
  util::ByteWriter out;
  out.u8(1).u8(0).u8(4).u8(4);
  out.bytes(name_index).bytes(top_index).bytes(string_index).bytes(gsubr_index);
  out.bytes(charset_bytes).bytes(charstring_index).bytes(priv);
  return out.take();
}

namespace {

Bytes encode_dict(const CffDict& dict, std::initializer_list<int> skip) {
  Bytes out;
  for (const auto& [op, values] : dict) {
    if (std::ranges::find(skip, op) != skip.end()) continue;
    for (const double v : values) encode_dict_real(out, v);
    encode_op(out, op);
  }
  return out;
}

std::vector<Bytes> copy_items(const std::vector<ByteView>& items) {
  std::vector<Bytes> out;
  out.reserve(items.size());
  for (const ByteView item : items) out.emplace_back(item.begin(), item.end());
  return out;
}

}  // namespace

Result<Bytes> rebuild_cff(const CffFont& font, const std::vector<std::uint32_t>& glyphs,
                          const std::vector<Bytes>& replacement, bool keep_subrs) {
  if (font.cff2) return fail(Status::Encode, "CFF2 tables cannot be rebuilt");
  if (glyphs.empty() || glyphs[0] != 0) return fail(Status::Encode, "a CFF font must start with .notdef");
  std::vector<Bytes> charstrings;
  charstrings.reserve(glyphs.size());
  for (std::size_t i = 0; i < glyphs.size(); ++i) {
    if (glyphs[i] >= font.charstrings.size()) return fail(Status::Encode, "glyph {} does not exist", glyphs[i]);
    if (i < replacement.size() && !replacement[i].empty()) charstrings.push_back(replacement[i]);
    else charstrings.emplace_back(font.charstrings[glyphs[i]].begin(), font.charstrings[glyphs[i]].end());
  }
  Bytes charset{std::byte{0}};
  for (std::size_t i = 1; i < glyphs.size(); ++i) {
    const std::uint16_t sid = glyphs[i] < font.charset.size() ? font.charset[glyphs[i]] : 0;
    charset.push_back(static_cast<std::byte>(sid >> 8));
    charset.push_back(static_cast<std::byte>(sid & 0xFF));
  }
  const bool cid = !font.fd_dicts.empty();
  Bytes fd_select;
  if (cid) {
    fd_select.push_back(std::byte{0});
    for (const std::uint32_t g : glyphs) {
      fd_select.push_back(static_cast<std::byte>(font.fd_select.empty() ? 0 : font.fd_select[g]));
    }
  }
  // Private DICTs, each followed by its local subroutines.
  std::vector<Bytes> privates;
  for (const CffPrivate& p : font.privates) {
    Bytes dict = encode_dict(p.dict, {19});
    if (keep_subrs && !p.subrs.empty()) {
      encode_int32_fixed_width(dict, static_cast<std::int32_t>(dict.size() + 6));
      encode_op(dict, 19);
      const Bytes subrs = build_index(copy_items(p.subrs));
      dict.insert(dict.end(), subrs.begin(), subrs.end());
    }
    privates.push_back(std::move(dict));
  }
  const auto private_dict_size = [&](std::size_t i) {
    const CffPrivate& p = font.privates[i];
    std::size_t size = encode_dict(p.dict, {19}).size();
    if (keep_subrs && !p.subrs.empty()) size += 6;
    return size;
  };
  const Bytes name_index = build_index({to_bytes(font.name.empty() ? std::string("Untitled") : font.name)});
  const Bytes string_index = build_index(copy_items([&] {
    std::vector<ByteView> views;
    for (const std::string& s : font.strings) views.push_back(as_bytes(s));
    return views;
  }()));
  const Bytes gsubr_index = keep_subrs ? build_index(copy_items(font.global_subrs)) : build_index({});
  const Bytes charstring_index = build_index(charstrings);
  const auto make_top = [&](std::uint32_t charset_at, std::uint32_t select_at, std::uint32_t strings_at,
                            std::uint32_t fd_array_at, std::uint32_t private_at) {
    Bytes top;
    // ROS must come first in a CID-keyed Top DICT.
    if (const auto it = font.top.find(1230); it != font.top.end()) {
      for (const double v : it->second) encode_dict_real(top, v);
      encode_op(top, 1230);
    }
    const Bytes rest = encode_dict(font.top, {1230, 15, 16, 17, 18, 1236, 1237});
    top.insert(top.end(), rest.begin(), rest.end());
    encode_int32_fixed_width(top, static_cast<std::int32_t>(charset_at));
    encode_op(top, 15);
    encode_int32_fixed_width(top, static_cast<std::int32_t>(strings_at));
    encode_op(top, 17);
    if (cid) {
      encode_int32_fixed_width(top, static_cast<std::int32_t>(select_at));
      encode_op(top, 1237);
      encode_int32_fixed_width(top, static_cast<std::int32_t>(fd_array_at));
      encode_op(top, 1236);
    } else {
      encode_int32_fixed_width(top, static_cast<std::int32_t>(private_dict_size(0)));
      encode_int32_fixed_width(top, static_cast<std::int32_t>(private_at));
      encode_op(top, 18);
    }
    return top;
  };
  const Bytes probe_top = build_index({make_top(0, 0, 0, 0, 0)});
  std::uint64_t at = 4 + name_index.size() + probe_top.size() + string_index.size() + gsubr_index.size();
  const std::uint64_t charset_at = at;
  at += charset.size();
  const std::uint64_t select_at = at;
  at += fd_select.size();
  const std::uint64_t charstrings_at = at;
  at += charstring_index.size();
  std::vector<std::uint64_t> private_offsets;
  Bytes fd_array;
  if (cid) {
    const auto make_fd_array = [&](const std::vector<std::uint64_t>& offsets) {
      std::vector<Bytes> fds;
      for (std::size_t i = 0; i < font.fd_dicts.size(); ++i) {
        Bytes fd = encode_dict(font.fd_dicts[i], {18});
        const std::size_t p = std::min(i, font.privates.size() - 1);
        encode_int32_fixed_width(fd, static_cast<std::int32_t>(private_dict_size(p)));
        encode_int32_fixed_width(fd, static_cast<std::int32_t>(i < offsets.size() ? offsets[i] : 0));
        encode_op(fd, 18);
        fds.push_back(std::move(fd));
      }
      return build_index(fds);
    };
    const std::uint64_t fd_array_size = make_fd_array({}).size();
    const std::uint64_t fd_array_at = at;
    at += fd_array_size;
    for (std::size_t i = 0; i < font.fd_dicts.size(); ++i) {
      const std::size_t p = std::min(i, font.privates.size() - 1);
      private_offsets.push_back(at);
      at += privates[p].size();
    }
    fd_array = make_fd_array(private_offsets);
    if (at > 0x7FFFFFFF) return fail(Status::Encode, "the CFF table is too large");
    util::ByteWriter out;
    out.u8(1).u8(0).u8(4).u8(4);
    out.bytes(name_index)
        .bytes(build_index({make_top(static_cast<std::uint32_t>(charset_at), static_cast<std::uint32_t>(select_at),
                                     static_cast<std::uint32_t>(charstrings_at), static_cast<std::uint32_t>(fd_array_at), 0)}));
    out.bytes(string_index).bytes(gsubr_index).bytes(charset).bytes(fd_select).bytes(charstring_index).bytes(fd_array);
    for (std::size_t i = 0; i < font.fd_dicts.size(); ++i) out.bytes(privates[std::min(i, font.privates.size() - 1)]);
    return out.take();
  }
  const std::uint64_t private_at = at;
  at += privates.empty() ? 0 : privates[0].size();
  if (at > 0x7FFFFFFF || privates.empty()) return fail(Status::Encode, "the CFF table cannot be rebuilt");
  util::ByteWriter out;
  out.u8(1).u8(0).u8(4).u8(4);
  out.bytes(name_index)
      .bytes(build_index({make_top(static_cast<std::uint32_t>(charset_at), 0, static_cast<std::uint32_t>(charstrings_at), 0,
                                   static_cast<std::uint32_t>(private_at))}));
  out.bytes(string_index).bytes(gsubr_index).bytes(charset).bytes(charstring_index).bytes(privates[0]);
  return out.take();
}

}  // namespace cvt::font
