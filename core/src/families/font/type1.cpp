// Convertible core - font family: PostScript Type 1 fonts (PFA/PFB) and AFM metrics.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/font/type1.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "families/font/build.hpp"
#include "families/font/cff.hpp"
#include "families/font/sfnt.hpp"
#include "families/font/tables.hpp"
#include "util/text.hpp"

namespace cvt::font {

namespace {

constexpr std::uint16_t eexec_key = 55665;
constexpr std::uint16_t charstring_key = 4330;

Bytes decrypt(ByteView data, std::uint16_t key, std::size_t skip) {
  Bytes out;
  out.reserve(data.size());
  std::uint16_t r = key;
  for (std::size_t i = 0; i < data.size(); ++i) {
    const auto c = std::to_integer<std::uint8_t>(data[i]);
    const auto p = static_cast<std::uint8_t>(c ^ (r >> 8));
    r = static_cast<std::uint16_t>((c + r) * 52845u + 22719u);
    if (i >= skip) out.push_back(static_cast<std::byte>(p));
  }
  return out;
}

Bytes encrypt(ByteView plain, std::uint16_t key) {
  Bytes out;
  out.reserve(plain.size());
  std::uint16_t r = key;
  for (const std::byte b : plain) {
    const auto c = static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(b) ^ (r >> 8));
    r = static_cast<std::uint16_t>((c + r) * 52845u + 22719u);
    out.push_back(static_cast<std::byte>(c));
  }
  return out;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Split a Type 1 font into its clear text and its decrypted private part.
Result<std::pair<std::string, Bytes>> split_font(ByteView data) {
  std::string clear;
  Bytes encrypted;
  if (data.size() >= 6 && std::to_integer<unsigned>(data[0]) == 0x80) {
    util::ByteReader r(data);
    for (int guard = 0; guard < 4096 && !r.at_end(); ++guard) {
      if (r.u8() != 0x80) return fail(Status::Decode, "a PFB segment header is malformed");
      const std::uint8_t type = r.u8();
      if (type == 3) break;
      const std::uint32_t length = r.u32le();
      const ByteView segment = r.bytes(length);
      if (!r.ok()) return fail(Status::Decode, "a PFB segment is truncated");
      if (type == 1) {
        if (encrypted.empty()) clear += as_chars(segment);
      } else if (type == 2) {
        append(encrypted, segment);
      } else {
        return fail(Status::Decode, "unknown PFB segment type {}", type);
      }
    }
  } else {
    const std::string_view text = as_chars(data);
    const std::size_t eexec = text.find("currentfile eexec");
    if (eexec == std::string_view::npos) return fail(Status::Decode, "the Type 1 font has no encrypted part");
    std::size_t at = eexec + 17;
    clear = std::string(text.substr(0, at));
    while (at < text.size() && util::is_ascii_space(text[at])) ++at;
    // Hexadecimal (PFA) or raw binary eexec data.
    bool hex = at + 4 <= text.size();
    for (std::size_t k = at; k < at + 4 && k < text.size(); ++k) hex &= hex_value(text[k]) >= 0;
    if (hex) {
      int high = -1;
      for (; at < text.size(); ++at) {
        const char c = text[at];
        if (util::is_ascii_space(c)) continue;
        const int v = hex_value(c);
        if (v < 0) break;
        if (high < 0) {
          high = v;
        } else {
          encrypted.push_back(static_cast<std::byte>((high << 4) | v));
          high = -1;
        }
      }
    } else {
      append(encrypted, data.subspan(at));
    }
  }
  if (encrypted.size() < 4) return fail(Status::Decode, "the Type 1 font has no encrypted part");
  return std::pair{std::move(clear), decrypt(encrypted, eexec_key, 4)};
}

// ---- a small PostScript tokenizer for font dictionaries

struct Token {
  enum Kind { Name, Literal, Number, String, Open, Close, OpenBrace, CloseBrace, Other } kind = Other;
  std::string text;
  double number = 0;
};

class Tokenizer {
 public:
  explicit Tokenizer(std::string_view text) : text_(text) {}
  [[nodiscard]] bool at_end() {
    skip_space();
    return pos_ >= text_.size();
  }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  void seek(std::size_t pos) noexcept { pos_ = std::min(pos, text_.size()); }
  Token next() {
    skip_space();
    Token t;
    if (pos_ >= text_.size()) return t;
    const char c = text_[pos_];
    if (c == '(') {
      t.kind = Token::String;
      int depth = 0;
      ++pos_;
      while (pos_ < text_.size()) {
        char ch = text_[pos_++];
        if (ch == '\\' && pos_ < text_.size()) {
          const char e = text_[pos_++];
          if (e >= '0' && e <= '7') {
            int v = e - '0';
            for (int k = 0; k < 2 && pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '7'; ++k) v = v * 8 + (text_[pos_++] - '0');
            ch = static_cast<char>(v);
          } else {
            ch = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e;
          }
          t.text.push_back(ch);
          continue;
        }
        if (ch == '(') ++depth;
        if (ch == ')' && depth-- == 0) break;
        t.text.push_back(ch);
        if (t.text.size() > 65536) break;
      }
      return t;
    }
    if (c == '[' || c == ']' || c == '{' || c == '}') {
      ++pos_;
      t.kind = c == '[' ? Token::Open : c == ']' ? Token::Close : c == '{' ? Token::OpenBrace : Token::CloseBrace;
      t.text = std::string(1, c);
      return t;
    }
    const std::size_t start = pos_;
    if (c == '/') ++pos_;
    while (pos_ < text_.size() && !util::is_ascii_space(text_[pos_]) &&
           std::string_view("[]{}()/<>%").find(text_[pos_]) == std::string_view::npos) {
      ++pos_;
    }
    if (pos_ == start) ++pos_;  // a lone delimiter such as '<'
    t.text = std::string(text_.substr(start, pos_ - start));
    if (c == '/') {
      t.kind = Token::Literal;
      t.text.erase(0, 1);
    } else if (const auto v = util::parse_double(t.text); v && (util::is_ascii_digit(c) || c == '-' || c == '.' || c == '+')) {
      t.kind = Token::Number;
      t.number = *v;
    } else {
      t.kind = Token::Name;
    }
    return t;
  }
  /// Raw bytes after the current position (binary charstring data).
  std::string_view take(std::size_t count) {
    if (pos_ < text_.size()) ++pos_;  // the single space after RD
    const std::string_view out = text_.substr(pos_, std::min(count, text_.size() - pos_));
    pos_ += out.size();
    return out;
  }

