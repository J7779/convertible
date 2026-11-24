// Convertible core - pdf family: CFF fonts and the Type 2 charstring interpreter.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "families/pdf/font_data.hpp"
#include "families/pdf/font_program.hpp"

namespace cvt::pdf::font {

namespace {

constexpr const char* kStandardStrings[] = {
    ".notdef", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quoteright",
    "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two",
    "three", "four", "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater",
    "question", "at", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
    "T", "U", "V", "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore",
    "quoteleft", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t",
    "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde", "exclamdown", "cent", "sterling",
    "fraction", "yen", "florin", "section", "currency", "quotesingle", "quotedblleft", "guillemotleft",
    "guilsinglleft", "guilsinglright", "fi", "fl", "endash", "dagger", "daggerdbl", "periodcentered", "paragraph",
    "bullet", "quotesinglbase", "quotedblbase", "quotedblright", "guillemotright", "ellipsis", "perthousand",
    "questiondown", "grave", "acute", "circumflex", "tilde", "macron", "breve", "dotaccent", "dieresis", "ring",
    "cedilla", "hungarumlaut", "ogonek", "caron", "emdash", "AE", "ordfeminine", "Lslash", "Oslash", "OE",
    "ordmasculine", "ae", "dotlessi", "lslash", "oslash", "oe", "germandbls", "onesuperior", "logicalnot", "mu",
    "trademark", "Eth", "onehalf", "plusminus", "Thorn", "onequarter", "divide", "brokenbar", "degree", "thorn",
    "threequarters", "twosuperior", "registered", "minus", "eth", "multiply", "threesuperior", "copyright",
    "Aacute", "Acircumflex", "Adieresis", "Agrave", "Aring", "Atilde", "Ccedilla", "Eacute", "Ecircumflex",
    "Edieresis", "Egrave", "Iacute", "Icircumflex", "Idieresis", "Igrave", "Ntilde", "Oacute", "Ocircumflex",
    "Odieresis", "Ograve", "Otilde", "Scaron", "Uacute", "Ucircumflex", "Udieresis", "Ugrave", "Yacute",
    "Ydieresis", "Zcaron", "aacute", "acircumflex", "adieresis", "agrave", "aring", "atilde", "ccedilla", "eacute",
    "ecircumflex", "edieresis", "egrave", "iacute", "icircumflex", "idieresis", "igrave", "ntilde", "oacute",
    "ocircumflex", "odieresis", "ograve", "otilde", "scaron", "uacute", "ucircumflex", "udieresis", "ugrave",
    "yacute", "ydieresis", "zcaron", "exclamsmall", "Hungarumlautsmall", "dollaroldstyle", "dollarsuperior",
    "ampersandsmall", "Acutesmall", "parenleftsuperior", "parenrightsuperior", "twodotenleader", "onedotenleader",
    "zerooldstyle", "oneoldstyle", "twooldstyle", "threeoldstyle", "fouroldstyle", "fiveoldstyle", "sixoldstyle",
    "sevenoldstyle", "eightoldstyle", "nineoldstyle", "commasuperior", "threequartersemdash", "periodsuperior",
    "questionsmall", "asuperior", "bsuperior", "centsuperior", "dsuperior", "esuperior", "isuperior", "lsuperior",
    "msuperior", "nsuperior", "osuperior", "rsuperior", "ssuperior", "tsuperior", "ff", "ffi", "ffl",
    "parenleftinferior", "parenrightinferior", "Circumflexsmall", "hyphensuperior", "Gravesmall", "Asmall",
    "Bsmall", "Csmall", "Dsmall", "Esmall", "Fsmall", "Gsmall", "Hsmall", "Ismall", "Jsmall", "Ksmall", "Lsmall",
    "Msmall", "Nsmall", "Osmall", "Psmall", "Qsmall", "Rsmall", "Ssmall", "Tsmall", "Usmall", "Vsmall", "Wsmall",
    "Xsmall", "Ysmall", "Zsmall", "colonmonetary", "onefitted", "rupiah", "Tildesmall", "exclamdownsmall",
    "centoldstyle", "Lslashsmall", "Scaronsmall", "Zcaronsmall", "Dieresissmall", "Brevesmall", "Caronsmall",
    "Dotaccentsmall", "Macronsmall", "figuredash", "hypheninferior", "Ogoneksmall", "Ringsmall", "Cedillasmall",
    "questiondownsmall", "oneeighth", "threeeighths", "fiveeighths", "seveneighths", "onethird", "twothirds",
    "zerosuperior", "foursuperior", "fivesuperior", "sixsuperior", "sevensuperior", "eightsuperior",
    "ninesuperior", "zeroinferior", "oneinferior", "twoinferior", "threeinferior", "fourinferior", "fiveinferior",
    "sixinferior", "seveninferior", "eightinferior", "nineinferior", "centinferior", "dollarinferior",
    "periodinferior", "commainferior", "Agravesmall", "Aacutesmall", "Acircumflexsmall", "Atildesmall",
    "Adieresissmall", "Aringsmall", "AEsmall", "Ccedillasmall", "Egravesmall", "Eacutesmall", "Ecircumflexsmall",
    "Edieresissmall", "Igravesmall", "Iacutesmall", "Icircumflexsmall", "Idieresissmall", "Ethsmall",
    "Ntildesmall", "Ogravesmall", "Oacutesmall", "Ocircumflexsmall", "Otildesmall", "Odieresissmall", "OEsmall",
    "Oslashsmall", "Ugravesmall", "Uacutesmall", "Ucircumflexsmall", "Udieresissmall", "Yacutesmall",
    "Thornsmall", "Ydieresissmall", "001.000", "001.001", "001.002", "001.003", "Black", "Bold", "Book", "Light",
    "Medium", "Regular", "Roman", "Semibold"};
static_assert(std::size(kStandardStrings) == 391, "CFF standard strings");

struct Index {
  std::vector<std::pair<std::size_t, std::size_t>> items;  // offset, length (absolute)
  std::size_t end = 0;
  bool ok = false;
};

std::uint8_t at(ByteView d, std::size_t o) { return o < d.size() ? std::to_integer<std::uint8_t>(d[o]) : 0; }
std::uint32_t read_be(ByteView d, std::size_t o, int n) {
  std::uint32_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8) | at(d, o + static_cast<std::size_t>(i));
  return v;
}

Index read_index(ByteView d, std::size_t o) {
  Index index;
  if (o + 2 > d.size()) return index;
  const std::uint32_t count = read_be(d, o, 2);
  if (count == 0) {
    index.end = o + 2;
    index.ok = true;
    return index;
  }
  const int off_size = at(d, o + 2);
  if (off_size < 1 || off_size > 4) return index;
  const std::size_t offsets = o + 3;
  const std::size_t data_base = offsets + static_cast<std::size_t>(count + 1) * static_cast<std::size_t>(off_size) - 1;
  if (data_base > d.size()) return index;
  index.items.reserve(count);
  std::uint32_t prev = read_be(d, offsets, off_size);
  for (std::uint32_t i = 1; i <= count; ++i) {
    const std::uint32_t cur = read_be(d, offsets + static_cast<std::size_t>(i) * static_cast<std::size_t>(off_size), off_size);
    std::size_t start = data_base + prev, end = data_base + cur;
    if (cur < prev || end > d.size()) {
      start = std::min(start, d.size());
      end = std::max(start, std::min(end, d.size()));
    }
    index.items.emplace_back(start, end - start);
    prev = cur;
  }
  index.end = data_base + prev;
  index.ok = index.end <= d.size();
  return index;
}

using DictMap = std::map<int, std::vector<double>>;

DictMap read_dict(ByteView d, std::size_t start, std::size_t length) {
  DictMap dict;
  std::vector<double> operands;
  std::size_t p = start;
  const std::size_t end = std::min(d.size(), start + length);
  while (p < end) {
    const std::uint8_t b0 = at(d, p);
    if (b0 <= 21) {
      int op = b0;
      ++p;
      if (b0 == 12) op = 1200 + at(d, p++);
      dict[op] = operands;
      operands.clear();
    } else if (b0 == 28) {
      operands.push_back(static_cast<std::int16_t>(read_be(d, p + 1, 2)));
      p += 3;
    } else if (b0 == 29) {
      operands.push_back(static_cast<std::int32_t>(read_be(d, p + 1, 4)));
      p += 5;
    } else if (b0 == 30) {
      std::string s;
      ++p;
      bool done = false;
      while (p < end && !done) {
        const std::uint8_t b = at(d, p++);
        for (int k = 0; k < 2 && !done; ++k) {
          const int n = k == 0 ? b >> 4 : b & 15;
          if (n <= 9) s.push_back(static_cast<char>('0' + n));
          else if (n == 0xA) s.push_back('.');
          else if (n == 0xB) s.push_back('E');
          else if (n == 0xC) s += "E-";
          else if (n == 0xE) s.push_back('-');
          else if (n == 0xF) done = true;
        }
      }
      // Locale-free parse.
      double value = 0, scale = 0;
      bool neg = false, exp_neg = false;
      int exponent = 0;
      bool in_exp = false;
      for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '-' && !in_exp) neg = true;
        else if (c == 'E') {
          in_exp = true;
          if (i + 1 < s.size() && s[i + 1] == '-') {
            exp_neg = true;
            ++i;
          }
        } else if (c == '.') scale = 1;
        else if (c >= '0' && c <= '9') {
          if (in_exp) exponent = std::min(exponent * 10 + (c - '0'), 300);
          else if (scale == 0) value = value * 10 + (c - '0');
          else {
            scale *= 0.1;
            value += (c - '0') * scale;
          }
        }
      }
      value *= std::pow(10.0, exp_neg ? -exponent : exponent);
      operands.push_back(neg ? -value : value);
    } else if (b0 >= 32 && b0 <= 246) {
      operands.push_back(b0 - 139);
      ++p;
    } else if (b0 >= 247 && b0 <= 250) {
      operands.push_back((b0 - 247) * 256 + at(d, p + 1) + 108);
      p += 2;
    } else if (b0 >= 251 && b0 <= 254) {
      operands.push_back(-(b0 - 251) * 256 - at(d, p + 1) - 108);
      p += 2;
    } else {
      ++p;
    }
    if (operands.size() > 48) operands.erase(operands.begin());
  }
  return dict;
}

