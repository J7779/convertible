// Convertible core - pdf family: CMap parsing.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/cmap.hpp"

#include <algorithm>

#include "families/pdf/syntax.hpp"

namespace cvt::pdf {

std::u32string utf16be_to_u32(std::string_view bytes) {
  std::u32string out;
  for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
    char32_t u = (static_cast<std::uint8_t>(bytes[i]) << 8) | static_cast<std::uint8_t>(bytes[i + 1]);
    if (u >= 0xD800 && u <= 0xDBFF && i + 3 < bytes.size()) {
      const char32_t lo = (static_cast<std::uint8_t>(bytes[i + 2]) << 8) | static_cast<std::uint8_t>(bytes[i + 3]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
    }
    out.push_back(u);
  }
  if (bytes.size() == 1) out.push_back(static_cast<std::uint8_t>(bytes[0]));
  return out;
}

namespace {

std::uint32_t code_value(std::string_view bytes) {
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < bytes.size() && i < 4; ++i) v = (v << 8) | static_cast<std::uint8_t>(bytes[i]);
  return v;
}

int code_length(std::string_view bytes) { return static_cast<int>(std::clamp<std::size_t>(bytes.size(), 1, 4)); }

}  // namespace

void CMap::merge(const CMap& other) {
  if (spaces_.empty()) spaces_ = other.spaces_;
  for (const auto& [k, v] : other.cid_single_) cid_single_.emplace(k, v);
  cid_ranges_.insert(cid_ranges_.end(), other.cid_ranges_.begin(), other.cid_ranges_.end());
  for (const auto& [k, v] : other.uni_single_) uni_single_.emplace(k, v);
  uni_ranges_.insert(uni_ranges_.end(), other.uni_ranges_.begin(), other.uni_ranges_.end());
  identity_ = identity_ || other.identity_;
  code_is_unicode_ = code_is_unicode_ || other.code_is_unicode_;
}