 private:
  void skip_space() {
    while (pos_ < text_.size()) {
      if (util::is_ascii_space(text_[pos_]) || text_[pos_] == '\0') {
        ++pos_;
      } else if (text_[pos_] == '%') {
        while (pos_ < text_.size() && text_[pos_] != '\n' && text_[pos_] != '\r') ++pos_;
      } else {
        break;
      }
    }
  }
  std::string_view text_;
  std::size_t pos_ = 0;
};

struct Type1Data {
  std::map<std::string, std::string> info;      // FontInfo and top-level strings / names
  std::map<std::string, std::vector<double>> numbers;  // arrays and numbers
  std::map<std::uint8_t, std::string> encoding;  // custom encoding
  bool standard_encoding = false;
  int len_iv = 4;
  std::vector<Bytes> subrs;
  std::vector<std::pair<std::string, Bytes>> charstrings;
};

void parse_clear(std::string_view text, Type1Data& font) {
  Tokenizer t(text);
  std::string pending;  // last literal name
  std::vector<double> array;
  bool in_array = false;
  while (!t.at_end()) {
    Token token = t.next();
    if (token.kind == Token::Literal) {
      if (pending == "Encoding" && token.text == "StandardEncoding") font.standard_encoding = true;
      if (!pending.empty() && (pending == "FontName")) font.info[pending] = token.text;
      pending = token.text;
      in_array = false;
      array.clear();
      continue;
    }
    if (token.kind == Token::String && !pending.empty()) {
      font.info[pending] = token.text;
      continue;
    }
    if (token.kind == Token::Open || token.kind == Token::OpenBrace) {
      in_array = true;
      array.clear();
      continue;
    }
    if ((token.kind == Token::Close || token.kind == Token::CloseBrace) && in_array) {
      if (!pending.empty()) font.numbers[pending] = array;
      in_array = false;
      continue;
    }
    if (token.kind == Token::Number) {
      if (in_array) array.push_back(token.number);
      else if (!pending.empty()) font.numbers[pending] = {token.number};
      continue;
    }
    if (token.kind == Token::Name) {
      if (token.text == "true" || token.text == "false") {
        if (!pending.empty()) font.numbers[pending] = {token.text == "true" ? 1.0 : 0.0};
      } else if (token.text == "dup" && pending == "Encoding") {
        const std::size_t save = t.position();
        const Token code = t.next();
        const Token name = t.next();
        const Token put = t.next();
        if (code.kind == Token::Number && name.kind == Token::Literal && put.text == "put" && code.number >= 0 &&
            code.number <= 255) {
          font.encoding[static_cast<std::uint8_t>(code.number)] = name.text;
        } else {
          t.seek(save);
        }
      } else if (token.text == "eexec") {
        break;
      }
    }
  }
}

Result<void> parse_private(std::string_view text, Type1Data& font, const Context& ctx) {
  Tokenizer t(text);
  std::string pending;
  std::vector<double> array;
  bool in_array = false;
  std::vector<Token> recent;
  bool in_charstrings = false;
  while (!t.at_end()) {
    Token token = t.next();
    if (token.kind == Token::Name && (token.text == "RD" || token.text == "-|") && recent.size() >= 2) {
      const Token& length = recent.back();
      if (length.kind != Token::Number || length.number < 0 || length.number > 65535) {
        return fail(Status::Decode, "a Type 1 charstring has an invalid length");
      }
      const std::string_view raw = t.take(static_cast<std::size_t>(length.number));
      if (raw.size() != static_cast<std::size_t>(length.number)) return fail(Status::Decode, "a Type 1 charstring is truncated");
      const Bytes plain = font.len_iv < 0 ? to_bytes(raw) : decrypt(as_bytes(raw), charstring_key, static_cast<std::size_t>(font.len_iv));
      const Token& key = recent[recent.size() - 2];
      if (in_charstrings && key.kind == Token::Literal) {
        font.charstrings.emplace_back(key.text, plain);
        if (font.charstrings.size() > 65535) return fail(Status::Decode, "the Type 1 font has too many glyphs");
        if ((font.charstrings.size() & 255) == 0) CVT_TRY(ctx.checkpoint(0.5, "Reading charstrings"));
      } else if (!in_charstrings && key.kind == Token::Number && key.number >= 0 && key.number < 65536) {
        const auto index = static_cast<std::size_t>(key.number);
        if (index >= font.subrs.size()) font.subrs.resize(index + 1);
        font.subrs[index] = plain;
      }
      recent.clear();
      continue;
    }
    if (token.kind == Token::Literal) {
      if (token.text == "CharStrings") in_charstrings = true;
      if (!in_charstrings) {
        pending = token.text;
        in_array = false;
      }
    } else if (!in_charstrings) {
      if (token.kind == Token::Open || token.kind == Token::OpenBrace) {
        in_array = true;
        array.clear();
      } else if ((token.kind == Token::Close || token.kind == Token::CloseBrace) && in_array) {
        if (!pending.empty() && pending != "Subrs") font.numbers[pending] = array;
        in_array = false;
      } else if (token.kind == Token::Number) {
        if (in_array) array.push_back(token.number);
        else if (pending == "lenIV") font.len_iv = static_cast<int>(token.number);
        else if (!pending.empty() && pending != "Subrs") font.numbers[pending] = {token.number};
      }
    }
    recent.push_back(std::move(token));
    if (recent.size() > 4) recent.erase(recent.begin());
  }
  return {};
}

// ---- Type 1 charstring interpreter

struct Type1Glyph {
  Path path;
  double width = 0;
  double sbx = 0;
};

class Type1Interpreter {
 public:
  Type1Interpreter(const Type1Data& font, const std::map<std::string, std::size_t>& index) : font_(font), index_(index) {}

