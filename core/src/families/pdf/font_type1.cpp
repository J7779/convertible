// Convertible core - pdf family: Type 1 fonts (eexec, Type 1 charstrings).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "families/pdf/font_data.hpp"
#include "families/pdf/font_program.hpp"
#include "families/pdf/syntax.hpp"
#include "util/text.hpp"

namespace cvt::pdf::font {

namespace {

Bytes decrypt(ByteView in, std::uint16_t r, std::size_t skip) {
  Bytes out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const auto c = std::to_integer<std::uint8_t>(in[i]);
    const auto p = static_cast<std::uint8_t>(c ^ (r >> 8));
    r = static_cast<std::uint16_t>((c + r) * 52845u + 22719u);
    if (i >= skip) out.push_back(static_cast<std::byte>(p));
  }
  return out;
}

bool is_hex(std::uint8_t c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string_view chars(ByteView d) { return as_chars(d); }

std::size_t util_parse_count(std::string_view word) {
  std::size_t value = 0;
  for (const char c : word) {
    if (c < '0' || c > '9') break;
    value = value * 10 + static_cast<std::size_t>(c - '0');
    if (value > (1u << 26)) return 0;
  }
  return value;
}

// Find "RD"/"-|" style binary token: after "<n> RD " comes n bytes.
struct BinaryCursor {
  std::string_view text;
  std::size_t pos = 0;

  void skip_space() {
    while (pos < text.size() && is_pdf_space(static_cast<std::uint8_t>(text[pos]))) ++pos;
  }
  std::string_view word() {
    skip_space();
    const std::size_t start = pos;
    if (pos < text.size() && text[pos] == '/') ++pos;
    while (pos < text.size() && !is_pdf_space(static_cast<std::uint8_t>(text[pos])) && text[pos] != '/' &&
           text[pos] != '[' && text[pos] != ']' && text[pos] != '{' && text[pos] != '}') {
      ++pos;
    }
    if (pos == start && pos < text.size()) ++pos;
    return text.substr(start, pos - start);
  }
};

}  // namespace

Result<Type1> Type1::parse(ByteView input, std::size_t length1) {
  Bytes joined;
  ByteView data = input;
  // PFB segments.
  if (data.size() > 6 && std::to_integer<std::uint8_t>(data[0]) == 0x80) {
    std::size_t p = 0;
    while (p + 6 <= data.size() && std::to_integer<std::uint8_t>(data[p]) == 0x80) {
      const int type = std::to_integer<int>(data[p + 1]);
      if (type == 3) break;
      const std::uint32_t len = std::to_integer<std::uint32_t>(data[p + 2]) |
                                (std::to_integer<std::uint32_t>(data[p + 3]) << 8) |
                                (std::to_integer<std::uint32_t>(data[p + 4]) << 16) |
                                (std::to_integer<std::uint32_t>(data[p + 5]) << 24);
      p += 6;
      if (len > data.size() - p) break;
      if (type == 1 && length1 == 0 && !joined.empty()) length1 = 0;
      if (type == 1 && joined.empty()) length1 = len;
      joined.insert(joined.end(), data.begin() + static_cast<std::ptrdiff_t>(p),
                    data.begin() + static_cast<std::ptrdiff_t>(p + len));
      p += len;
    }
    data = joined;
  }
  const std::string_view text = chars(data);
  std::size_t eexec = text.find("eexec");
  if (length1 == 0 || length1 > data.size()) {
    if (eexec == std::string_view::npos) return fail(Status::Decode, "Type 1 font without eexec section");
    length1 = eexec + 5;
  } else if (eexec != std::string_view::npos && eexec + 5 <= length1) {
    // Length1 is right when it points past "eexec".
  } else if (eexec != std::string_view::npos) {
    length1 = eexec + 5;
  }
  Type1 font;
  const std::string_view clear = text.substr(0, length1);
  // FontMatrix
  if (const std::size_t fm = clear.find("/FontMatrix"); fm != std::string_view::npos) {
    Lexer lexer(data.first(length1), fm + 11);
    bool ok = true;
    const Object m = parse_object(lexer, 4, &ok);
    const Array& a = m.as_array();
    if (a.size() == 6) {
      font.matrix_ = Matrix{a[0].as_number(), a[1].as_number(), a[2].as_number(), a[3].as_number(),
                            a[4].as_number(), a[5].as_number()};
      if (!font.matrix_.invertible()) font.matrix_ = Matrix{0.001, 0, 0, 0.001, 0, 0};
    }
  }
  if (const std::size_t fn = clear.find("/FontName"); fn != std::string_view::npos) {
    Lexer lexer(data.first(length1), fn + 9);
    const Token t = lexer.next();
    if (t.kind == Token::Kind::Name) font.name_ = t.text;
  }
  // Encoding
  for (int c = 0; c < 256; ++c) font.encoding_[static_cast<std::size_t>(c)] =
      std::string(encoding_glyph_name(BaseEncoding::Standard, static_cast<std::uint8_t>(c)));
  if (const std::size_t en = clear.find("/Encoding"); en != std::string_view::npos) {
    const std::string_view rest = clear.substr(en + 9);
    if (!rest.substr(0, 40).contains("StandardEncoding")) {
      for (auto& n : font.encoding_) n.clear();
      std::size_t pos = 0;
      const std::size_t end = rest.find("readonly def") != std::string_view::npos ? rest.find("readonly def")
                                                                                   : rest.find(" def");
      const std::string_view body = rest.substr(0, end == std::string_view::npos ? rest.size() : end);
      while ((pos = body.find("dup ", pos)) != std::string_view::npos) {
        pos += 4;
        Lexer lexer(as_bytes(body), pos);
        const Token code = lexer.next();
        const Token name = lexer.next();
        const Token put = lexer.next();
        if (code.kind == Token::Kind::Integer && code.integer >= 0 && code.integer < 256 &&
            name.kind == Token::Kind::Name && put.kind == Token::Kind::Keyword && put.text == "put") {
          font.encoding_[static_cast<std::size_t>(code.integer)] = name.text;
        }
      }
    }
  }
  // eexec section: skip whitespace, detect hex.
  std::size_t start = length1;
  while (start < data.size() && is_pdf_space(std::to_integer<std::uint8_t>(data[start]))) ++start;
  ByteView encrypted = data.subspan(std::min(start, data.size()));
  Bytes hex_decoded;
  bool is_hex_section = encrypted.size() >= 4;
  for (std::size_t i = 0; i < 4 && i < encrypted.size(); ++i) {
    is_hex_section = is_hex_section && is_hex(std::to_integer<std::uint8_t>(encrypted[i]));
  }
  if (is_hex_section) {
    int high = -1;
    for (const std::byte b : encrypted) {
      const auto c = std::to_integer<std::uint8_t>(b);
      if (!is_hex(c)) {
        if (is_pdf_space(c)) continue;
        break;
      }
      const int v = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
      if (high < 0) {
        high = v;
      } else {
        hex_decoded.push_back(static_cast<std::byte>(high * 16 + v));
        high = -1;
      }
    }
    encrypted = hex_decoded;
  }
  const Bytes priv = decrypt(encrypted, 55665, 4);
  const std::string_view ptext = chars(priv);
  int len_iv = 4;
  if (const std::size_t li = ptext.find("/lenIV"); li != std::string_view::npos) {
    Lexer lexer(priv, li + 6);
    const Token t = lexer.next();
    if (t.kind == Token::Kind::Integer) len_iv = static_cast<int>(std::clamp<std::int64_t>(t.integer, -1, 64));
  }
  const auto charstring = [&](ByteView raw) {
    if (len_iv < 0) return Bytes(raw.begin(), raw.end());
    return decrypt(raw, 4330, static_cast<std::size_t>(len_iv));
  };
  // Subrs: "dup <i> <n> RD <bytes> NP"
  if (const std::size_t sp = ptext.find("/Subrs"); sp != std::string_view::npos) {
    BinaryCursor cur{ptext, sp + 6};
    const auto count = util_parse_count(cur.word());
    (void)count;
    const std::size_t limit = std::min<std::size_t>(count, 65536);
    font.subrs_.resize(limit);
    const std::size_t cs_pos = ptext.find("/CharStrings", cur.pos);
    for (std::size_t k = 0; k < limit; ++k) {
      std::size_t dup = ptext.find("dup", cur.pos);
      if (dup == std::string_view::npos || (cs_pos != std::string_view::npos && dup > cs_pos)) break;
      cur.pos = dup + 3;
      const std::string_view index_word = cur.word();
      const std::string_view length_word = cur.word();
      cur.word();  // RD or -|
      const auto index = util_parse_count(index_word);
      const auto length = util_parse_count(length_word);
      ++cur.pos;  // single space before binary data
      if (cur.pos + length > priv.size()) break;
      if (index < font.subrs_.size()) font.subrs_[index] = charstring(ByteView(priv).subspan(cur.pos, length));
      cur.pos += length;
    }
  }
  // CharStrings: "/<name> <n> RD <bytes> ND"
  if (const std::size_t cp = ptext.find("/CharStrings"); cp != std::string_view::npos) {
    BinaryCursor cur{ptext, cp + 12};
    const std::size_t count = std::min<std::size_t>(util_parse_count(cur.word()), 65536);
    std::size_t found = 0;
    while (found < count && cur.pos < ptext.size()) {
      const std::size_t slash = ptext.find('/', cur.pos);
      if (slash == std::string_view::npos) break;
      cur.pos = slash;
      std::string_view name = cur.word();
      if (name.empty()) break;
      name.remove_prefix(1);
      const std::string_view length_word = cur.word();
      if (length_word.empty() || !util::is_ascii_digit(length_word[0])) continue;
      cur.word();
      const auto length = util_parse_count(length_word);
      ++cur.pos;
      if (cur.pos + length > priv.size()) break;
      font.charstrings_.emplace(std::string(name), charstring(ByteView(priv).subspan(cur.pos, length)));
      cur.pos += length;
      ++found;
    }
  }
  if (font.charstrings_.empty()) return fail(Status::Decode, "Type 1 font has no glyphs");
  return font;
}

bool Type1::has_glyph(std::string_view name) const { return charstrings_.contains(name); }

std::vector<std::string> Type1::glyph_names() const {
  std::vector<std::string> names;
  for (const auto& [name, cs] : charstrings_) names.push_back(name);
  return names;
}

namespace {

struct T1State {
  const std::vector<Bytes>* subrs;
  Path* path;
  std::vector<double> stack;
  std::vector<double> ps_stack;
  double x = 0, y = 0;
  double sbx = 0, sby = 0, width = 0;
  bool open = false;
  bool flex = false;
  std::vector<Point> flex_points;
  int depth = 0;
  std::size_t ops = 0;
  bool ended = false;
  bool seac = false;
  double asb = 0, adx = 0, ady = 0;
  int bchar = 0, achar = 0;
};

void run_t1(T1State& st, ByteView cs) {
  if (++st.depth > 12) return;
  auto& s = st.stack;
  const auto move_to = [&](double nx, double ny) {
    st.x = nx;
    st.y = ny;
    if (st.flex) {
      st.flex_points.push_back({nx, ny});
      return;
    }
    if (st.open) st.path->close();
    st.path->move_to(nx, ny);
    st.open = true;
  };
  const auto line_to = [&](double nx, double ny) {
    if (!st.open) move_to(st.x, st.y);
    st.x = nx;
    st.y = ny;
    st.path->line_to(nx, ny);
  };
  std::size_t p = 0;
  while (p < cs.size() && !st.ended) {
    if (++st.ops > 200000) return;
    const auto b0 = std::to_integer<std::uint8_t>(cs[p]);
    if (b0 >= 32) {
      if (b0 <= 246) {
        s.push_back(b0 - 139);
        ++p;
      } else if (b0 <= 250) {
        if (p + 1 >= cs.size()) return;
        s.push_back((b0 - 247) * 256 + std::to_integer<int>(cs[p + 1]) + 108);
        p += 2;
      } else if (b0 <= 254) {
        if (p + 1 >= cs.size()) return;
        s.push_back(-(b0 - 251) * 256 - std::to_integer<int>(cs[p + 1]) - 108);
        p += 2;
      } else {
        if (p + 4 >= cs.size()) return;
        std::uint32_t v = 0;
        for (int k = 1; k <= 4; ++k) v = (v << 8) | std::to_integer<std::uint32_t>(cs[p + static_cast<std::size_t>(k)]);
        s.push_back(static_cast<std::int32_t>(v));
        p += 5;
      }
      if (s.size() > 64) s.erase(s.begin());
      continue;
    }
    ++p;
    const auto arg = [&](std::size_t i) { return i < s.size() ? s[i] : 0.0; };
    switch (b0) {
      case 13:  // hsbw
        st.sbx = arg(0);
        st.width = arg(1);
        st.x = st.sbx;
        st.y = 0;
        s.clear();
        break;
      case 21: move_to(st.x + arg(0), st.y + arg(1)); s.clear(); break;
      case 22: move_to(st.x + arg(0), st.y); s.clear(); break;
      case 4: move_to(st.x, st.y + arg(0)); s.clear(); break;
      case 5: line_to(st.x + arg(0), st.y + arg(1)); s.clear(); break;
      case 6: line_to(st.x + arg(0), st.y); s.clear(); break;
      case 7: line_to(st.x, st.y + arg(0)); s.clear(); break;
      case 8: case 30: case 31: {
        double dx1, dy1, dx2, dy2, dx3, dy3;
        if (b0 == 8) {
          dx1 = arg(0); dy1 = arg(1); dx2 = arg(2); dy2 = arg(3); dx3 = arg(4); dy3 = arg(5);
        } else if (b0 == 30) {
          dx1 = 0; dy1 = arg(0); dx2 = arg(1); dy2 = arg(2); dx3 = arg(3); dy3 = 0;
        } else {
          dx1 = arg(0); dy1 = 0; dx2 = arg(1); dy2 = arg(2); dx3 = 0; dy3 = arg(3);
        }
        if (!st.open) move_to(st.x, st.y);
        const double x1 = st.x + dx1, y1 = st.y + dy1, x2 = x1 + dx2, y2 = y1 + dy2;
        st.x = x2 + dx3;
        st.y = y2 + dy3;
        st.path->cubic_to(x1, y1, x2, y2, st.x, st.y);
        s.clear();
        break;
      }
      case 9:  // closepath
        if (st.open) st.path->close();
        st.open = false;
        s.clear();
        break;
      case 10: {  // callsubr
        if (s.empty()) return;
        const auto index = static_cast<long long>(s.back());
        s.pop_back();
        if (index >= 0 && static_cast<std::size_t>(index) < st.subrs->size()) {
          run_t1(st, (*st.subrs)[static_cast<std::size_t>(index)]);
          --st.depth;
        }
        break;
      }
      case 11: return;
      case 14:  // endchar
        if (st.open) st.path->close();
        st.open = false;
        st.ended = true;
        return;
      case 1: case 3: s.clear(); break;  // hints
      case 12: {
        if (p >= cs.size()) return;
        const auto b1 = std::to_integer<std::uint8_t>(cs[p++]);
        switch (b1) {
          case 0: case 1: case 2: s.clear(); break;  // dotsection, vstem3, hstem3
          case 6:  // seac
            st.seac = true;
            st.asb = arg(0);
            st.adx = arg(1);
            st.ady = arg(2);
            st.bchar = static_cast<int>(arg(3));
            st.achar = static_cast<int>(arg(4));
            st.ended = true;
            return;
          case 7:  // sbw
            st.sbx = arg(0);
            st.sby = arg(1);
            st.width = arg(2);
            st.x = st.sbx;
            st.y = st.sby;
            s.clear();
            break;
          case 12: {  // div
            if (s.size() >= 2) {
              const double b = s.back();
              s.pop_back();
              s.back() = b != 0 ? s.back() / b : 0;
            }
            break;
          }
          case 16: {  // callothersubr
            if (s.size() < 2) {
              s.clear();
              break;
            }
            const int other = static_cast<int>(s.back());
            s.pop_back();
            const int n = static_cast<int>(s.back());
            s.pop_back();
            std::vector<double> args;
            for (int k = 0; k < n && !s.empty(); ++k) {
              args.insert(args.begin(), s.back());
              s.pop_back();
            }
            if (other == 1) {
              st.flex = true;
              st.flex_points.clear();
            } else if (other == 2) {
              // point recorded by the preceding rmoveto
            } else if (other == 0 && st.flex) {
              st.flex = false;
              // 7 points: reference + 6 curve points.
              if (st.flex_points.size() >= 7) {
                const auto& f = st.flex_points;
                if (!st.open) {
                  st.path->move_to(st.x, st.y);
                  st.open = true;
                }
                st.path->cubic_to(f[1].x, f[1].y, f[2].x, f[2].y, f[3].x, f[3].y);
                st.path->cubic_to(f[4].x, f[4].y, f[5].x, f[5].y, f[6].x, f[6].y);
                st.x = f[6].x;
                st.y = f[6].y;
              }
              st.ps_stack = {st.y, st.x};
              continue;
            } else if (other == 3) {
              st.ps_stack = {3};
              continue;
            }
            st.ps_stack.assign(args.rbegin(), args.rend());
            break;
          }
          case 17:  // pop
            if (!st.ps_stack.empty()) {
              s.push_back(st.ps_stack.back());
              st.ps_stack.pop_back();
            } else {
              s.push_back(0);
            }
            break;
          case 33:  // setcurrentpoint
            st.x = arg(0);
            st.y = arg(1);
            s.clear();
            break;
          default: s.clear(); break;
        }
        break;
      }
      default: s.clear(); break;
    }
  }
}

}  // namespace

Path Type1::outline(std::string_view glyph_name) const {
  Path path;
  const auto it = charstrings_.find(glyph_name);
  if (it == charstrings_.end()) return path;
  T1State st;
  st.subrs = &subrs_;
  st.path = &path;
  run_t1(st, it->second);
  if (st.open) path.close();
  if (st.seac) {
    Path composed;
    const std::string_view base = encoding_glyph_name(BaseEncoding::Standard, static_cast<std::uint8_t>(st.bchar & 0xFF));
    const std::string_view accent =
        encoding_glyph_name(BaseEncoding::Standard, static_cast<std::uint8_t>(st.achar & 0xFF));
    if (!base.empty() && base != glyph_name) composed.append(outline(base));
    if (!accent.empty() && accent != glyph_name) {
      composed.append(outline(accent), Matrix::translate(st.adx - st.asb, st.ady));
    }
    return composed;
  }
  return path;
}

double Type1::advance(std::string_view glyph_name) const {
  const auto it = charstrings_.find(glyph_name);
  if (it == charstrings_.end()) return 0;
  Path scratch;
  T1State st;
  st.subrs = &subrs_;
  st.path = &scratch;
  run_t1(st, it->second);
  return st.width;
}

}  // namespace cvt::pdf::font
