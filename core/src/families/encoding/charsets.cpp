// Convertible core - encoding family: legacy character sets and detection.
// See charsets.hpp; the tables are in charset_tables.inc (charsets_gen.py).
//
// Targets registered here:
//   charset-detect   a report of the plausible encodings of any file
// The charsets themselves are offered through the `txt` reader and writer
// (options source_encoding / encoding, see txt.cpp).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/encoding/charsets.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/encoding/encoding.hpp"
#include "families/encoding/simple.hpp"
#include "util/deflate.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt::encoding {

namespace {

struct SbcsTable {
  std::string_view id;
  std::array<std::uint16_t, 256> map;
};

#include "families/encoding/charset_tables.inc"

enum class Kind { Sbcs, ShiftJis, Cp932, EucJp, Iso2022Jp, Gbk, Gb18030, Big5, EucKr };

struct Charset {
  CharsetInfo info;
  Kind kind;
};

constexpr std::array<Charset, 42> charset_list = {{
    {{"iso-8859-1", "Western European (ISO-8859-1)", "latin1 l1 iso88591 iso_8859-1 cp819"}, Kind::Sbcs},
    {{"iso-8859-2", "Central European (ISO-8859-2)", "latin2 l2 iso88592"}, Kind::Sbcs},
    {{"iso-8859-3", "South European (ISO-8859-3)", "latin3 l3 iso88593"}, Kind::Sbcs},
    {{"iso-8859-4", "Baltic (ISO-8859-4)", "latin4 l4 iso88594"}, Kind::Sbcs},
    {{"iso-8859-5", "Cyrillic (ISO-8859-5)", "cyrillic iso88595"}, Kind::Sbcs},
    {{"iso-8859-6", "Arabic (ISO-8859-6)", "arabic iso88596"}, Kind::Sbcs},
    {{"iso-8859-7", "Greek (ISO-8859-7)", "greek iso88597"}, Kind::Sbcs},
    {{"iso-8859-8", "Hebrew (ISO-8859-8)", "hebrew iso88598"}, Kind::Sbcs},
    {{"iso-8859-9", "Turkish (ISO-8859-9)", "latin5 l5 iso88599"}, Kind::Sbcs},
    {{"iso-8859-10", "Nordic (ISO-8859-10)", "latin6 l6 iso885910"}, Kind::Sbcs},
    {{"iso-8859-11", "Thai (ISO-8859-11)", "thai iso885911 tis620"}, Kind::Sbcs},
    {{"iso-8859-13", "Baltic (ISO-8859-13)", "latin7 l7 iso885913"}, Kind::Sbcs},
    {{"iso-8859-14", "Celtic (ISO-8859-14)", "latin8 l8 iso885914"}, Kind::Sbcs},
    {{"iso-8859-15", "Western European with euro (ISO-8859-15)", "latin9 l9 iso885915 latin0"}, Kind::Sbcs},
    {{"iso-8859-16", "South-Eastern European (ISO-8859-16)", "latin10 l10 iso885916"}, Kind::Sbcs},
    {{"windows-1250", "Central European (Windows-1250)", "cp1250 win1250 xcp1250"}, Kind::Sbcs},
    {{"windows-1251", "Cyrillic (Windows-1251)", "cp1251 win1251 xcp1251"}, Kind::Sbcs},
    {{"windows-1252", "Western European (Windows-1252)", "cp1252 win1252 xcp1252 ansi"}, Kind::Sbcs},
    {{"windows-1253", "Greek (Windows-1253)", "cp1253 win1253"}, Kind::Sbcs},
    {{"windows-1254", "Turkish (Windows-1254)", "cp1254 win1254"}, Kind::Sbcs},
    {{"windows-1255", "Hebrew (Windows-1255)", "cp1255 win1255"}, Kind::Sbcs},
    {{"windows-1256", "Arabic (Windows-1256)", "cp1256 win1256"}, Kind::Sbcs},
    {{"windows-1257", "Baltic (Windows-1257)", "cp1257 win1257"}, Kind::Sbcs},
    {{"windows-1258", "Vietnamese (Windows-1258)", "cp1258 win1258"}, Kind::Sbcs},
    {{"koi8-r", "Russian (KOI8-R)", "koi8r koi8 cskoi8r"}, Kind::Sbcs},
    {{"koi8-u", "Ukrainian (KOI8-U)", "koi8u"}, Kind::Sbcs},
    {{"macintosh", "Mac OS Roman", "macroman mac-roman xmacroman mac csmacintosh"}, Kind::Sbcs},
    {{"ibm437", "DOS United States (IBM 437)", "cp437 437 dos ibmpc"}, Kind::Sbcs},
    {{"ibm850", "DOS Latin 1 (IBM 850)", "cp850 850"}, Kind::Sbcs},
    {{"ibm852", "DOS Latin 2 (IBM 852)", "cp852 852"}, Kind::Sbcs},
    {{"ibm866", "DOS Cyrillic (IBM 866)", "cp866 866"}, Kind::Sbcs},
    {{"ibm037", "EBCDIC US/Canada (IBM 037)", "cp037 037 ebcdic ebcdiccpus"}, Kind::Sbcs},
    {{"ibm500", "EBCDIC International (IBM 500)", "cp500 500 ebcdicinternational"}, Kind::Sbcs},
    {{"ibm1047", "EBCDIC Latin 1/Open Systems (IBM 1047)", "cp1047 1047"}, Kind::Sbcs},
    {{"shift_jis", "Japanese (Shift_JIS)", "sjis shiftjis ms_kanji csshiftjis"}, Kind::ShiftJis},
    {{"windows-31j", "Japanese (Windows-31J, cp932)", "cp932 ms932 windows31j mskanji"}, Kind::Cp932},
    {{"euc-jp", "Japanese (EUC-JP)", "eucjp ujis xeucjp"}, Kind::EucJp},
    {{"iso-2022-jp", "Japanese (ISO-2022-JP)", "iso2022jp jis csiso2022jp"}, Kind::Iso2022Jp},
    {{"gbk", "Chinese Simplified (GBK)", "cp936 windows936 xgbk gb2312 euccn"}, Kind::Gbk},
    {{"gb18030", "Chinese Simplified (GB18030)", "gb180302005 gb18030-2022"}, Kind::Gb18030},
    {{"big5", "Chinese Traditional (Big5)", "big5tw csbig5 xxbig5"}, Kind::Big5},
    {{"euc-kr", "Korean (EUC-KR)", "euckr ksc5601 ksc56011987 cseuckr korean"}, Kind::EucKr},
}};

constexpr std::size_t public_count = charset_list.size();

std::span<const CharsetInfo> make_info_span() noexcept;

std::string label_key(std::string_view label) {
  std::string out;
  for (const char c : label) {
    if (util::is_ascii_alpha(c) || util::is_ascii_digit(c)) out.push_back(util::ascii_lower(c));
  }
  return out;
}

const Charset* charset_of(const CharsetInfo& info) {
  for (std::size_t i = 0; i < public_count; ++i) {
    if (charset_list[i].info.id == info.id) return &charset_list[i];
  }
  return nullptr;
}

const std::array<std::uint16_t, 256>* sbcs_map(std::string_view id) {
  for (const SbcsTable& t : sbcs_tables) {
    if (t.id == id) return &t.map;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------- the CJK tables

std::vector<std::uint16_t> inflate_table(std::span<const std::string_view> chunks, std::size_t count) {
  std::string text;
  for (const std::string_view chunk : chunks) text.append(chunk);
  auto compressed = util::hex_decode(text);
  if (!compressed) return std::vector<std::uint16_t>(count, 0xFFFF);
  auto raw = util::zlib_decompress(*compressed, count * 2);
  std::vector<std::uint16_t> out(count, 0xFFFF);
  if (!raw || raw->size() != count * 2) return out;
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = static_cast<std::uint16_t>(std::to_integer<unsigned>((*raw)[2 * i]) | (std::to_integer<unsigned>((*raw)[2 * i + 1]) << 8));
  }
  return out;
}

struct Tables {
  std::vector<std::uint16_t> jis0208, jis0212, cp932, gb18030, gbk, big5, ksc5601;
};

const Tables& tables() {
  static const Tables instance = [] {
    Tables t;
    t.jis0208 = inflate_table(jis0208_z, jis0208_count);
    t.jis0212 = inflate_table(jis0212_z, jis0212_count);
    t.cp932 = inflate_table(cp932_z, cp932_count);
    t.gb18030 = inflate_table(gb18030_z, gb18030_count);
    t.gbk = t.gb18030;
    const std::vector<std::uint16_t> patch = inflate_table(gbk_patch_z, gbk_patch_count);
    for (std::size_t i = 0; i + 1 < patch.size(); i += 2) {
      if (patch[i] < t.gbk.size()) t.gbk[patch[i]] = patch[i + 1];
    }
    t.big5 = inflate_table(big5_z, big5_count);
    t.ksc5601 = inflate_table(ksc5601_z, ksc5601_count);
    return t;
  }();
  return instance;
}

/// Code point -> byte code (big-endian packed) for encoding; built once per table.
using Reverse = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

Reverse build_reverse(const std::vector<std::uint16_t>& table, auto code_of, bool last_wins = false) {
  Reverse out;
  for (std::size_t i = 0; i < table.size(); ++i) {
    if (table[i] != 0xFFFF) out.emplace_back(table[i], code_of(i));
  }
  // Several codes may map to one character; like Python's encoders, the first
  // (lowest) code wins, except in Big5 where the later duplicate is preferred.
  if (last_wins) std::ranges::reverse(out);
  std::ranges::stable_sort(out, [](const auto& a, const auto& b) { return a.first < b.first; });
  out.erase(std::unique(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), out.end());
  return out;
}

std::optional<std::uint32_t> lookup(const Reverse& reverse, char32_t cp) {
  const auto it = std::ranges::lower_bound(reverse, static_cast<std::uint32_t>(cp), {}, &std::pair<std::uint32_t, std::uint32_t>::first);
  if (it == reverse.end() || it->first != cp) return std::nullopt;
  return it->second;
}

struct Reverses {
  Reverse jis0208, jis0212, cp932, gb18030, gbk, big5, ksc5601;
};

const Reverses& reverses() {
  static const Reverses instance = [] {
    const Tables& t = tables();
    Reverses r;
    const auto square = [](std::size_t i) { return static_cast<std::uint32_t>(((i / 94) << 8) | (i % 94)); };  // row, col
    r.jis0208 = build_reverse(t.jis0208, square);
    r.jis0212 = build_reverse(t.jis0212, square);
    r.ksc5601 = build_reverse(t.ksc5601, square);
    r.cp932 = build_reverse(t.cp932, [](std::size_t i) {
      const std::size_t lead_index = i / 189;
      const auto lead = static_cast<std::uint32_t>(lead_index < 31 ? 0x81 + lead_index : 0xE0 + (lead_index - 31));
      return (lead << 8) | static_cast<std::uint32_t>(0x40 + i % 189);
    });
    const auto wide = [](std::size_t i) { return static_cast<std::uint32_t>(((0x81 + i / 191) << 8) | (0x40 + i % 191)); };
    r.gb18030 = build_reverse(t.gb18030, wide);
    r.gbk = build_reverse(t.gbk, wide);
    r.big5 = build_reverse(t.big5, wide, true);
    return r;
  }();
  return instance;
}

// ---------------------------------------------------------------------------- decoding

class Decoder {
 public:
  explicit Decoder(ByteView data) : data_(data) { out_.reserve(data.size() * 3 / 2); }
  [[nodiscard]] bool more() const { return i_ < data_.size(); }
  [[nodiscard]] std::uint8_t peek(std::size_t ahead = 0) const {
    return i_ + ahead < data_.size() ? std::to_integer<std::uint8_t>(data_[i_ + ahead]) : 0;
  }
  [[nodiscard]] std::size_t left() const { return data_.size() - i_; }
  void take(std::size_t n) { i_ += n; }
  void put(char32_t cp) { util::append_utf8(out_, cp); }
  void bad(std::size_t n) {
    put(U'�');
    ++replaced_;
    i_ += n;
  }
  CharsetText finish() { return {std::move(out_), replaced_}; }

 private:
  ByteView data_;
  std::size_t i_ = 0;
  std::string out_;
  std::size_t replaced_ = 0;
};

// One 94x94 table entry (rows/columns from 0xA1 or 0x21 offsets already removed).
bool put_square(Decoder& d, const std::vector<std::uint16_t>& table, unsigned row, unsigned col, std::size_t width) {
  if (row >= 94 || col >= 94) return false;
  const std::uint16_t cp = table[row * 94 + col];
  if (cp == 0xFFFF) return false;
  d.put(cp);
  d.take(width);
  return true;
}

void decode_shift_jis(Decoder& d, bool cp932) {
  const Tables& t = tables();
  while (d.more()) {
    const unsigned b = d.peek();
    if (b < 0x80) {
      d.put(b);
      d.take(1);
    } else if (b >= 0xA1 && b <= 0xDF) {
      d.put(0xFF61 + (b - 0xA1));
      d.take(1);
    } else if (cp932 && (b == 0x80 || b == 0xA0 || b >= 0xFD)) {
      d.put(b == 0x80 ? 0x80 : b == 0xA0 ? 0xF8F0 : 0xF8F1 + (b - 0xFD));  // as Python's cp932 decoder
      d.take(1);
    } else if (((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= (cp932 ? 0xFCu : 0xEFu))) && d.left() >= 2) {
      const unsigned s2 = d.peek(1);
      if (s2 < 0x40 || s2 > 0xFC || s2 == 0x7F) {
        d.bad(1);
        continue;
      }
      if (cp932) {
        const unsigned lead_index = b < 0xA0 ? b - 0x81 : 31 + (b - 0xE0);
        const std::uint16_t cp = t.cp932[lead_index * 189 + (s2 - 0x40)];
        if (cp == 0xFFFF) {
          d.bad(2);
        } else {
          d.put(cp);
          d.take(2);
        }
        continue;
      }
      // Shift_JIS byte pair -> JIS X 0208 row and column.
      unsigned row = (b < 0xA0 ? b - 0x81 : b - 0xC1) * 2;
      unsigned col = 0;
      if (s2 >= 0x9F) {
        ++row;
        col = s2 - 0x9F;
      } else {
        col = s2 - (s2 >= 0x80 ? 0x41 : 0x40);
      }
      if (!put_square(d, t.jis0208, row, col, 2)) d.bad(2);
    } else {
      d.bad(1);
    }
  }
}

void decode_euc(Decoder& d, Kind kind) {
  const Tables& t = tables();
  while (d.more()) {
    const unsigned b = d.peek();
    if (b < 0x80) {
      d.put(b);
      d.take(1);
      continue;
    }
    if (kind == Kind::EucJp && b == 0x8E && d.left() >= 2 && d.peek(1) >= 0xA1 && d.peek(1) <= 0xDF) {
      d.put(0xFF61 + (d.peek(1) - 0xA1));
      d.take(2);
      continue;
    }
    if (kind == Kind::EucJp && b == 0x8F && d.left() >= 3 && d.peek(1) >= 0xA1 && d.peek(2) >= 0xA1) {
      if (!put_square(d, t.jis0212, d.peek(1) - 0xA1u, d.peek(2) - 0xA1u, 3)) d.bad(3);
      continue;
    }
    if (b >= 0xA1 && b <= 0xFE && d.left() >= 2 && d.peek(1) >= 0xA1 && d.peek(1) <= 0xFE) {
      if (!put_square(d, kind == Kind::EucKr ? t.ksc5601 : t.jis0208, b - 0xA1u, d.peek(1) - 0xA1u, 2)) d.bad(2);
      continue;
    }
    d.bad(1);
  }
}

void decode_iso2022jp(Decoder& d) {
  const Tables& t = tables();
  enum class Mode { Ascii, Roman, Jis0208, Katakana } mode = Mode::Ascii;
  while (d.more()) {
    const unsigned b = d.peek();
    if (b == 0x1B && d.left() >= 3) {
      const unsigned a = d.peek(1), c = d.peek(2);
      if (a == '(' && c == 'B') {
        mode = Mode::Ascii;
      } else if (a == '(' && c == 'J') {
        mode = Mode::Roman;
      } else if (a == '(' && c == 'I') {
        mode = Mode::Katakana;
      } else if (a == '$' && (c == 'B' || c == '@')) {
        mode = Mode::Jis0208;
      } else {
        d.bad(1);
        continue;
      }
      d.take(3);
      continue;
    }
    if (b == '\n' || b == '\r') mode = Mode::Ascii;  // lines start in ASCII (RFC 1468)
    if (b >= 0x80) {
      d.bad(1);
      continue;
    }
    switch (mode) {
      case Mode::Ascii: d.put(b); d.take(1); break;
      case Mode::Roman: d.put(b == 0x5C ? 0xA5 : b == 0x7E ? 0x203E : b); d.take(1); break;
      case Mode::Katakana:
        if (b >= 0x21 && b <= 0x5F) {
          d.put(0xFF61 + (b - 0x21));
          d.take(1);
        } else {
          d.put(b);
          d.take(1);
        }
        break;
      case Mode::Jis0208:
        if (b < 0x21 || d.left() < 2) {
          d.put(b);
          d.take(1);
        } else if (!put_square(d, t.jis0208, b - 0x21u, d.peek(1) - 0x21u, 2)) {
          d.bad(2);
        }
        break;
    }
  }
}

void decode_wide(Decoder& d, Kind kind) {
  const Tables& t = tables();
  const std::vector<std::uint16_t>& table = kind == Kind::Big5 ? t.big5 : kind == Kind::Gbk ? t.gbk : t.gb18030;
  while (d.more()) {
    const unsigned b = d.peek();
    if (b < 0x80) {
      d.put(b);
      d.take(1);
      continue;
    }
    if (kind == Kind::Gbk && b == 0x80) {
      d.put(0x20AC);  // the euro sign of code page 936
      d.take(1);
      continue;
    }
    if (b >= 0x81 && b <= 0xFE && d.left() >= 2) {
      const unsigned b2 = d.peek(1);
      if (kind == Kind::Gb18030 && b2 >= 0x30 && b2 <= 0x39) {  // four-byte sequence
        if (d.left() < 4 || d.peek(2) < 0x81 || d.peek(2) > 0xFE || d.peek(3) < 0x30 || d.peek(3) > 0x39) {
          d.bad(1);
          continue;
        }
        const std::uint32_t linear = ((((b - 0x81u) * 10 + (b2 - 0x30u)) * 126 + (d.peek(2) - 0x81u)) * 10) + (d.peek(3) - 0x30u);
        if (b <= 0x84) {
          const auto range = std::ranges::upper_bound(gb18030_ranges, linear, {}, &Gb18030Range::index);
          if (range != gb18030_ranges.begin() && linear < std::prev(range)->index + std::prev(range)->length) {
            d.put(std::prev(range)->code_point + (linear - std::prev(range)->index));
            d.take(4);
            continue;
          }
        } else if (b >= 0x90 && linear >= 189000 && linear - 189000 <= 0xFFFFF) {
          d.put(0x10000 + (linear - 189000));
          d.take(4);
          continue;
        }
        d.bad(4);
        continue;
      }
      if (b2 >= 0x40 && b2 <= 0xFE) {
        const std::uint16_t cp = table[(b - 0x81) * 191 + (b2 - 0x40)];
        if (cp != 0xFFFF) {
          d.put(cp);
          d.take(2);
          continue;
        }
      }
    }
    d.bad(1);
  }
}

// ---------------------------------------------------------------------------- encoding

class Encoder {
 public:
  void byte(unsigned b) { out_.push_back(static_cast<std::byte>(b)); }
  void pair(std::uint32_t code) {
    byte(code >> 8);
    byte(code & 0xFF);
  }
  void missing() {
    byte('?');
    ++replaced_;
  }
  CharsetBytes finish() { return {std::move(out_), replaced_}; }

 private:
  Bytes out_;
  std::size_t replaced_ = 0;
};

// Encoder-only mappings of Python's codecs: characters that are written with
// a code whose decoding is a different (canonical) character.
struct OneWay {
  char32_t cp;
  std::uint16_t code;  // one byte when <= 0xFF
};
constexpr std::array<OneWay, 2> jis_roman = {{{0x00A5, 0x5C}, {0x203E, 0x7E}}};
constexpr std::array<OneWay, 11> cp932_extra = {{{0x0080, 0x80}, {0x00A2, 0x8191}, {0x00A3, 0x8192}, {0x00AC, 0x81CA},
                                                  {0x2016, 0x8161}, {0x2212, 0x817C}, {0x301C, 0x8160}, {0xF8F0, 0xA0},
                                                  {0xF8F1, 0xFD}, {0xF8F2, 0xFE}, {0xF8F3, 0xFF}}};

template <std::size_t N>
bool emit_one_way(Encoder& e, char32_t cp, const std::array<OneWay, N>& table) {
  for (const OneWay& m : table) {
    if (m.cp != cp) continue;
    if (m.code > 0xFF) {
      e.pair(m.code);
    } else {
      e.byte(m.code);
    }
    return true;
  }
  return false;
}

void encode_char(Encoder& e, char32_t cp, Kind kind, int& iso_mode) {
  const Reverses& r = reverses();
  if (kind == Kind::Iso2022Jp) {
    if (cp < 0x80) {
      if (iso_mode != 0) {
        for (const char c : std::string_view("\x1b(B")) e.byte(static_cast<unsigned char>(c));
        iso_mode = 0;
      }
      e.byte(cp);
      return;
    }
    if (cp == 0x00A5 || cp == 0x203E) {  // JIS X 0201 Roman
      if (iso_mode != 2) {
        for (const char c : std::string_view("\x1b(J")) e.byte(static_cast<unsigned char>(c));
        iso_mode = 2;
      }
      e.byte(cp == 0x00A5 ? 0x5C : 0x7E);
      return;
    }
    const auto code = lookup(r.jis0208, cp);
    if (!code) {
      e.missing();
      return;
    }
    if (iso_mode != 1) {
      for (const char c : std::string_view("\x1b$B")) e.byte(static_cast<unsigned char>(c));
      iso_mode = 1;
    }
    e.byte((*code >> 8) + 0x21);
    e.byte((*code & 0xFF) + 0x21);
    return;
  }
  if (cp < 0x80) {
    e.byte(cp);
    return;
  }
  switch (kind) {
    case Kind::Sbcs: break;  // handled by the caller
    case Kind::ShiftJis:
    case Kind::Cp932: {
      if (cp >= 0xFF61 && cp <= 0xFF9F) {
        e.byte(0xA1 + (cp - 0xFF61));
        return;
      }
      if (kind == Kind::ShiftJis && emit_one_way(e, cp, jis_roman)) return;
      if (kind == Kind::Cp932) {
        if (const auto code = lookup(r.cp932, cp)) {
          e.pair(*code);
          return;
        }
        if (emit_one_way(e, cp, cp932_extra)) return;
        break;
      }
      if (const auto code = lookup(r.jis0208, cp)) {
        const unsigned row = *code >> 8, col = *code & 0xFF;
        const unsigned s1 = row / 2 + (row < 62 ? 0x81 : 0xC1);
        const unsigned s2 = (row % 2 == 0) ? col + (col < 63 ? 0x40 : 0x41) : col + 0x9F;
        e.byte(s1);
        e.byte(s2);
        return;
      }
      break;
    }
    case Kind::EucJp:
      if (emit_one_way(e, cp, jis_roman)) return;
      if (cp >= 0xFF61 && cp <= 0xFF9F) {
        e.byte(0x8E);
        e.byte(0xA1 + (cp - 0xFF61));
        return;
      }
      if (const auto code = lookup(r.jis0208, cp)) {
        e.pair(*code + 0xA1A1);
        return;
      }
      if (const auto code = lookup(r.jis0212, cp)) {
        e.byte(0x8F);
        e.pair(*code + 0xA1A1);
        return;
      }
      break;
    case Kind::EucKr:
      if (const auto code = lookup(r.ksc5601, cp)) {
        e.pair(*code + 0xA1A1);
        return;
      }
      if (cp == 0x3164) {  // HANGUL FILLER: the lead code of KS X 1001's composed syllables
        e.pair(0xA4D4);
        return;
      }
      break;
    case Kind::Gbk:
      if (cp == 0x20AC) {
        e.byte(0x80);
        return;
      }
      if (const auto code = lookup(r.gbk, cp)) {
        e.pair(*code);
        return;
      }
      break;
    case Kind::Big5:
      if (const auto code = lookup(r.big5, cp)) {
        e.pair(*code);
        return;
      }
      break;
    case Kind::Gb18030: {
      if (const auto code = lookup(r.gb18030, cp)) {
        e.pair(*code);
        return;
      }
      std::uint32_t linear = 0;
      if (cp >= 0x10000) {
        linear = 189000 + static_cast<std::uint32_t>(cp - 0x10000);
      } else {
        const auto found = std::ranges::find_if(gb18030_ranges, [cp](const Gb18030Range& range) {
          return cp >= range.code_point && cp < range.code_point + range.length;
        });
        if (found == gb18030_ranges.end()) break;
        linear = found->index + static_cast<std::uint32_t>(cp - found->code_point);
      }
      const std::uint32_t b4 = linear % 10, b3 = linear / 10 % 126, b2 = linear / 1260 % 10, b1 = linear / 12600;
      e.byte(0x81 + b1);
      e.byte(0x30 + b2);
      e.byte(0x81 + b3);
      e.byte(0x30 + b4);
      return;
    }
    case Kind::Iso2022Jp: break;
  }
  e.missing();
}

// ---------------------------------------------------------------------------- detection

struct Scripts {
  std::size_t total = 0, ascii = 0, ascii_letters = 0, latin = 0, cyrillic = 0, greek = 0, hebrew = 0, arabic = 0,
              thai = 0, kana = 0, halfwidth_kana = 0, han = 0, hangul = 0, fullwidth = 0, symbols = 0, controls = 0,
              rare = 0, upper_cyr = 0, spaced = 0,  // spaced: spaces between two non-ASCII characters
              mixed = 0;  // an ASCII letter next to a letter of a non-Latin script, inside one word
};

Scripts classify(std::string_view utf8) {
  Scripts s;
  char32_t previous = 0;
  bool space_after_wide = false;
  for (std::size_t i = 0; i < utf8.size();) {
    const util::Utf8Decoded d = util::decode_utf8(utf8, i);
    const char32_t c = d.code_point;
    i += d.length;
    ++s.total;
    if (c >= 0x80 && space_after_wide) ++s.spaced;
    space_after_wide = c == ' ' && previous >= 0x80;
    const auto non_latin_letter = [](char32_t x) {
      return (x >= 0x370 && x <= 0x6FF) || (x >= 0xE00 && x <= 0xE7F);  // Greek, Cyrillic, Hebrew, Arabic, Thai
    };
    const auto ascii_letter = [](char32_t x) { return x < 0x80 && util::is_ascii_alpha(static_cast<char>(x)); };
    if ((ascii_letter(previous) && non_latin_letter(c)) || (non_latin_letter(previous) && ascii_letter(c))) ++s.mixed;
    previous = c;
    if (c < 0x80) {
      if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') ++s.controls;
      if (util::is_ascii_alpha(static_cast<char>(c))) ++s.ascii_letters;
      ++s.ascii;
    } else if (c < 0xA0) {
      ++s.controls;
    } else if ((c >= 0xC0 && c <= 0x24F) || (c >= 0x1E00 && c <= 0x1EFF)) {
      ++s.latin;
    } else if (c >= 0x400 && c <= 0x4FF) {
      ++s.cyrillic;
      if (c >= 0x410 && c <= 0x42F) ++s.upper_cyr;
    } else if (c >= 0x370 && c <= 0x3FF) {
      ++s.greek;
    } else if (c >= 0x590 && c <= 0x5FF) {
      ++s.hebrew;
    } else if (c >= 0x600 && c <= 0x6FF) {
      ++s.arabic;
    } else if (c >= 0xE00 && c <= 0xE7F) {
      ++s.thai;
    } else if (c >= 0xFF61 && c <= 0xFF9F) {
      ++s.halfwidth_kana;  // real text rarely uses it; random bytes read as Shift_JIS often do
    } else if (c >= 0x3040 && c <= 0x30FF) {
      ++s.kana;
    } else if (c >= 0x4E00 && c <= 0x9FFF) {
      ++s.han;
    } else if (c >= 0xAC00 && c <= 0xD7A3) {
      ++s.hangul;
    } else if ((c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFF60)) {
      ++s.fullwidth;
    } else if (c == 0xFFFD || (c >= 0xE000 && c <= 0xF8FF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF)) {
      ++s.rare;
    } else {
      ++s.symbols;
    }
  }
  return s;
}

enum class Family { Japanese, Chinese, Korean, Cyrillic, Greek, Hebrew, Arabic, Thai, Latin };

Family family_of(std::string_view id) {
  if (id == "shift_jis" || id == "windows-31j" || id == "euc-jp" || id == "iso-2022-jp") return Family::Japanese;
  if (id == "gbk" || id == "gb18030" || id == "big5") return Family::Chinese;
  if (id == "euc-kr") return Family::Korean;
  if (id == "windows-1251" || id == "koi8-r" || id == "koi8-u" || id == "iso-8859-5" || id == "ibm866") return Family::Cyrillic;
  if (id == "windows-1253" || id == "iso-8859-7") return Family::Greek;
  if (id == "windows-1255" || id == "iso-8859-8") return Family::Hebrew;
  if (id == "windows-1256" || id == "iso-8859-6") return Family::Arabic;
  if (id == "iso-8859-11") return Family::Thai;
  return Family::Latin;
}

/// 0..100: how much the decoded text looks like real text of the charset's language.
int plausibility(const Scripts& s, std::string_view id, std::size_t replaced) {
  const std::size_t non_ascii = s.total - s.ascii;
  if (non_ascii == 0) return 50;
  if (replaced > 0) return 0;
  const auto share = [non_ascii](std::size_t n) { return static_cast<int>(100 * n / non_ascii); };
  const int noise = share(s.controls + s.rare + s.symbols + s.halfwidth_kana);
  // Korean separates words with spaces; Chinese and Japanese do not.
  const std::size_t wide = s.han + s.hangul + s.kana + s.fullwidth;
  const bool spaced = wide > 0 && s.spaced * 10 > wide;
  int good = 0;
  const Family family = family_of(id);
  switch (family) {
    case Family::Japanese:
      good = s.kana > 0 ? share(s.kana + s.han + s.fullwidth) : share(s.han + s.fullwidth) * 3 / 4;
      if (spaced) good /= 2;
      break;
    case Family::Chinese:
      good = s.kana + s.hangul > 0 ? 0 : share(s.han + s.fullwidth);
      if (spaced) good /= 2;
      break;
    case Family::Korean:
      good = share(s.hangul + s.han / 4 + s.fullwidth);
      if (!spaced) good = good * 3 / 4;
      break;
    case Family::Cyrillic: {
      // Running text is mostly lower case: KOI8 read as Windows-1251 (and back) swaps the cases.
      const int lower = s.cyrillic > 0 ? static_cast<int>(100 * (s.cyrillic - s.upper_cyr) / s.cyrillic) : 0;
      good = share(s.cyrillic) * lower / 100;
      break;
    }
    case Family::Greek: good = share(s.greek); break;
    case Family::Hebrew: good = share(s.hebrew); break;
    case Family::Arabic: good = share(s.arabic); break;
    case Family::Thai: good = share(s.thai); break;
    case Family::Latin: {
      // Latin text is mostly ASCII letters with some accented ones; text in
      // another script read as Latin-1 turns almost every letter into one.
      good = share(s.latin);
      const std::size_t letters = s.ascii_letters + s.latin;
      if (letters > 0 && s.latin * 2 > letters) good = good * static_cast<int>(s.ascii_letters * 2) / static_cast<int>(letters);
      break;
    }
  }
  if (family == Family::Cyrillic || family == Family::Greek || family == Family::Hebrew || family == Family::Arabic ||
      family == Family::Thai) {
    // "cafй": Latin text read in a Cyrillic (Greek, ...) code page mixes scripts inside words.
    good = std::max(0, good - share(s.mixed) * 2);
  }
  return std::clamp(good - noise / 2, 0, 100);
}

// Candidates tried when the text is not Unicode, roughly by how common they are.
constexpr std::array<std::string_view, 16> detection_candidates = {
    "windows-1252", "windows-1250", "windows-1251", "koi8-r", "windows-1253", "windows-1255", "windows-1256",
    "iso-8859-11", "shift_jis", "windows-31j", "euc-jp", "iso-2022-jp", "gbk", "gb18030", "big5", "euc-kr"};

int sniff_report(ByteView data, std::string_view) {
  const std::string_view text = as_chars(data).substr(0, 64);
  return text.starts_with("Most likely encoding: ") ? 85 : 0;
}

}  // namespace

std::span<const CharsetInfo> legacy_charsets() noexcept { return make_info_span(); }

namespace {
std::span<const CharsetInfo> make_info_span() noexcept {
  static const std::array<CharsetInfo, public_count> infos = [] {
    std::array<CharsetInfo, public_count> out{};
    for (std::size_t i = 0; i < public_count; ++i) out[i] = charset_list[i].info;
    return out;
  }();
  return infos;
}
}  // namespace

const CharsetInfo* find_charset(std::string_view label) noexcept {
  const std::string key = label_key(label);
  if (key.empty()) return nullptr;
  for (const CharsetInfo& info : legacy_charsets()) {
    if (label_key(info.id) == key) return &info;
    for (const std::string_view alias : util::split(info.aliases, ' ', false)) {
      if (label_key(alias) == key) return &info;
    }
  }
  return nullptr;
}

CharsetText charset_decode(ByteView data, const CharsetInfo& info) {
  const Charset* charset = charset_of(info);
  Decoder d(data);
  if (charset == nullptr) {
    while (d.more()) d.bad(1);
    return d.finish();
  }
  switch (charset->kind) {
    case Kind::Sbcs: {
      const auto* map = sbcs_map(info.id);
      while (d.more()) {
        const std::uint16_t cp = (*map)[d.peek()];
        if (cp == 0xFFFF) {
          d.bad(1);
        } else {
          d.put(cp);
          d.take(1);
        }
      }
      break;
    }
    case Kind::ShiftJis: decode_shift_jis(d, false); break;
    case Kind::Cp932: decode_shift_jis(d, true); break;
    case Kind::EucJp:
    case Kind::EucKr: decode_euc(d, charset->kind); break;
    case Kind::Iso2022Jp: decode_iso2022jp(d); break;
    case Kind::Gbk:
    case Kind::Gb18030:
    case Kind::Big5: decode_wide(d, charset->kind); break;
  }
  return d.finish();
}

CharsetBytes charset_encode(std::string_view utf8, const CharsetInfo& info) {
  const Charset* charset = charset_of(info);
  Encoder e;
  if (charset == nullptr) return e.finish();
  std::vector<std::pair<std::uint16_t, std::uint8_t>> sbcs_reverse;
  if (charset->kind == Kind::Sbcs) {
    const auto* map = sbcs_map(info.id);
    for (unsigned b = 0; b < 256; ++b) {
      if ((*map)[b] != 0xFFFF) sbcs_reverse.emplace_back((*map)[b], static_cast<std::uint8_t>(b));
    }
    std::ranges::stable_sort(sbcs_reverse, [](const auto& a, const auto& b) { return a.first < b.first; });
  }
  int iso_mode = 0;
  for (std::size_t i = 0; i < utf8.size();) {
    const util::Utf8Decoded d = util::decode_utf8(utf8, i);
    i += d.length;
    const char32_t cp = d.length == 1 && static_cast<unsigned char>(utf8[i - 1]) >= 0x80 ? U'�' : d.code_point;
    if (charset->kind == Kind::Sbcs) {
      const auto it = std::ranges::lower_bound(sbcs_reverse, static_cast<std::uint16_t>(std::min<char32_t>(cp, 0xFFFF)), {},
                                               &std::pair<std::uint16_t, std::uint8_t>::first);
      if (cp <= 0xFFFF && it != sbcs_reverse.end() && it->first == cp) {
        e.byte(it->second);
      } else {
        e.missing();
      }
      continue;
    }
    encode_char(e, cp, charset->kind, iso_mode);
  }
  if (iso_mode != 0) {
    for (const char c : std::string_view("\x1b(B")) e.byte(static_cast<unsigned char>(c));
  }
  return e.finish();
}

std::vector<CharsetGuess> detect_charsets(ByteView data) {
  std::vector<CharsetGuess> out;
  const ByteView sample = data.first(std::min<std::size_t>(data.size(), 64 * 1024));
  if (const auto bom = util::detect_bom(sample)) {
    out.push_back({std::string(util::encoding_name(bom->encoding)), 100});
    return out;
  }
  const std::string_view text = as_chars(sample);
  const bool ascii = std::ranges::all_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
  if (ascii && text.find("\x1b$B") == std::string_view::npos && text.find("\x1b$@") == std::string_view::npos) {
    if (text.find('\0') == std::string_view::npos) {
      out.push_back({"utf-8", 100});  // pure ASCII is valid in every ASCII-compatible charset
      return out;
    }
  }
  // Valid UTF-8 with multi-byte sequences is almost never anything else. A
  // sample cut inside a sequence is still accepted.
  std::string_view utf8_part = text;
  if (sample.size() < data.size()) {
    std::size_t cut = utf8_part.size();
    while (cut > 0 && cut + 4 > utf8_part.size() && (static_cast<unsigned char>(utf8_part[cut - 1]) & 0xC0) == 0x80) --cut;
    if (cut > 0 && static_cast<unsigned char>(utf8_part[cut - 1]) >= 0xC0) --cut;
    utf8_part = utf8_part.substr(0, cut);
  }
  if (util::is_valid_utf8(utf8_part)) {
    out.push_back({"utf-8", 98});
    return out;
  }
  const util::EncodingGuess unicode = util::guess_encoding(sample);
  if (unicode.confident && unicode.encoding != util::TextEncoding::Utf8) {
    out.push_back({std::string(util::encoding_name(unicode.encoding)), 90});
    return out;
  }
  for (const std::string_view id : detection_candidates) {
    const CharsetInfo* info = find_charset(id);
    if (info == nullptr) continue;
    const CharsetText decoded = charset_decode(sample, *info);
    // A truncated multi-byte character at the very end of the sample is not an error.
    std::size_t replaced = decoded.replaced;
    if (replaced == 1 && sample.size() < data.size()) replaced = 0;
    const Scripts scripts = classify(decoded.text);
    int score = plausibility(scripts, id, replaced);
    if (score == 0) continue;
    // Prefer the multi-byte candidates when they decode cleanly: single-byte
    // code pages accept any bytes, so their mere success says little.
    const bool multibyte = id.starts_with("shift") || id.starts_with("euc") || id.starts_with("gb") || id == "big5" ||
                           id == "windows-31j" || id == "iso-2022-jp";
    score = multibyte ? 40 + score * 6 / 10 : 20 + score * 6 / 10;
    out.push_back({std::string(id), std::clamp(score, 1, 99)});
  }
  std::ranges::stable_sort(out, [](const CharsetGuess& a, const CharsetGuess& b) { return a.confidence > b.confidence; });
  if (out.size() > 8) out.resize(8);
  if (out.empty()) out.push_back({"windows-1252", 10});
  return out;
}

void register_charsets(Registry& registry) {
  add_binary_encoding(registry, {
      .id = "charset-detect", .name = "Text encoding report",
      .description = "The most likely character encodings of a text file, with a confidence for each.",
      .extensions = {"txt", "charset"}, .popularity = 20,
      .encode = [](const ir::Blob& blob, const WriteContext& ctx) -> Result<std::string> {
        CVT_TRY(ctx.checkpoint(0.0, "Detecting"));
        const std::vector<CharsetGuess> guesses = detect_charsets(blob.bytes);
        std::string out = std::format("Most likely encoding: {} ({}%)\n", guesses.front().id, guesses.front().confidence);
        for (std::size_t i = 1; i < guesses.size(); ++i) out += std::format("Also possible: {} ({}%)\n", guesses[i].id, guesses[i].confidence);
        return out;
      },
      .expansion = 0,
      .sniff = sniff_report,
  });
}

}  // namespace cvt::encoding