  Result<Type1Glyph> run(std::size_t glyph, int depth) {
    if (depth > 2) return fail(Status::Decode, "accented Type 1 glyphs are nested");
    CVT_TRY(execute(font_.charstrings[glyph].second, 0));
    if (open_) path_.close();
    Type1Glyph out{std::move(path_), width_, sbx_};
    if (seac_) {
      const std::string_view base_name = standard_encoding(static_cast<std::uint8_t>(seac_base_));
      const std::string_view accent_name = standard_encoding(static_cast<std::uint8_t>(seac_accent_));
      const auto base = index_.find(std::string(base_name));
      const auto accent = index_.find(std::string(accent_name));
      if (base == index_.end() || accent == index_.end()) return fail(Status::Decode, "an accented Type 1 glyph refers to missing glyphs");
      Type1Interpreter bi(font_, index_), ai(font_, index_);
      CVT_TRY_ASSIGN(Type1Glyph b, bi.run(base->second, depth + 1));
      CVT_TRY_ASSIGN(Type1Glyph a, ai.run(accent->second, depth + 1));
      a.path.transform(1, 0, 0, 1, seac_adx_ - seac_asb_ + sbx_, seac_ady_);
      out.path = std::move(b.path);
      out.path.append(a.path);
    }
    return out;
  }

 private:
  Result<void> execute(ByteView code, int depth) {
    if (depth > 10) return fail(Status::Decode, "Type 1 subroutines nest too deeply");
    util::ByteReader r(code);
    while (!r.at_end()) {
      if (++ops_ > 100000) return fail(Status::Decode, "a Type 1 charstring is too long");
      const std::uint8_t v = r.u8();
      if (v >= 32) {
        double n = 0;
        if (v <= 246) n = v - 139;
        else if (v <= 250) n = (v - 247) * 256 + r.u8() + 108;
        else if (v <= 254) n = -(v - 251) * 256 - r.u8() - 108;
        else n = r.i32be();
        if (!r.ok()) return fail(Status::Decode, "a Type 1 charstring is truncated");
        if (stack_.size() >= 64) return fail(Status::Decode, "Type 1 charstring stack overflow");
        stack_.push_back(n);
        continue;
      }
      auto& s = stack_;
      const auto need = [&](std::size_t n) -> Result<void> {
        if (s.size() < n) return fail(Status::Decode, "Type 1 operator {} needs {} arguments", v, n);
        return {};
      };
      switch (v) {
        case 1: case 3: s.clear(); break;  // hints
        case 4: CVT_TRY(need(1)); rmove(0, s.back()); s.clear(); break;
        case 5: CVT_TRY(need(2)); line(s[s.size() - 2], s.back()); s.clear(); break;
        case 6: CVT_TRY(need(1)); line(s.back(), 0); s.clear(); break;
        case 7: CVT_TRY(need(1)); line(0, s.back()); s.clear(); break;
        case 8: {
          CVT_TRY(need(6));
          const std::size_t b = s.size() - 6;
          curve(s[b], s[b + 1], s[b + 2], s[b + 3], s[b + 4], s[b + 5]);
          s.clear();
          break;
        }
        case 9:
          if (open_) path_.close();
          open_ = false;
          s.clear();
          break;
        case 10: {
          CVT_TRY(need(1));
          const double index = s.back();
          s.pop_back();
          if (index < 0 || index >= static_cast<double>(font_.subrs.size())) return fail(Status::Decode, "a Type 1 charstring calls a missing subroutine");
          CVT_TRY(execute(font_.subrs[static_cast<std::size_t>(index)], depth + 1));
          if (ended_) return {};
          break;
        }
        case 11: return {};
        case 13:
          CVT_TRY(need(2));
          sbx_ = s[s.size() - 2];
          width_ = s.back();
          x_ = sbx_;
          y_ = 0;
          s.clear();
          break;
        case 14:
          if (open_) path_.close();
          open_ = false;
          ended_ = true;
          return {};
        case 21: CVT_TRY(need(2)); rmove(s[s.size() - 2], s.back()); s.clear(); break;
        case 22: CVT_TRY(need(1)); rmove(s.back(), 0); s.clear(); break;
        case 30: {
          CVT_TRY(need(4));
          const std::size_t b = s.size() - 4;
          curve(0, s[b], s[b + 1], s[b + 2], s[b + 3], 0);
          s.clear();
          break;
        }
        case 31: {
          CVT_TRY(need(4));
          const std::size_t b = s.size() - 4;
          curve(s[b], 0, s[b + 1], s[b + 2], 0, s[b + 3]);
          s.clear();
          break;
        }
        case 12: {
          const std::uint8_t e = r.u8();
          if (!r.ok()) return fail(Status::Decode, "a Type 1 charstring is truncated");
          CVT_TRY(escape(e));
          if (ended_) return {};
          break;
        }
        default:
          return fail(Status::Decode, "Type 1 charstring uses the unknown operator {}", v);
      }
    }
    return {};
  }