double dict_num(const DictMap& d, int op, double fallback, std::size_t index = 0) {
  const auto it = d.find(op);
  if (it == d.end() || it->second.size() <= index) return fallback;
  return it->second[index];
}

int subr_bias(std::size_t count) { return count < 1240 ? 107 : (count < 33900 ? 1131 : 32768); }

}  // namespace

struct Cff::Impl {
  Bytes data;
  Index charstrings;
  Index global_subrs;
  struct Private {
    Index subrs;
    double default_width = 0;
    double nominal_width = 0;
    Matrix matrix;
    bool has_matrix = false;
  };
  std::vector<Private> privates;           // one per FD (or one)
  std::vector<std::uint8_t> fd_select;      // gid -> fd
  std::vector<std::uint16_t> charset;       // gid -> sid / cid
  std::vector<std::string> strings;         // custom strings (sid - 391)
  std::array<std::uint16_t, 256> encoding{};  // code -> gid (0 = none)
  bool standard_encoding = true;
  Matrix top_matrix{0.001, 0, 0, 0.001, 0, 0};
  bool top_has_matrix = false;

  [[nodiscard]] std::string sid_name(std::uint16_t sid) const {
    if (sid < 391) return kStandardStrings[sid];
    if (sid - 391u < strings.size()) return strings[sid - 391u];
    return {};
  }
  [[nodiscard]] const Private& private_for(std::uint16_t gid) const {
    const std::size_t fd = gid < fd_select.size() ? fd_select[gid] : 0;
    return privates[fd < privates.size() ? fd : 0];
  }