CMap CMap::parse(ByteView program, int depth) {
  CMap map;
  Lexer lexer(program);
  std::vector<Token> operands;
  std::size_t entries = 0;
  constexpr std::size_t max_entries = 2'000'000;
  while (true) {
    Token t = lexer.next();
    if (t.kind == Token::Kind::End) break;
    if (t.kind != Token::Kind::Keyword) {
      if (t.kind == Token::Kind::ArrayOpen) {
        // Arrays only occur in bfrange; handled inline there.
        continue;
      }
      operands.push_back(std::move(t));
      if (operands.size() > 64) operands.erase(operands.begin());
      continue;
    }
    const std::string& op = t.text;
    if (op == "usecmap" && !operands.empty() && operands.back().kind == Token::Kind::Name && depth < 4) {
      map.merge(predefined(operands.back().text));
    } else if (op == "begincodespacerange") {
      while (true) {
        Token lo = lexer.next();
        if (lo.kind != Token::Kind::HexString) break;
        Token hi = lexer.next();
        if (hi.kind != Token::Kind::HexString) break;
        Space s;
        s.length = code_length(lo.text);
        s.low = code_value(lo.text);
        s.high = code_value(hi.text);
        if (map.spaces_.size() < 256) map.spaces_.push_back(s);
      }
    } else if (op == "begincidchar") {
      while (entries < max_entries) {
        Token code = lexer.next();
        if (code.kind != Token::Kind::HexString) break;
        Token cid = lexer.next();
        if (cid.kind != Token::Kind::Integer) break;
        map.cid_single_[key(code_length(code.text), code_value(code.text))] =
            static_cast<std::uint32_t>(std::clamp<std::int64_t>(cid.integer, 0, 0xFFFFFF));
        ++entries;
      }
    } else if (op == "begincidrange") {
      while (entries < max_entries) {
        Token lo = lexer.next();
        if (lo.kind != Token::Kind::HexString) break;
        Token hi = lexer.next();
        Token cid = lexer.next();
        if (hi.kind != Token::Kind::HexString || cid.kind != Token::Kind::Integer) break;
        CidRange r;
        r.length = code_length(lo.text);
        r.low = code_value(lo.text);
        r.high = code_value(hi.text);
        r.cid = static_cast<std::uint32_t>(std::clamp<std::int64_t>(cid.integer, 0, 0xFFFFFF));
        if (r.high >= r.low) map.cid_ranges_.push_back(r);
        ++entries;
      }
    } else if (op == "beginbfchar") {
      while (entries < max_entries) {
        Token code = lexer.next();
        if (code.kind != Token::Kind::HexString) break;
        Token dst = lexer.next();
        std::u32string value;
        if (dst.kind == Token::Kind::HexString || dst.kind == Token::Kind::String) {
          value = utf16be_to_u32(dst.text);
        } else if (dst.kind == Token::Kind::Name) {
          // Glyph names as destinations are rare; keep ASCII single letters.
          if (dst.text.size() == 1) value.push_back(static_cast<char32_t>(dst.text[0]));
        } else {
          break;
        }
        map.uni_single_[key(code_length(code.text), code_value(code.text))] = std::move(value);
        ++entries;
      }
    } else if (op == "beginbfrange") {
      while (entries < max_entries) {
        Token lo = lexer.next();
        if (lo.kind != Token::Kind::HexString) break;
        Token hi = lexer.next();
        if (hi.kind != Token::Kind::HexString) break;
        UniRange r;
        r.length = code_length(lo.text);
        r.low = code_value(lo.text);
        r.high = code_value(hi.text);
        Token dst = lexer.next();
        if (dst.kind == Token::Kind::HexString || dst.kind == Token::Kind::String) {
          r.base = utf16be_to_u32(dst.text);
        } else if (dst.kind == Token::Kind::ArrayOpen) {
          while (true) {
            Token item = lexer.next();
            if (item.kind != Token::Kind::HexString && item.kind != Token::Kind::String) break;
            if (r.array.size() < 65536) r.array.push_back(utf16be_to_u32(item.text));
          }
        } else {
          break;
        }
        if (r.high >= r.low && r.high - r.low < (1u << 24)) map.uni_ranges_.push_back(std::move(r));
        ++entries;
      }
    } else if (op == "WMode") {
      // handled via /WMode value: operands are "/WMode 1 def"
    } else if (op == "def" && operands.size() >= 2 && operands[operands.size() - 2].kind == Token::Kind::Name &&
               operands[operands.size() - 2].text == "WMode" && operands.back().kind == Token::Kind::Integer) {
      map.vertical_ = operands.back().integer == 1;
    } else if (op == "def" && operands.size() >= 2 && operands[operands.size() - 2].kind == Token::Kind::Name &&
               operands[operands.size() - 2].text == "CMapName" && operands.back().kind == Token::Kind::Name) {
      map.name_ = operands.back().text;
    }
    operands.clear();
  }
  return map;
}

CMap CMap::predefined(std::string_view name) {
  CMap map;
  map.name_ = std::string(name);
  map.vertical_ = name.ends_with("-V");
  Space two{2, 0, 0xFFFF};
  if (name == "Identity-H" || name == "Identity-V") {
    map.identity_ = true;
    map.spaces_.push_back(two);
    return map;
  }
  if (name.find("UCS2") != std::string_view::npos || name.find("UTF16") != std::string_view::npos) {
    // Codes are UTF-16BE. Mark them as Unicode; CIDs equal the codes (the
    // collection-specific CID tables are not bundled).
    map.code_is_unicode_ = true;
    map.identity_ = true;
    if (name.find("UTF16") != std::string_view::npos) {
      map.spaces_.push_back(two);
      map.spaces_.push_back(Space{4, 0xD800DC00u, 0xDBFFDFFFu});
    } else {
      map.spaces_.push_back(two);
    }
    return map;
  }
  if (name.find("UTF8") != std::string_view::npos) {
    map.code_is_unicode_ = true;
    map.identity_ = true;
    map.spaces_.push_back(Space{1, 0, 0x7F});
    map.spaces_.push_back(Space{2, 0xC080, 0xDFBF});
    map.spaces_.push_back(Space{3, 0xE08080, 0xEFBFBF});
    map.spaces_.push_back(Space{4, 0xF0808080u, 0xF7BFBFBFu});
    return map;
  }
  // Legacy CJK encodings (RKSJ, EUC, GBK, Big5 ...) need large tables that
  // are not bundled: treat codes as 1 or 2 bytes with identity CIDs.
  map.known_ = false;
  map.identity_ = true;
  map.spaces_.push_back(Space{1, 0x00, 0x80});
  map.spaces_.push_back(Space{2, 0x8140, 0xFFFC});
  return map;
}