  Result<void> escape(std::uint8_t e) {
    auto& s = stack_;
    switch (e) {
      case 0: case 1: case 2: s.clear(); break;  // dotsection, vstem3, hstem3
      case 6:
        if (s.size() < 5) return fail(Status::Decode, "seac needs five arguments");
        seac_ = true;
        seac_asb_ = s[s.size() - 5];
        seac_adx_ = s[s.size() - 4];
        seac_ady_ = s[s.size() - 3];
        seac_base_ = static_cast<int>(s[s.size() - 2]);
        seac_accent_ = static_cast<int>(s.back());
        if (seac_base_ < 0 || seac_base_ > 255 || seac_accent_ < 0 || seac_accent_ > 255) return fail(Status::Decode, "seac codes are invalid");
        ended_ = true;
        s.clear();
        break;
      case 7:
        if (s.size() < 4) return fail(Status::Decode, "sbw needs four arguments");
        sbx_ = s[s.size() - 4];
        width_ = s[s.size() - 2];
        x_ = sbx_;
        y_ = s[s.size() - 3];
        s.clear();
        break;
      case 12:
        if (s.size() < 2) return fail(Status::Decode, "div needs two arguments");
        s[s.size() - 2] = s.back() != 0 ? s[s.size() - 2] / s.back() : 0;
        s.pop_back();
        break;
      case 16: {  // callothersubr
        if (s.size() < 2) return fail(Status::Decode, "callothersubr needs two arguments");
        const int index = static_cast<int>(s.back());
        const auto n = static_cast<std::size_t>(std::max(0.0, s[s.size() - 2]));
        s.resize(s.size() - 2);
        if (n > s.size()) return fail(Status::Decode, "callothersubr has too few arguments");
        std::vector<double> args(s.end() - static_cast<std::ptrdiff_t>(n), s.end());
        s.resize(s.size() - n);
        ps_stack_.clear();
        if (index == 1) {
          flex_ = true;
          flex_points_.clear();
        } else if (index == 0 && flex_) {
          flex_ = false;
          if (flex_points_.size() >= 7) {
            const auto& p = flex_points_;
            if (!open_) {
              path_.move_to({x_flex_start_, y_flex_start_});
              open_ = true;
            }
            path_.cubic_to(p[1], p[2], p[3]);
            path_.cubic_to(p[4], p[5], p[6]);
            x_ = p[6].x;
            y_ = p[6].y;
          }
          ps_stack_ = {y_, x_};  // pops deliver x, then y
        } else if (index == 3) {
          ps_stack_ = {3};
        } else {
          ps_stack_.assign(args.begin(), args.end());
        }
        break;
      }
      case 17:  // pop
        if (s.size() >= 64) return fail(Status::Decode, "Type 1 charstring stack overflow");
        if (!ps_stack_.empty()) {
          s.push_back(ps_stack_.back());
          ps_stack_.pop_back();
        } else {
          s.push_back(0);
        }
        break;
      case 33:
        if (s.size() >= 2) {
          x_ = s[s.size() - 2];
          y_ = s.back();
        }
        s.clear();
        break;
      default:
        s.clear();
        break;
    }
    return {};
  }

  void rmove(double dx, double dy) {
    if (flex_) {
      if (flex_points_.empty()) {
        x_flex_start_ = x_;
        y_flex_start_ = y_;
      }
      x_ += dx;
      y_ += dy;
      flex_points_.push_back({x_, y_});
      return;
    }
    if (open_) path_.close();
    x_ += dx;
    y_ += dy;
    path_.move_to({x_, y_});
    open_ = true;
  }
  void line(double dx, double dy) {
    if (!open_) {
      path_.move_to({x_, y_});
      open_ = true;
    }
    x_ += dx;
    y_ += dy;
    path_.line_to({x_, y_});
  }
  void curve(double a, double b, double c, double d, double e, double f) {
    if (!open_) {
      path_.move_to({x_, y_});
      open_ = true;
    }
    const Point c1{x_ + a, y_ + b};
    const Point c2{c1.x + c, c1.y + d};
    x_ = c2.x + e;
    y_ = c2.y + f;
    path_.cubic_to(c1, c2, {x_, y_});
  }