  // Type 2 interpreter state.
  struct Interp {
    const Impl* font;
    const Private* priv;
    Path* path;
    std::vector<double> stack;
    double x = 0, y = 0;
    int stems = 0;
    bool width_seen = false;
    double width = 0;
    bool open = false;
    std::array<double, 32> transient{};
    int depth = 0;
    std::size_t ops = 0;
    bool ended = false;
    std::uint16_t seac_base = 0, seac_accent = 0;
    bool has_seac = false;
    double seac_adx = 0, seac_ady = 0;
  };

  void run(Interp& in, ByteView cs) const;
  Path outline(std::uint16_t gid, double* width_out, int depth) const;
};

void Cff::Impl::run(Interp& in, ByteView cs) const {
  if (++in.depth > 12) return;
  auto& s = in.stack;
  const auto take_width = [&](bool odd_has_width, std::size_t expected) {
    if (in.width_seen) return;
    in.width_seen = true;
    const bool has = odd_has_width ? (s.size() % 2 == 1) : (s.size() > expected);
    if (has && !s.empty()) {
      in.width = in.priv->nominal_width + s.front();
      s.erase(s.begin());
    } else {
      in.width = in.priv->default_width;
    }
  };
  const auto move_to = [&](double dx, double dy) {
    if (in.open) in.path->close();
    in.x += dx;
    in.y += dy;
    in.path->move_to(in.x, in.y);
    in.open = true;
  };
  const auto line_to = [&](double dx, double dy) {
    if (!in.open) move_to(0, 0);
    in.x += dx;
    in.y += dy;
    in.path->line_to(in.x, in.y);
  };
  const auto curve_to = [&](double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
    if (!in.open) move_to(0, 0);
    const double x1 = in.x + dx1, y1 = in.y + dy1;
    const double x2 = x1 + dx2, y2 = y1 + dy2;
    in.x = x2 + dx3;
    in.y = y2 + dy3;
    in.path->cubic_to(x1, y1, x2, y2, in.x, in.y);
  };
  std::size_t p = 0;
  while (p < cs.size() && !in.ended) {
    if (++in.ops > 200000) return;
    const std::uint8_t b0 = at(cs, p);
    if (b0 >= 32 || b0 == 28) {
      if (b0 == 28) {
        s.push_back(static_cast<std::int16_t>(read_be(cs, p + 1, 2)));
        p += 3;
      } else if (b0 <= 246) {
        s.push_back(b0 - 139);
        ++p;
      } else if (b0 <= 250) {
        s.push_back((b0 - 247) * 256 + at(cs, p + 1) + 108);
        p += 2;
      } else if (b0 <= 254) {
        s.push_back(-(b0 - 251) * 256 - at(cs, p + 1) - 108);
        p += 2;
      } else {
        s.push_back(static_cast<std::int32_t>(read_be(cs, p + 1, 4)) / 65536.0);
        p += 5;
      }
      if (s.size() > 96) s.erase(s.begin());
      continue;
    }
    ++p;
    switch (b0) {
      case 1: case 3: case 18: case 23:  // stems
        take_width(true, 0);
        in.stems += static_cast<int>(s.size() / 2);
        s.clear();
        break;
      case 19: case 20: {  // hintmask, cntrmask
        take_width(true, 0);
        in.stems += static_cast<int>(s.size() / 2);
        s.clear();
        p += static_cast<std::size_t>((in.stems + 7) / 8);
        break;
      }
      case 21:  // rmoveto
        take_width(false, 2);
        if (s.size() >= 2) move_to(s[s.size() - 2], s[s.size() - 1]);
        s.clear();
        break;
      case 22:  // hmoveto
        take_width(false, 1);
        if (!s.empty()) move_to(s.back(), 0);
        s.clear();
        break;
      case 4:  // vmoveto
        take_width(false, 1);
        if (!s.empty()) move_to(0, s.back());
        s.clear();
        break;
      case 5:  // rlineto
        for (std::size_t i = 0; i + 1 < s.size(); i += 2) line_to(s[i], s[i + 1]);
        s.clear();
        break;
      case 6: case 7: {  // hlineto, vlineto
        bool horizontal = b0 == 6;
        for (const double v : s) {
          if (horizontal) line_to(v, 0);
          else line_to(0, v);
          horizontal = !horizontal;
        }
        s.clear();
        break;
      }
      case 8:  // rrcurveto
        for (std::size_t i = 0; i + 5 < s.size(); i += 6) curve_to(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
        s.clear();
        break;
      case 24: {  // rcurveline
        std::size_t i = 0;
        for (; i + 8 <= s.size(); i += 6) curve_to(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
        if (i + 2 <= s.size()) line_to(s[i], s[i + 1]);
        s.clear();
        break;
      }
      case 25: {  // rlinecurve
        std::size_t i = 0;
        for (; i + 8 <= s.size(); i += 2) line_to(s[i], s[i + 1]);
        if (i + 6 <= s.size()) curve_to(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
        s.clear();
        break;
      }
      case 26: {  // vvcurveto
        std::size_t i = 0;
        double dx1 = 0;
        if (s.size() % 4 == 1) dx1 = s[i++];
        for (; i + 3 < s.size(); i += 4) {
          curve_to(dx1, s[i], s[i + 1], s[i + 2], 0, s[i + 3]);
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
          curve_to(s[i], dy1, s[i + 1], s[i + 2], s[i + 3], 0);
          dy1 = 0;
        }
        s.clear();
        break;
      }
      case 30: case 31: {  // vhcurveto, hvcurveto
        bool horizontal = b0 == 31;
        std::size_t i = 0;
        while (i + 3 < s.size()) {
          const bool last = s.size() - i == 5;
          if (horizontal) {
            curve_to(s[i], 0, s[i + 1], s[i + 2], last ? s[i + 4] : 0, s[i + 3]);
          } else {
            curve_to(0, s[i], s[i + 1], s[i + 2], s[i + 3], last ? s[i + 4] : 0);
          }
          i += last ? 5 : 4;
          horizontal = !horizontal;
        }
        s.clear();
        break;
      }
      case 10: case 29: {  // callsubr, callgsubr
        if (s.empty()) return;
        const Index& subrs = b0 == 10 ? in.priv->subrs : global_subrs;
        const auto index = static_cast<long long>(s.back()) + subr_bias(subrs.items.size());
        s.pop_back();
        if (index < 0 || static_cast<std::size_t>(index) >= subrs.items.size()) return;
        const auto [off, len] = subrs.items[static_cast<std::size_t>(index)];
        run(in, ByteView(data).subspan(off, len));
        --in.depth;
        break;
      }
      case 11:  // return
        return;
      case 14: {  // endchar
        take_width(false, s.size() >= 4 ? 4 : 0);
        if (s.size() >= 4) {
          in.has_seac = true;
          in.seac_adx = s[s.size() - 4];
          in.seac_ady = s[s.size() - 3];
          in.seac_base = static_cast<std::uint16_t>(std::clamp(s[s.size() - 2], 0.0, 255.0));
          in.seac_accent = static_cast<std::uint16_t>(std::clamp(s[s.size() - 1], 0.0, 255.0));
        }
        if (in.open) in.path->close();
        in.open = false;
        in.ended = true;
        s.clear();
        return;
      }
      case 12: {
        const std::uint8_t b1 = at(cs, p++);
        const auto pop = [&]() {
          if (s.empty()) return 0.0;
          const double v = s.back();
          s.pop_back();
          return v;
        };
        switch (b1) {
          case 3: { const double b = pop(), a = pop(); s.push_back(a != 0 && b != 0 ? 1 : 0); break; }
          case 4: { const double b = pop(), a = pop(); s.push_back(a != 0 || b != 0 ? 1 : 0); break; }
          case 5: s.push_back(pop() == 0 ? 1 : 0); break;
          case 9: s.push_back(std::abs(pop())); break;
          case 10: { const double b = pop(), a = pop(); s.push_back(a + b); break; }
          case 11: { const double b = pop(), a = pop(); s.push_back(a - b); break; }
          case 12: { const double b = pop(), a = pop(); s.push_back(b != 0 ? a / b : 0); break; }
          case 14: s.push_back(-pop()); break;
          case 15: { const double b = pop(), a = pop(); s.push_back(a == b ? 1 : 0); break; }
          case 18: pop(); break;
          case 20: { const double i = pop(), v = pop(); if (i >= 0 && i < 32) in.transient[static_cast<std::size_t>(i)] = v; break; }
          case 21: { const double i = pop(); s.push_back(i >= 0 && i < 32 ? in.transient[static_cast<std::size_t>(i)] : 0); break; }
          case 22: { const double v2 = pop(), v1 = pop(), s2 = pop(), s1 = pop(); s.push_back(v1 <= v2 ? s1 : s2); break; }
          case 23: s.push_back(0.5); break;
          case 24: { const double b = pop(), a = pop(); s.push_back(a * b); break; }
          case 26: s.push_back(std::sqrt(std::max(0.0, pop()))); break;
          case 27: { const double a = pop(); s.push_back(a); s.push_back(a); break; }
          case 28: { const double b = pop(), a = pop(); s.push_back(b); s.push_back(a); break; }
          case 29: {
            const auto i = static_cast<long long>(pop());
            const long long n = static_cast<long long>(s.size());
            s.push_back(i < 0 ? (n ? s.back() : 0) : (i < n ? s[static_cast<std::size_t>(n - 1 - i)] : 0));
            break;
          }
          case 30: {
            const auto j = static_cast<long long>(pop());
            const auto n = static_cast<long long>(pop());
            if (n > 0 && n <= static_cast<long long>(s.size())) {
              const auto first = s.end() - n;
              const long long shift = ((j % n) + n) % n;
              std::rotate(first, s.end() - shift, s.end());
            }
            break;
          }
          case 34: {  // hflex
            if (s.size() >= 7) {
              const double y0 = in.y;
              curve_to(s[0], 0, s[1], s[2], s[3], 0);
              curve_to(s[4], 0, s[5], y0 - in.y, s[6], 0);
            }
            s.clear();
            break;
          }
          case 35:  // flex
            if (s.size() >= 12) {
              curve_to(s[0], s[1], s[2], s[3], s[4], s[5]);
              curve_to(s[6], s[7], s[8], s[9], s[10], s[11]);
            }
            s.clear();
            break;
          case 36:  // hflex1
            if (s.size() >= 9) {
              const double y0 = in.y;
              curve_to(s[0], s[1], s[2], s[3], s[4], 0);
              curve_to(s[5], 0, s[6], s[7], s[8], y0 - in.y);
            }
            s.clear();
            break;
          case 37:  // flex1
            if (s.size() >= 11) {
              const double dx = s[0] + s[2] + s[4] + s[6] + s[8];
              const double dy = s[1] + s[3] + s[5] + s[7] + s[9];
              const bool horizontal = std::abs(dx) > std::abs(dy);
              const double dx6 = horizontal ? s[10] : -dx;
              const double dy6 = horizontal ? -dy : s[10];
              curve_to(s[0], s[1], s[2], s[3], s[4], s[5]);
              curve_to(s[6], s[7], s[8], s[9], dx6, dy6);
            }
            s.clear();
            break;
          default: s.clear(); break;
        }
        break;
      }
      default:
        s.clear();
        break;
    }
  }
}

Path Cff::Impl::outline(std::uint16_t gid, double* width_out, int depth) const {
  Path path;
  if (gid >= charstrings.items.size() || depth > 2) return path;
  Interp in{};
  in.font = this;
  in.priv = &private_for(gid);
  in.path = &path;
  const auto [off, len] = charstrings.items[gid];
  run(in, ByteView(data).subspan(off, len));
  if (in.open) path.close();
  if (!in.width_seen) in.width = in.priv->default_width;
  if (width_out != nullptr) *width_out = in.width;
  if (in.has_seac) {
    // Standard-encoding accented character composition.
    const auto glyph_of_code = [&](std::uint16_t code) -> std::optional<std::uint16_t> {
      const std::string_view name = encoding_glyph_name(BaseEncoding::Standard, static_cast<std::uint8_t>(code));
      for (std::size_t g = 0; g < charset.size(); ++g) {
        if (sid_name(charset[g]) == name) return static_cast<std::uint16_t>(g);
      }
      return std::nullopt;
    };
    Path composed;
    if (const auto base = glyph_of_code(in.seac_base)) composed.append(outline(*base, nullptr, depth + 1));
    if (const auto accent = glyph_of_code(in.seac_accent)) {
      composed.append(outline(*accent, nullptr, depth + 1), Matrix::translate(in.seac_adx, in.seac_ady));
    }
    composed.append(path);
    return composed;
  }
  return path;
}

Result<Cff> Cff::parse(ByteView input) {
  ByteView data = input;
  if (data.size() >= 12 && read_be(data, 0, 4) == 0x4F54544F) {  // 'OTTO'
    const std::uint32_t n = read_be(data, 4, 2);
    bool found = false;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::size_t rec = 12 + 16ull * i;
      if (rec + 16 > data.size()) break;
      if (read_be(data, rec, 4) == 0x43464620) {  // 'CFF '
        const std::uint32_t off = read_be(data, rec + 8, 4), len = read_be(data, rec + 12, 4);
        if (off <= data.size() && len <= data.size() - off) {
          data = data.subspan(off, len);
          found = true;
        }
        break;
      }
    }
    if (!found) return fail(Status::Decode, "OpenType font without a CFF table");
  }
  if (data.size() < 4 || at(data, 0) != 1) return fail(Status::Decode, "not a CFF font");
  auto impl = std::make_shared<Impl>();
  impl->data.assign(data.begin(), data.end());
  const ByteView d = impl->data;
  const std::size_t hdr = at(d, 2);
  const Index names = read_index(d, hdr);
  if (!names.ok) return fail(Status::Decode, "damaged CFF name index");
  const Index top = read_index(d, names.end);
  if (!top.ok || top.items.empty()) return fail(Status::Decode, "damaged CFF top dict");
  const Index strings = read_index(d, top.end);
  if (!strings.ok) return fail(Status::Decode, "damaged CFF string index");
  impl->global_subrs = read_index(d, strings.end);
  for (const auto& [off, len] : strings.items) {
    impl->strings.emplace_back(reinterpret_cast<const char*>(d.data()) + off, len);
  }
  Cff font;
  if (!names.items.empty()) {
    font.name_.assign(reinterpret_cast<const char*>(d.data()) + names.items[0].first, names.items[0].second);
  }
  const DictMap topdict = read_dict(d, top.items[0].first, top.items[0].second);
  if (const auto it = topdict.find(1207); it != topdict.end() && it->second.size() >= 6) {
    const auto& v = it->second;
    impl->top_matrix = Matrix{v[0], v[1], v[2], v[3], v[4], v[5]};
    impl->top_has_matrix = true;
  }
  font.cid_ = topdict.contains(1230);
  const auto cs_offset = static_cast<std::size_t>(dict_num(topdict, 17, 0));
  impl->charstrings = read_index(d, cs_offset);
  if (!impl->charstrings.ok || impl->charstrings.items.empty()) return fail(Status::Decode, "CFF font has no glyphs");
  const std::size_t n_glyphs = impl->charstrings.items.size();
  const auto read_private = [&](const DictMap& dict) {
    Impl::Private priv;
    const auto size = static_cast<std::size_t>(std::max(0.0, dict_num(dict, 18, 0, 0)));
    const auto offset = static_cast<std::size_t>(std::max(0.0, dict_num(dict, 18, 0, 1)));
    if (offset < d.size() && size > 0) {
      const DictMap pd = read_dict(d, offset, size);
      priv.default_width = dict_num(pd, 20, 0);
      priv.nominal_width = dict_num(pd, 21, 0);
      const double subrs = dict_num(pd, 19, 0);
      if (subrs > 0) priv.subrs = read_index(d, offset + static_cast<std::size_t>(subrs));
    }
    if (const auto it = dict.find(1207); it != dict.end() && it->second.size() >= 6) {
      const auto& v = it->second;
      priv.matrix = Matrix{v[0], v[1], v[2], v[3], v[4], v[5]};
      priv.has_matrix = true;
    }
    return priv;
  };
  if (font.cid_) {
    const Index fdarray = read_index(d, static_cast<std::size_t>(dict_num(topdict, 1236, 0)));
    for (const auto& [off, len] : fdarray.items) {
      if (impl->privates.size() >= 256) break;
      impl->privates.push_back(read_private(read_dict(d, off, len)));
    }
    const auto fds = static_cast<std::size_t>(dict_num(topdict, 1237, 0));
    impl->fd_select.assign(n_glyphs, 0);
    if (fds > 0 && fds < d.size()) {
      const std::uint8_t format = at(d, fds);
      if (format == 0) {
        for (std::size_t g = 0; g < n_glyphs; ++g) impl->fd_select[g] = at(d, fds + 1 + g);
      } else if (format == 3) {
        const std::uint32_t ranges = read_be(d, fds + 1, 2);
        for (std::uint32_t r = 0; r < ranges; ++r) {
          const std::size_t rec = fds + 3 + 3ull * r;
          const std::uint32_t first = read_be(d, rec, 2), next = read_be(d, rec + 3, 2);
          const std::uint8_t fd = at(d, rec + 2);
          for (std::uint32_t g = first; g < next && g < n_glyphs; ++g) impl->fd_select[g] = fd;
        }
      }
    }
  }
  if (impl->privates.empty()) impl->privates.push_back(read_private(topdict));
  // charset
  impl->charset.assign(n_glyphs, 0);
  const auto cs = static_cast<std::size_t>(dict_num(topdict, 15, 0));
  if (cs == 0 || cs > 2) {
    for (std::size_t g = 0; g < n_glyphs; ++g) impl->charset[g] = static_cast<std::uint16_t>(g);
    if (cs > 2 && cs < d.size()) {
      const std::uint8_t format = at(d, cs);
      std::size_t p = cs + 1;
      std::size_t g = 1;
      if (format == 0) {
        for (; g < n_glyphs && p + 2 <= d.size(); ++g, p += 2) impl->charset[g] = static_cast<std::uint16_t>(read_be(d, p, 2));
      } else if (format == 1 || format == 2) {
        while (g < n_glyphs && p + (format == 1 ? 3 : 4) <= d.size()) {
          const std::uint32_t first = read_be(d, p, 2);
          const std::uint32_t left = format == 1 ? at(d, p + 2) : read_be(d, p + 2, 2);
          p += format == 1 ? 3 : 4;
          for (std::uint32_t k = 0; k <= left && g < n_glyphs; ++k, ++g) {
            impl->charset[g] = static_cast<std::uint16_t>(first + k);
          }
        }
      }
    }
  } else {
    for (std::size_t g = 0; g < n_glyphs; ++g) impl->charset[g] = static_cast<std::uint16_t>(std::min<std::size_t>(g, 390));
  }
  // encoding
  const auto enc = static_cast<std::size_t>(dict_num(topdict, 16, 0));
  if (enc > 1 && enc < d.size() && !font.cid_) {
    impl->standard_encoding = false;
    const std::uint8_t format = at(d, enc);
    std::size_t p = enc + 1;
    if ((format & 0x7F) == 0) {
      const std::uint8_t n = at(d, p++);
      for (std::uint32_t g = 1; g <= n && g < n_glyphs; ++g) impl->encoding[at(d, p++)] = static_cast<std::uint16_t>(g);
    } else if ((format & 0x7F) == 1) {
      const std::uint8_t ranges = at(d, p++);
      std::uint32_t g = 1;
      for (std::uint32_t r = 0; r < ranges; ++r) {
        const std::uint32_t first = at(d, p), left = at(d, p + 1);
        p += 2;
        for (std::uint32_t k = 0; k <= left && g < n_glyphs; ++k, ++g) {
          if (first + k < 256) impl->encoding[first + k] = static_cast<std::uint16_t>(g);
        }
      }
    }
    if (format & 0x80) {
      const std::uint8_t sups = at(d, p++);
      for (std::uint32_t s = 0; s < sups; ++s) {
        const std::uint8_t code = at(d, p);
        const std::uint16_t sid = static_cast<std::uint16_t>(read_be(d, p + 1, 2));
        p += 3;
        for (std::size_t g = 0; g < n_glyphs; ++g) {
          if (impl->charset[g] == sid) {
            impl->encoding[code] = static_cast<std::uint16_t>(g);
            break;
          }
        }
      }
    }
  }
  font.impl_ = impl;
  return font;
}

std::uint16_t Cff::num_glyphs() const noexcept {
  return static_cast<std::uint16_t>(std::min<std::size_t>(impl_->charstrings.items.size(), 0xFFFF));
}

std::optional<std::uint16_t> Cff::glyph_for_name(std::string_view name) const {
  if (cid_) return std::nullopt;
  for (std::size_t g = 0; g < impl_->charset.size(); ++g) {
    if (impl_->sid_name(impl_->charset[g]) == name) return static_cast<std::uint16_t>(g);
  }
  return std::nullopt;
}

std::optional<std::uint16_t> Cff::glyph_for_cid(std::uint16_t cid) const {
  if (!cid_) {
    if (cid < impl_->charstrings.items.size()) return cid;
    return std::nullopt;
  }
  if (cid == 0) return 0;
  for (std::size_t g = 0; g < impl_->charset.size(); ++g) {
    if (impl_->charset[g] == cid) return static_cast<std::uint16_t>(g);
  }
  return std::nullopt;
}

std::optional<std::uint16_t> Cff::glyph_for_code(std::uint8_t code) const {
  if (!impl_->standard_encoding) {
    const std::uint16_t g = impl_->encoding[code];
    if (g != 0) return g;
    return std::nullopt;
  }
  const std::string_view name = encoding_glyph_name(BaseEncoding::Standard, code);
  if (name.empty()) return std::nullopt;
  return glyph_for_name(name);
}

std::string Cff::glyph_name(std::uint16_t gid) const {
  if (cid_ || gid >= impl_->charset.size()) return {};
  return impl_->sid_name(impl_->charset[gid]);
}

std::array<std::string, 256> Cff::builtin_encoding() const {
  std::array<std::string, 256> names;
  for (int c = 0; c < 256; ++c) {
    const auto g = glyph_for_code(static_cast<std::uint8_t>(c));
    if (g) names[static_cast<std::size_t>(c)] = glyph_name(*g);
  }
  return names;
}

Path Cff::outline(std::uint16_t gid) const { return impl_->outline(gid, nullptr, 0); }

double Cff::advance(std::uint16_t gid) const {
  double width = 0;
  (void)impl_->outline(gid, &width, 0);
  return width;
}

Matrix Cff::font_matrix(std::uint16_t gid) const {
  if (cid_) {
    const auto& priv = impl_->private_for(gid);
    if (priv.has_matrix) return impl_->top_has_matrix ? Matrix::concat(priv.matrix, impl_->top_matrix) : priv.matrix;
  }
  return impl_->top_matrix;
}

}  // namespace cvt::pdf::font