CMap::Code CMap::next_code(std::string_view bytes, std::size_t pos) const noexcept {
  Code code;
  if (pos >= bytes.size()) return code;
  if (spaces_.empty()) {
    code.value = static_cast<std::uint8_t>(bytes[pos]);
    code.length = 1;
    return code;
  }
  std::uint32_t value = 0;
  for (int length = 1; length <= 4 && pos + static_cast<std::size_t>(length) <= bytes.size(); ++length) {
    value = (value << 8) | static_cast<std::uint8_t>(bytes[pos + static_cast<std::size_t>(length) - 1]);
    for (const Space& s : spaces_) {
      if (s.length != length) continue;
      // Byte-wise range check.
      bool inside = true;
      for (int b = 0; b < length && inside; ++b) {
        const int shift = 8 * (length - 1 - b);
        const std::uint32_t v = (value >> shift) & 0xFF;
        inside = v >= ((s.low >> shift) & 0xFF) && v <= ((s.high >> shift) & 0xFF);
      }
      if (inside) {
        code.value = value;
        code.length = length;
        return code;
      }
    }
  }
  // No range matched: take the shortest codespace length.
  int shortest = 4;
  for (const Space& s : spaces_) shortest = std::min(shortest, s.length);
  shortest = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(shortest), bytes.size() - pos));
  code.length = std::max(shortest, 1);
  code.value = 0;
  for (int b = 0; b < code.length; ++b) code.value = (code.value << 8) | static_cast<std::uint8_t>(bytes[pos + static_cast<std::size_t>(b)]);
  return code;
}

std::uint32_t CMap::cid(Code code) const {
  if (const auto it = cid_single_.find(key(code.length, code.value)); it != cid_single_.end()) return it->second;
  for (auto it = cid_ranges_.rbegin(); it != cid_ranges_.rend(); ++it) {
    if (it->length == code.length && code.value >= it->low && code.value <= it->high) {
      return it->cid + (code.value - it->low);
    }
  }
  return identity_ ? code.value : 0;
}

std::optional<std::u32string> CMap::unicode(Code code) const {
  if (const auto it = uni_single_.find(key(code.length, code.value)); it != uni_single_.end()) return it->second;
  for (auto it = uni_ranges_.rbegin(); it != uni_ranges_.rend(); ++it) {
    if (code.value < it->low || code.value > it->high) continue;
    if (it->length != code.length && !(code.length == 1 && it->length == 2)) continue;
    const std::uint32_t offset = code.value - it->low;
    if (!it->array.empty()) {
      if (offset < it->array.size()) return it->array[offset];
      return std::nullopt;
    }
    std::u32string value = it->base;
    if (value.empty()) return std::nullopt;
    value.back() += offset;
    return value;
  }
  // Some producers write 1-byte ToUnicode keys for 2-byte codes or vice versa.
  if (code.length == 2) {
    if (const auto it = uni_single_.find(key(1, code.value)); it != uni_single_.end() && code.value < 256) {
      return it->second;
    }
  }
  return std::nullopt;
}

}  // namespace cvt::pdf