  const Type1Data& font_;
  const std::map<std::string, std::size_t>& index_;
  std::vector<double> stack_, ps_stack_;
  Path path_;
  double x_ = 0, y_ = 0, width_ = 0, sbx_ = 0;
  bool open_ = false, ended_ = false, flex_ = false, seac_ = false;
  std::vector<Point> flex_points_;
  double x_flex_start_ = 0, y_flex_start_ = 0;
  double seac_asb_ = 0, seac_adx_ = 0, seac_ady_ = 0;
  int seac_base_ = 0, seac_accent_ = 0;
  std::size_t ops_ = 0;
};

std::vector<double> to_deltas(const std::vector<double>& values) {
  std::vector<double> out;
  double previous = 0;
  for (const double v : values) {
    out.push_back(v - previous);
    previous = v;
  }
  return out;
}

}  // namespace

Result<ir::Font> read_type1(ByteView data, const Context& ctx) {
  CVT_TRY_ASSIGN(auto parts, split_font(data));
  Type1Data font;
  parse_clear(parts.first, font);
  CVT_TRY(parse_private(as_chars(parts.second), font, ctx));
  if (font.charstrings.empty()) return fail(Status::Decode, "the Type 1 font has no CharStrings");
  std::map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < font.charstrings.size(); ++i) index.try_emplace(font.charstrings[i].first, i);
  FontSpec spec;
  const auto info = [&](const char* key) {
    const auto it = font.info.find(key);
    return it == font.info.end() ? std::string{} : util::sanitize_utf8(it->second);
  };
  spec.family = info("FamilyName");
  if (spec.family.empty()) spec.family = info("FontName");
  if (spec.family.empty()) spec.family = "Untitled";
  spec.subfamily = info("Weight");
  if (spec.subfamily.empty() || spec.subfamily == "Medium" || spec.subfamily == "Roman" || spec.subfamily == "Book") {
    spec.subfamily = spec.subfamily.empty() ? "Regular" : spec.subfamily;
  }
  spec.full_name = info("FullName");
  spec.postscript_name = info("FontName");
  const std::string version = info("version");
  if (!version.empty()) spec.version = "Version " + version;
  spec.copyright = info("Copyright");
  if (spec.copyright.empty()) spec.copyright = info("Notice");
  else spec.notice = info("Notice");
  const auto number = [&](const char* key, double fallback) {
    const auto it = font.numbers.find(key);
    return it != font.numbers.end() && !it->second.empty() ? it->second[0] : fallback;
  };
  const auto matrix = font.numbers.find("FontMatrix");
  double upm = 1000;
  if (matrix != font.numbers.end() && matrix->second.size() == 6 && std::abs(matrix->second[0]) > 1e-9) {
    upm = std::round(1.0 / std::abs(matrix->second[0]));
  }
  spec.units_per_em = static_cast<std::uint16_t>(std::clamp(upm, 16.0, 16384.0));
  spec.italic_angle = number("ItalicAngle", 0);
  spec.italic = spec.italic_angle != 0;
  spec.underline_position = static_cast<std::int16_t>(std::lround(number("UnderlinePosition", -100)));
  spec.underline_thickness = static_cast<std::int16_t>(std::lround(number("UnderlineThickness", 50)));
  spec.fixed_pitch = number("isFixedPitch", 0) != 0;
  const std::string weight = util::to_lower(spec.subfamily);
  spec.bold = weight.find("bold") != std::string::npos;
  spec.weight_class = spec.bold ? 700 : 400;
  for (const auto& [key, op] : std::initializer_list<std::pair<const char*, int>>{
           {"BlueValues", 6}, {"OtherBlues", 7}, {"FamilyBlues", 8}, {"FamilyOtherBlues", 9}, {"StemSnapH", 1212}, {"StemSnapV", 1213}}) {
    const auto it = font.numbers.find(key);
    if (it != font.numbers.end() && !it->second.empty()) spec.private_hints[op] = to_deltas(it->second);
  }
  for (const auto& [key, op] : std::initializer_list<std::pair<const char*, int>>{
           {"StdHW", 10}, {"StdVW", 11}, {"BlueScale", 1209}, {"BlueShift", 1210}, {"BlueFuzz", 1211}}) {
    const auto it = font.numbers.find(key);
    if (it != font.numbers.end() && !it->second.empty()) spec.private_hints[op] = {it->second[0]};
  }
  // .notdef first, then the glyphs in font order.
  std::vector<std::size_t> order;
  if (const auto it = index.find(".notdef"); it != index.end()) order.push_back(it->second);
  for (std::size_t i = 0; i < font.charstrings.size(); ++i) {
    if (font.charstrings[i].first != ".notdef" && index.at(font.charstrings[i].first) == i) order.push_back(i);
  }
  if (order.empty() || font.charstrings[order[0]].first != ".notdef") {
    spec.glyph_names.push_back(".notdef");
    spec.paths.emplace_back();
    spec.advances.push_back(0);
  }
  std::map<std::string, std::uint8_t> code_of;
  if (!font.standard_encoding) {
    for (const auto& [code, name] : font.encoding) code_of.try_emplace(name, code);
  }
  Bounds bbox;
  for (std::size_t k = 0; k < order.size(); ++k) {
    if ((k & 127) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(k) / static_cast<double>(order.size()), "Converting Type 1 outlines"));
    const auto& [name, code] = font.charstrings[order[k]];
    Type1Interpreter interpreter(font, index);
    CVT_TRY_ASSIGN(Type1Glyph glyph, interpreter.run(order[k], 0));
    const auto gid = static_cast<std::uint16_t>(spec.glyph_names.size());
    std::optional<std::uint32_t> unicode = unicode_from_glyph_name(name);
    if (!unicode) {
      if (const auto it = code_of.find(name); it != code_of.end() && it->second >= 0x20) unicode = it->second;
    }
    if (unicode && name != ".notdef") spec.cmap.try_emplace(*unicode, gid);
    const Bounds b = glyph.path.control_bounds();
    if (!b.empty) {
      bbox.add({b.x_min, b.y_min});
      bbox.add({b.x_max, b.y_max});
    }
    spec.glyph_names.push_back(name);
    spec.paths.push_back(std::move(glyph.path));
    spec.advances.push_back(glyph.width);
  }
  const auto fbbox = font.numbers.find("FontBBox");
  if (fbbox != font.numbers.end() && fbbox->second.size() == 4 && fbbox->second[3] > fbbox->second[1]) {
    spec.ascender = static_cast<std::int16_t>(std::lround(fbbox->second[3]));
    spec.descender = static_cast<std::int16_t>(std::lround(fbbox->second[1]));
  } else if (!bbox.empty) {
    spec.ascender = static_cast<std::int16_t>(std::lround(bbox.y_max));
    spec.descender = static_cast<std::int16_t>(std::lround(bbox.y_min));
  }
  return build_font(spec, true, ctx);
}

// --------------------------------------------------------------------------- writer

namespace {

void t1_number(Bytes& out, std::int32_t v) {
  if (v >= -1131 && v <= 1131) {
    cff_encode_dict_int(out, v);  // identical encoding for this range
  } else {
    out.push_back(std::byte{255});
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::byte>((v >> shift) & 0xFF));
  }
}

Bytes t1_charstring(const Path& input, double width) {
  const Path path = input.to_cubic();
  Bytes out;
  t1_number(out, 0);
  t1_number(out, static_cast<std::int32_t>(std::lround(width)));
  out.push_back(std::byte{13});  // hsbw
  std::int32_t x = 0, y = 0;
  std::size_t pi = 0;
  const auto delta = [&](Point p) {
    const auto nx = static_cast<std::int32_t>(std::lround(p.x)), ny = static_cast<std::int32_t>(std::lround(p.y));
    const std::pair<std::int32_t, std::int32_t> d{nx - x, ny - y};
    x = nx;
    y = ny;
    return d;
  };
  for (const Verb verb : path.verbs) {
    switch (verb) {
      case Verb::Move: {
        const auto [dx, dy] = delta(path.points[pi++]);
        t1_number(out, dx);
        t1_number(out, dy);
        out.push_back(std::byte{21});
        break;
      }
      case Verb::Line: {
        const auto [dx, dy] = delta(path.points[pi++]);
        t1_number(out, dx);
        t1_number(out, dy);
        out.push_back(std::byte{5});
        break;
      }
      case Verb::Cubic: {
        for (int k = 0; k < 3; ++k) {
          const auto [dx, dy] = delta(path.points[pi++]);
          t1_number(out, dx);
          t1_number(out, dy);
        }
        out.push_back(std::byte{8});
        break;
      }
      case Verb::Quad: pi += 2; break;
      case Verb::Close: out.push_back(std::byte{9}); break;
    }
  }
  out.push_back(std::byte{14});
  return out;
}

std::string ps_string(std::string_view text) {
  std::string out = "(";
  for (const char c : text) {
    if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) {
      out += std::format("\\{:03o}", static_cast<unsigned char>(c));
      continue;
    }
    out.push_back(c);
  }
  return out + ")";
}

}  // namespace

Result<Bytes> write_type1(const ir::Font& font, bool binary, const Context& ctx) {
  CVT_TRY_ASSIGN(const GlyphSource source, GlyphSource::open(font));
  const std::vector<std::string> names = glyph_names(font);
  std::vector<NameRecord> records;
  if (const ir::FontTable* name = font.find("name")) {
    if (auto parsed = parse_name(name->bytes)) records = std::move(*parsed);
  }
  const auto latin1 = [](std::string_view utf8) {
    std::string out;
    for (const char32_t c : util::utf8_to_utf32(utf8)) out.push_back(c < 0x100 ? static_cast<char>(c) : '?');
    return out;
  };
  std::string ps_name = best_name(records, 6);
  if (ps_name.empty()) ps_name = "Untitled";
  const double upm = source.units_per_em();
  Bounds bbox;
  std::vector<Bytes> charstrings;
  for (std::uint32_t g = 0; g < source.glyph_count(); ++g) {
    if ((g & 127) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(g) / source.glyph_count(), "Writing Type 1"));
    CVT_TRY_ASSIGN(Path path, source.outline(g));
    const Bounds b = path.control_bounds();
    if (!b.empty) {
      bbox.add({b.x_min, b.y_min});
      bbox.add({b.x_max, b.y_max});
    }
    if (font.find("glyf")) path = path.reversed();  // TrueType winding -> PostScript winding
    charstrings.push_back(t1_charstring(path, source.advance(g)));
  }
  const ir::FontTable* post = font.find("post");
  const double italic = post ? static_cast<std::int32_t>(get_u32(post->bytes, 4)) / 65536.0 : 0;
  std::string clear;
  clear += std::format("%!PS-AdobeFont-1.0: {} {}\n", ps_name, latin1(best_name(records, 5)));
  clear += "%%EndComments\n";
  clear += "12 dict begin\n/FontType 1 def\n/PaintType 0 def\n";
  clear += std::format("/FontMatrix [{} 0 0 {} 0 0] readonly def\n", util::format_double(1.0 / upm), util::format_double(1.0 / upm));
  clear += std::format("/FontName /{} def\n", ps_name);
  clear += std::format("/FontBBox {{{} {} {} {}}} readonly def\n", std::lround(bbox.x_min), std::lround(bbox.y_min),
                       std::lround(bbox.x_max), std::lround(bbox.y_max));
  clear += "/FontInfo 9 dict dup begin\n";
  const std::string version = best_name(records, 5);
  clear += std::format(" /version {} readonly def\n", ps_string(latin1(version.starts_with("Version ") ? version.substr(8) : version)));
  const std::string copyright = best_name(records, 0);
  if (!copyright.empty()) {
    clear += std::format(" /Notice {} readonly def\n", ps_string(latin1(copyright)));
    clear += std::format(" /Copyright {} readonly def\n", ps_string(latin1(copyright)));
  }
  clear += std::format(" /FullName {} readonly def\n", ps_string(latin1(best_name(records, 4))));
  clear += std::format(" /FamilyName {} readonly def\n", ps_string(latin1(best_name(records, 1))));
  clear += std::format(" /Weight {} readonly def\n", ps_string(latin1(best_name(records, 2))));
  clear += std::format(" /ItalicAngle {} def\n", util::format_double(italic));
  clear += std::format(" /isFixedPitch {} def\n", post && get_u32(post->bytes, 12) ? "true" : "false");
  clear += std::format(" /UnderlinePosition {} def\n", post ? get_i16(post->bytes, 8) : -100);
  clear += std::format(" /UnderlineThickness {} def\n", post ? get_i16(post->bytes, 10) : 50);
  clear += "end readonly def\n";
  clear += "/Encoding 256 array\n0 1 255 {1 index exch /.notdef put} for\n";
  for (const auto& [code, glyph] : source.cmap()) {
    if (code >= 0x20 && code <= 0xFF && glyph < names.size()) clear += std::format("dup {} /{} put\n", code, names[glyph]);
  }
  clear += "readonly def\ncurrentdict end\ncurrentfile eexec\n";
  // Private part.
  util::ByteWriter priv;
  priv.text("dup /Private 8 dict dup begin\n/RD{string currentfile exch readstring pop}executeonly def\n");
  priv.text("/ND{noaccess def}executeonly def\n/NP{noaccess put}executeonly def\n");
  priv.text("/MinFeature{16 16} def\n/password 5839 def\n/BlueValues [] def\n");
  // Standard flex and hint-replacement subroutines.
  const std::vector<Bytes> subrs = {
      Bytes{std::byte{142}, std::byte{139}, std::byte{12}, std::byte{16}, std::byte{12}, std::byte{17}, std::byte{12},
            std::byte{17}, std::byte{12}, std::byte{33}, std::byte{11}},
      Bytes{std::byte{139}, std::byte{140}, std::byte{12}, std::byte{16}, std::byte{11}},
      Bytes{std::byte{139}, std::byte{141}, std::byte{12}, std::byte{16}, std::byte{11}},
      Bytes{std::byte{11}}};
  priv.text(std::format("/Subrs {} array\n", subrs.size()));
  const auto encrypted_charstring = [](const Bytes& plain) {
    Bytes with_iv(4, std::byte{0});
    with_iv.insert(with_iv.end(), plain.begin(), plain.end());
    return encrypt(with_iv, charstring_key);
  };
  for (std::size_t i = 0; i < subrs.size(); ++i) {
    const Bytes e = encrypted_charstring(subrs[i]);
    priv.text(std::format("dup {} {} RD ", i, e.size())).bytes(e).text(" NP\n");
  }
  priv.text("ND\n2 index /CharStrings ");
  priv.text(std::format("{} dict dup begin\n", charstrings.size()));
  for (std::size_t g = 0; g < charstrings.size(); ++g) {
    const Bytes e = encrypted_charstring(charstrings[g]);
    priv.text(std::format("/{} {} RD ", g < names.size() ? names[g] : std::format("glyph{}", g), e.size())).bytes(e).text(" ND\n");
  }
  priv.text("end\nend\nreadonly put\nnoaccess put\ndup /FontName get exch definefont pop\nmark currentfile closefile\n");
  Bytes plain(4, std::byte{0});
  append(plain, priv.view());
  const Bytes cipher = encrypt(plain, eexec_key);
  std::string trailer;
  for (int i = 0; i < 8; ++i) trailer += std::string(64, '0') + "\n";
  trailer += "cleartomark\n";
  if (binary) {
    util::ByteWriter out;
    out.u8(0x80).u8(1).u32le(static_cast<std::uint32_t>(clear.size())).text(clear);
    out.u8(0x80).u8(2).u32le(static_cast<std::uint32_t>(cipher.size())).bytes(cipher);
    out.u8(0x80).u8(1).u32le(static_cast<std::uint32_t>(trailer.size())).text(trailer);
    out.u8(0x80).u8(3);
    return out.take();
  }
  std::string text = clear;
  static constexpr char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < cipher.size(); ++i) {
    const auto b = std::to_integer<unsigned>(cipher[i]);
    text.push_back(digits[b >> 4]);
    text.push_back(digits[b & 15]);
    if (i % 32 == 31) text.push_back('\n');
  }
  text += "\n" + trailer;
  return to_bytes(text);
}

// --------------------------------------------------------------------------- AFM

Result<Json> read_afm(ByteView data) {
  const std::string_view text = as_chars(data);
  if (!text.starts_with("StartFontMetrics")) return fail(Status::Decode, "not an Adobe font metrics file");
  Json root = Json::object();
  Json chars = Json::array();
  Json kerning = Json::array();
  for (const std::string_view raw : util::split_lines(text)) {
    const std::string_view line = util::trim(raw);
    if (line.empty() || line.starts_with("Comment")) continue;
    if (line.starts_with("C ") || line.starts_with("CH ")) {
      Json glyph = Json::object();
      for (const std::string_view part : util::split(line, ';', false)) {
        const std::vector<std::string_view> f = util::split(util::trim(part), ' ', false);
        if (f.empty()) continue;
        if (f[0] == "C" && f.size() > 1) glyph.set("code", util::parse_int(f[1]).value_or(-1));
        else if (f[0] == "WX" && f.size() > 1) glyph.set("width", util::parse_double(f[1]).value_or(0));
        else if (f[0] == "N" && f.size() > 1) glyph.set("name", std::string(f[1]));
        else if (f[0] == "B" && f.size() > 4) {
          Json box = Json::array();
          for (std::size_t k = 1; k < 5; ++k) box.push_back(util::parse_double(f[k]).value_or(0));
          glyph.set("bbox", std::move(box));
        }
      }
      chars.push_back(std::move(glyph));
      if (chars.size() > 1'000'000) return fail(Status::Decode, "the AFM file has too many glyphs");
      continue;
    }
    const std::vector<std::string_view> f = util::split(line, ' ', false);
    if (f.empty()) continue;
    if ((f[0] == "KPX" || f[0] == "KP") && f.size() >= 4) {
      kerning.push_back(Json::object({{"left", std::string(f[1])}, {"right", std::string(f[2])},
                                      {"value", util::parse_double(f[3]).value_or(0)}}));
      continue;
    }
    static constexpr std::string_view skip[] = {"StartFontMetrics", "StartCharMetrics", "EndCharMetrics", "StartKernData",
                                                 "StartKernPairs", "EndKernPairs", "EndKernData", "EndFontMetrics"};
    if (std::ranges::find(skip, f[0]) != std::end(skip)) continue;
    const std::string_view value = util::trim(line.substr(f[0].size()));
    if (const auto number = util::parse_double(value); number && value.find(' ') == std::string_view::npos) {
      root.set(f[0], *number);
    } else {
      root.set(f[0], util::sanitize_utf8(value));
    }
  }
  root.set("characters", std::move(chars));
  root.set("kerning", std::move(kerning));
  return root;
}

Result<Bytes> write_afm(const ir::Font& font, const Context& ctx) {
  CVT_TRY_ASSIGN(const GlyphSource source, GlyphSource::open(font));
  const std::vector<std::string> names = glyph_names(font);
  std::vector<NameRecord> records;
  if (const ir::FontTable* name = font.find("name")) {
    if (auto parsed = parse_name(name->bytes)) records = std::move(*parsed);
  }
  const double scale = 1000.0 / source.units_per_em();
  std::string out = "StartFontMetrics 2.0\n";
  out += "FontName " + best_name(records, 6) + "\n";
  out += "FullName " + best_name(records, 4) + "\n";
  out += "FamilyName " + best_name(records, 1) + "\n";
  out += "Weight " + best_name(records, 2) + "\n";
  const std::string copyright = best_name(records, 0);
  if (!copyright.empty()) out += "Notice (" + copyright + ")\n";
  std::map<std::uint16_t, std::uint32_t> code_of;
  for (const auto& [code, glyph] : source.cmap()) code_of.try_emplace(glyph, code);
  std::string metrics;
  Bounds all;
  for (std::uint32_t g = 0; g < source.glyph_count(); ++g) {
    if ((g & 127) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(g) / source.glyph_count(), "Writing metrics"));
    CVT_TRY_ASSIGN(const Path path, source.outline(g));
    const Bounds b = path.exact_bounds();
    const auto it = code_of.find(static_cast<std::uint16_t>(g));
    const long code = it != code_of.end() && it->second < 256 ? static_cast<long>(it->second) : -1;
    const auto r = [&](double v) { return std::lround(v * scale); };
    metrics += std::format("C {} ; WX {} ; N {} ; B {} {} {} {} ;\n", code, r(source.advance(g)), names[g],
                           b.empty ? 0 : r(b.x_min), b.empty ? 0 : r(b.y_min), b.empty ? 0 : r(b.x_max), b.empty ? 0 : r(b.y_max));
    if (!b.empty) {
      all.add({b.x_min * scale, b.y_min * scale});
      all.add({b.x_max * scale, b.y_max * scale});
    }
  }
  out += std::format("FontBBox {} {} {} {}\n", std::lround(all.x_min), std::lround(all.y_min), std::lround(all.x_max), std::lround(all.y_max));
  out += "EncodingScheme FontSpecific\n";
  out += std::format("StartCharMetrics {}\n", source.glyph_count());
  out += metrics;
  out += "EndCharMetrics\n";
  if (const ir::FontTable* kern = font.find("kern"); kern && get_u16(kern->bytes, 0) == 0 && get_u16(kern->bytes, 2) >= 1) {
    const std::uint16_t pairs = get_u16(kern->bytes, 10);
    std::string lines;
    std::size_t count = 0;
    for (std::uint32_t i = 0; i < pairs; ++i) {
      const std::size_t at = 18 + 6 * i;
      if (at + 6 > kern->bytes.size()) break;
      const std::uint16_t l = get_u16(kern->bytes, at), rr = get_u16(kern->bytes, at + 2);
      if (l >= names.size() || rr >= names.size()) continue;
      lines += std::format("KPX {} {} {}\n", names[l], names[rr], std::lround(get_i16(kern->bytes, at + 4) * scale));
      ++count;
    }
    out += std::format("StartKernData\nStartKernPairs {}\n{}EndKernPairs\nEndKernData\n", count, lines);
  }
  out += "EndFontMetrics\n";
  return to_bytes(out);
}

}  // namespace cvt::font
