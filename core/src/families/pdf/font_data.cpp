// Convertible core - pdf family: encodings, glyph names and standard-14 metrics.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/font_data.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

#include "util/text.hpp"

namespace cvt::pdf::font {

namespace {

using std::string_view;

// --------------------------------------------------------------------------- encodings

struct CodeName {
  std::uint8_t code;
  const char* name;
};

constexpr const char* kAsciiCommon[95] = {
    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quoteright", "parenleft",
    "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two", "three", "four",
    "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at", "A",
    "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "quoteleft", "a", "b", "c", "d",
    "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "braceleft", "bar", "braceright", "asciitilde"};

constexpr const char* kLatin1Upper[64] = {
    "Agrave", "Aacute", "Acircumflex", "Atilde", "Adieresis", "Aring", "AE", "Ccedilla", "Egrave", "Eacute",
    "Ecircumflex", "Edieresis", "Igrave", "Iacute", "Icircumflex", "Idieresis", "Eth", "Ntilde", "Ograve", "Oacute",
    "Ocircumflex", "Otilde", "Odieresis", "multiply", "Oslash", "Ugrave", "Uacute", "Ucircumflex", "Udieresis",
    "Yacute", "Thorn", "germandbls", "agrave", "aacute", "acircumflex", "atilde", "adieresis", "aring", "ae",
    "ccedilla", "egrave", "eacute", "ecircumflex", "edieresis", "igrave", "iacute", "icircumflex", "idieresis", "eth",
    "ntilde", "ograve", "oacute", "ocircumflex", "otilde", "odieresis", "divide", "oslash", "ugrave", "uacute",
    "ucircumflex", "udieresis", "yacute", "thorn", "ydieresis"};

constexpr const char* kLatin1Mid[32] = {  // 0xA0..0xBF (WinAnsi / PDFDoc)
    "space", "exclamdown", "cent", "sterling", "currency", "yen", "brokenbar", "section", "dieresis", "copyright",
    "ordfeminine", "guillemotleft", "logicalnot", "hyphen", "registered", "macron", "degree", "plusminus",
    "twosuperior", "threesuperior", "acute", "mu", "paragraph", "periodcentered", "cedilla", "onesuperior",
    "ordmasculine", "guillemotright", "onequarter", "onehalf", "threequarters", "questiondown"};

constexpr CodeName kStandardHigh[] = {
    {161, "exclamdown"}, {162, "cent"}, {163, "sterling"}, {164, "fraction"}, {165, "yen"}, {166, "florin"},
    {167, "section"}, {168, "currency"}, {169, "quotesingle"}, {170, "quotedblleft"}, {171, "guillemotleft"},
    {172, "guilsinglleft"}, {173, "guilsinglright"}, {174, "fi"}, {175, "fl"}, {177, "endash"}, {178, "dagger"},
    {179, "daggerdbl"}, {180, "periodcentered"}, {182, "paragraph"}, {183, "bullet"}, {184, "quotesinglbase"},
    {185, "quotedblbase"}, {186, "quotedblright"}, {187, "guillemotright"}, {188, "ellipsis"}, {189, "perthousand"},
    {191, "questiondown"}, {193, "grave"}, {194, "acute"}, {195, "circumflex"}, {196, "tilde"}, {197, "macron"},
    {198, "breve"}, {199, "dotaccent"}, {200, "dieresis"}, {202, "ring"}, {203, "cedilla"}, {205, "hungarumlaut"},
    {206, "ogonek"}, {207, "caron"}, {208, "emdash"}, {225, "AE"}, {227, "ordfeminine"}, {232, "Lslash"},
    {233, "Oslash"}, {234, "OE"}, {235, "ordmasculine"}, {241, "ae"}, {245, "dotlessi"}, {248, "lslash"},
    {249, "oslash"}, {250, "oe"}, {251, "germandbls"}};

constexpr CodeName kWinAnsiHigh[] = {
    {127, "bullet"}, {128, "Euro"}, {129, "bullet"}, {130, "quotesinglbase"}, {131, "florin"},
    {132, "quotedblbase"}, {133, "ellipsis"}, {134, "dagger"}, {135, "daggerdbl"}, {136, "circumflex"},
    {137, "perthousand"}, {138, "Scaron"}, {139, "guilsinglleft"}, {140, "OE"}, {141, "bullet"}, {142, "Zcaron"},
    {143, "bullet"}, {144, "bullet"}, {145, "quoteleft"}, {146, "quoteright"}, {147, "quotedblleft"},
    {148, "quotedblright"}, {149, "bullet"}, {150, "endash"}, {151, "emdash"}, {152, "tilde"}, {153, "trademark"},
    {154, "scaron"}, {155, "guilsinglright"}, {156, "oe"}, {157, "bullet"}, {158, "zcaron"}, {159, "Ydieresis"}};

constexpr const char* kMacRomanHigh[128] = {
    "Adieresis", "Aring", "Ccedilla", "Eacute", "Ntilde", "Odieresis", "Udieresis", "aacute", "agrave",
    "acircumflex", "adieresis", "atilde", "aring", "ccedilla", "eacute", "egrave", "ecircumflex", "edieresis",
    "iacute", "igrave", "icircumflex", "idieresis", "ntilde", "oacute", "ograve", "ocircumflex", "odieresis",
    "otilde", "uacute", "ugrave", "ucircumflex", "udieresis", "dagger", "degree", "cent", "sterling", "section",
    "bullet", "paragraph", "germandbls", "registered", "copyright", "trademark", "acute", "dieresis", "notequal", "AE",
    "Oslash", "infinity", "plusminus", "lessequal", "greaterequal", "yen", "mu", "partialdiff", "summation",
    "product", "pi", "integral", "ordfeminine", "ordmasculine", "Omega", "ae", "oslash", "questiondown",
    "exclamdown", "logicalnot", "radical", "florin", "approxequal", "Delta", "guillemotleft", "guillemotright",
    "ellipsis", "space", "Agrave", "Atilde", "Otilde", "OE", "oe", "endash", "emdash", "quotedblleft",
    "quotedblright", "quoteleft", "quoteright", "divide", "lozenge", "ydieresis", "Ydieresis", "fraction",
    "currency", "guilsinglleft", "guilsinglright", "fi", "fl", "daggerdbl", "periodcentered", "quotesinglbase",
    "quotedblbase", "perthousand", "Acircumflex", "Ecircumflex", "Aacute", "Edieresis", "Egrave", "Iacute",
    "Icircumflex", "Idieresis", "Igrave", "Oacute", "Ocircumflex", "apple", "Ograve", "Uacute", "Ucircumflex",
    "Ugrave", "dotlessi", "circumflex", "tilde", "macron", "breve", "dotaccent", "ring", "cedilla", "hungarumlaut",
    "ogonek", "caron"};

constexpr CodeName kPdfDocSpecial[] = {
    {24, "breve"}, {25, "caron"}, {26, "circumflex"}, {27, "dotaccent"}, {28, "hungarumlaut"}, {29, "ogonek"},
    {30, "ring"}, {31, "tilde"}, {128, "bullet"}, {129, "dagger"}, {130, "daggerdbl"}, {131, "ellipsis"},
    {132, "emdash"}, {133, "endash"}, {134, "florin"}, {135, "fraction"}, {136, "guilsinglleft"},
    {137, "guilsinglright"}, {138, "minus"}, {139, "perthousand"}, {140, "quotedblbase"}, {141, "quotedblleft"},
    {142, "quotedblright"}, {143, "quoteleft"}, {144, "quoteright"}, {145, "quotesinglbase"}, {146, "trademark"},
    {147, "fi"}, {148, "fl"}, {149, "Lslash"}, {150, "OE"}, {151, "Scaron"}, {152, "Ydieresis"}, {153, "Zcaron"},
    {154, "dotlessi"}, {155, "lslash"}, {156, "oe"}, {157, "scaron"}, {158, "zcaron"}, {160, "Euro"}};

constexpr CodeName kMacExpert[] = {
    {32, "space"}, {33, "exclamsmall"}, {34, "Hungarumlautsmall"}, {35, "centoldstyle"}, {36, "dollaroldstyle"},
    {37, "dollarsuperior"}, {38, "ampersandsmall"}, {39, "Acutesmall"}, {40, "parenleftsuperior"},
    {41, "parenrightsuperior"}, {42, "twodotenleader"}, {43, "onedotenleader"}, {44, "comma"}, {45, "hyphen"},
    {46, "period"}, {47, "fraction"}, {48, "zerooldstyle"}, {49, "oneoldstyle"}, {50, "twooldstyle"},
    {51, "threeoldstyle"}, {52, "fouroldstyle"}, {53, "fiveoldstyle"}, {54, "sixoldstyle"}, {55, "sevenoldstyle"},
    {56, "eightoldstyle"}, {57, "nineoldstyle"}, {58, "colon"}, {59, "semicolon"}, {61, "threequartersemdash"},
    {63, "questionsmall"}, {68, "Ethsmall"}, {71, "onequarter"}, {72, "onehalf"}, {73, "threequarters"},
    {74, "oneeighth"}, {75, "threeeighths"}, {76, "fiveeighths"}, {77, "seveneighths"}, {78, "onethird"},
    {79, "twothirds"}, {86, "ff"}, {87, "fi"}, {88, "fl"}, {89, "ffi"}, {90, "ffl"}, {91, "parenleftinferior"},
    {93, "parenrightinferior"}, {94, "Circumflexsmall"}, {95, "hypheninferior"}, {96, "Gravesmall"},
    {97, "Asmall"}, {98, "Bsmall"}, {99, "Csmall"}, {100, "Dsmall"}, {101, "Esmall"}, {102, "Fsmall"},
    {103, "Gsmall"}, {104, "Hsmall"}, {105, "Ismall"}, {106, "Jsmall"}, {107, "Ksmall"}, {108, "Lsmall"},
    {109, "Msmall"}, {110, "Nsmall"}, {111, "Osmall"}, {112, "Psmall"}, {113, "Qsmall"}, {114, "Rsmall"},
    {115, "Ssmall"}, {116, "Tsmall"}, {117, "Usmall"}, {118, "Vsmall"}, {119, "Wsmall"}, {120, "Xsmall"},
    {121, "Ysmall"}, {122, "Zsmall"}, {123, "colonmonetary"}, {124, "onefitted"}, {125, "rupiah"},
    {126, "Tildesmall"}};

constexpr const char* kSymbol[256] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "space", "exclam", "universal",
    "numbersign", "existential", "percent", "ampersand", "suchthat", "parenleft", "parenright", "asteriskmath",
    "plus", "comma", "minus", "period", "slash", "zero", "one", "two", "three", "four", "five", "six", "seven",
    "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "congruent", "Alpha", "Beta",
    "Chi", "Delta", "Epsilon", "Phi", "Gamma", "Eta", "Iota", "theta1", "Kappa", "Lambda", "Mu", "Nu", "Omicron",
    "Pi", "Theta", "Rho", "Sigma", "Tau", "Upsilon", "sigma1", "Omega", "Xi", "Psi", "Zeta", "bracketleft",
    "therefore", "bracketright", "perpendicular", "underscore", "radicalex", "alpha", "beta", "chi", "delta",
    "epsilon", "phi", "gamma", "eta", "iota", "phi1", "kappa", "lambda", "mu", "nu", "omicron", "pi", "theta", "rho",
    "sigma", "tau", "upsilon", "omega1", "omega", "xi", "psi", "zeta", "braceleft", "bar", "braceright", "similar",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "Euro", "Upsilon1",
    "minute", "lessequal", "fraction", "infinity", "florin", "club", "diamond", "heart", "spade", "arrowboth",
    "arrowleft", "arrowup", "arrowright", "arrowdown", "degree", "plusminus", "second", "greaterequal", "multiply",
    "proportional", "partialdiff", "bullet", "divide", "notequal", "equivalence", "approxequal", "ellipsis",
    "arrowvertex", "arrowhorizex", "carriagereturn", "aleph", "Ifraktur", "Rfraktur", "weierstrass",
    "circlemultiply", "circleplus", "emptyset", "intersection", "union", "propersuperset", "reflexsuperset",
    "notsubset", "propersubset", "reflexsubset", "element", "notelement", "angle", "gradient", "registerserif",
    "copyrightserif", "trademarkserif", "product", "radical", "dotmath", "logicalnot", "logicaland", "logicalor",
    "arrowdblboth", "arrowdblleft", "arrowdblup", "arrowdblright", "arrowdbldown", "lozenge", "angleleft",
    "registersans", "copyrightsans", "trademarksans", "summation", "parenlefttp", "parenleftex", "parenleftbt",
    "bracketlefttp", "bracketleftex", "bracketleftbt", "bracelefttp", "braceleftmid", "braceleftbt", "braceex",
    nullptr, "angleright", "integral", "integraltp", "integralex", "integralbt", "parenrighttp", "parenrightex",
    "parenrightbt", "bracketrighttp", "bracketrightex", "bracketrightbt", "bracerighttp", "bracerightmid",
    "bracerightbt", nullptr};

// ZapfDingbats codes map to the Unicode Dingbats block (names "a1".. are
// not needed for text extraction; uniXXXX names are synthesised instead).
constexpr char32_t zapf_unicode(std::uint8_t code) {
  if (code == 0x20) return 0x20;
  if (code >= 0x21 && code <= 0x7E) {
    switch (code) {
      case 0x25: return 0x260E;
      case 0x2A: return 0x261B;
      case 0x2B: return 0x261E;
      case 0x48: return 0x2605;
      case 0x6C: return 0x25CF;
      case 0x6E: return 0x25A0;
      case 0x73: return 0x25B2;
      case 0x74: return 0x25BC;
      case 0x75: return 0x25C6;
      case 0x77: return 0x25D7;
      default: return 0x2700 + (code - 0x20);
    }
  }
  if (code >= 0xA1 && code <= 0xA7) return 0x2761 + (code - 0xA1);
  if (code == 0xA8) return 0x2663;
  if (code == 0xA9) return 0x2666;
  if (code == 0xAA) return 0x2665;
  if (code == 0xAB) return 0x2660;
  if (code >= 0xAC && code <= 0xB5) return 0x2460 + (code - 0xAC);
  if (code == 0xD5) return 0x2192;
  if (code == 0xD6) return 0x2194;
  if (code == 0xD7) return 0x2195;
  if (code >= 0xB6 && code <= 0xFE) return 0x2776 + (code - 0xB6);
  return 0;
}

struct ZapfNames {
  char names[256][8] = {};
};

constexpr ZapfNames make_zapf_names() {
  ZapfNames z;
  for (int c = 0; c < 256; ++c) {
    const char32_t u = zapf_unicode(static_cast<std::uint8_t>(c));
    if (u == 0) continue;
    if (u == 0x20) {
      const char* s = "space";
      for (int i = 0; s[i]; ++i) z.names[c][i] = s[i];
      continue;
    }
    constexpr char hex[] = "0123456789ABCDEF";
    z.names[c][0] = 'u';
    z.names[c][1] = 'n';
    z.names[c][2] = 'i';
    for (int i = 0; i < 4; ++i) z.names[c][3 + i] = hex[(u >> (12 - 4 * i)) & 15];
  }
  return z;
}

constexpr ZapfNames kZapfNames = make_zapf_names();

struct Table256 {
  const char* names[256] = {};
};

constexpr Table256 make_table(BaseEncoding encoding) {
  Table256 t;
  if (encoding == BaseEncoding::Symbol) {
    for (int i = 0; i < 256; ++i) t.names[i] = kSymbol[i];
    return t;
  }
  if (encoding == BaseEncoding::MacExpert) {
    for (const CodeName& cn : kMacExpert) t.names[cn.code] = cn.name;
    return t;
  }
  for (int i = 0; i < 95; ++i) t.names[32 + i] = kAsciiCommon[i];
  if (encoding != BaseEncoding::Standard) {
    t.names[39] = "quotesingle";
    t.names[96] = "grave";
  }
  switch (encoding) {
    case BaseEncoding::Standard:
      for (const CodeName& cn : kStandardHigh) t.names[cn.code] = cn.name;
      break;
    case BaseEncoding::WinAnsi:
      for (const CodeName& cn : kWinAnsiHigh) t.names[cn.code] = cn.name;
      for (int i = 0; i < 32; ++i) t.names[0xA0 + i] = kLatin1Mid[i];
      for (int i = 0; i < 64; ++i) t.names[0xC0 + i] = kLatin1Upper[i];
      break;
    case BaseEncoding::MacRoman:
      for (int i = 0; i < 128; ++i) t.names[128 + i] = kMacRomanHigh[i];
      break;
    case BaseEncoding::PdfDoc:
      for (const CodeName& cn : kPdfDocSpecial) t.names[cn.code] = cn.name;
      for (int i = 1; i < 32; ++i) t.names[0xA0 + i] = kLatin1Mid[i];
      t.names[0xAD] = nullptr;
      for (int i = 0; i < 64; ++i) t.names[0xC0 + i] = kLatin1Upper[i];
      break;
    default: break;
  }
  return t;
}

constexpr Table256 kStandard = make_table(BaseEncoding::Standard);
constexpr Table256 kWinAnsi = make_table(BaseEncoding::WinAnsi);
constexpr Table256 kMacRoman = make_table(BaseEncoding::MacRoman);
constexpr Table256 kMacExpertTable = make_table(BaseEncoding::MacExpert);
constexpr Table256 kPdfDoc = make_table(BaseEncoding::PdfDoc);
constexpr Table256 kSymbolTable = make_table(BaseEncoding::Symbol);

// --------------------------------------------------------------------------- glyph list

struct NameCode {
  string_view name;
  char32_t cp;
};

constexpr NameCode kGlyphListUnsorted[] = {
    // ASCII
    {"space", 0x20}, {"exclam", 0x21}, {"quotedbl", 0x22}, {"numbersign", 0x23}, {"dollar", 0x24},
    {"percent", 0x25}, {"ampersand", 0x26}, {"quotesingle", 0x27}, {"parenleft", 0x28}, {"parenright", 0x29},
    {"asterisk", 0x2A}, {"plus", 0x2B}, {"comma", 0x2C}, {"hyphen", 0x2D}, {"period", 0x2E}, {"slash", 0x2F},
    {"zero", 0x30}, {"one", 0x31}, {"two", 0x32}, {"three", 0x33}, {"four", 0x34}, {"five", 0x35}, {"six", 0x36},
    {"seven", 0x37}, {"eight", 0x38}, {"nine", 0x39}, {"colon", 0x3A}, {"semicolon", 0x3B}, {"less", 0x3C},
    {"equal", 0x3D}, {"greater", 0x3E}, {"question", 0x3F}, {"at", 0x40}, {"bracketleft", 0x5B},
    {"backslash", 0x5C}, {"bracketright", 0x5D}, {"asciicircum", 0x5E}, {"underscore", 0x5F}, {"grave", 0x60},
    {"braceleft", 0x7B}, {"bar", 0x7C}, {"braceright", 0x7D}, {"asciitilde", 0x7E},
    // Latin-1
    {"nbspace", 0xA0}, {"nonbreakingspace", 0xA0}, {"exclamdown", 0xA1}, {"cent", 0xA2}, {"sterling", 0xA3},
    {"currency", 0xA4}, {"yen", 0xA5}, {"brokenbar", 0xA6}, {"section", 0xA7}, {"dieresis", 0xA8},
    {"copyright", 0xA9}, {"ordfeminine", 0xAA}, {"guillemotleft", 0xAB}, {"logicalnot", 0xAC},
    {"sfthyphen", 0xAD}, {"softhyphen", 0xAD}, {"registered", 0xAE}, {"macron", 0xAF}, {"degree", 0xB0},
    {"plusminus", 0xB1}, {"twosuperior", 0xB2}, {"threesuperior", 0xB3}, {"acute", 0xB4}, {"mu", 0xB5},
    {"paragraph", 0xB6}, {"periodcentered", 0xB7}, {"middot", 0xB7}, {"cedilla", 0xB8}, {"onesuperior", 0xB9},
    {"ordmasculine", 0xBA}, {"guillemotright", 0xBB}, {"onequarter", 0xBC}, {"onehalf", 0xBD},
    {"threequarters", 0xBE}, {"questiondown", 0xBF}, {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acircumflex", 0xC2},
    {"Atilde", 0xC3}, {"Adieresis", 0xC4}, {"Aring", 0xC5}, {"AE", 0xC6}, {"Ccedilla", 0xC7}, {"Egrave", 0xC8},
    {"Eacute", 0xC9}, {"Ecircumflex", 0xCA}, {"Edieresis", 0xCB}, {"Igrave", 0xCC}, {"Iacute", 0xCD},
    {"Icircumflex", 0xCE}, {"Idieresis", 0xCF}, {"Eth", 0xD0}, {"Ntilde", 0xD1}, {"Ograve", 0xD2},
    {"Oacute", 0xD3}, {"Ocircumflex", 0xD4}, {"Otilde", 0xD5}, {"Odieresis", 0xD6}, {"multiply", 0xD7},
    {"Oslash", 0xD8}, {"Ugrave", 0xD9}, {"Uacute", 0xDA}, {"Ucircumflex", 0xDB}, {"Udieresis", 0xDC},
    {"Yacute", 0xDD}, {"Thorn", 0xDE}, {"germandbls", 0xDF}, {"agrave", 0xE0}, {"aacute", 0xE1},
    {"acircumflex", 0xE2}, {"atilde", 0xE3}, {"adieresis", 0xE4}, {"aring", 0xE5}, {"ae", 0xE6},
    {"ccedilla", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecircumflex", 0xEA}, {"edieresis", 0xEB},
    {"igrave", 0xEC}, {"iacute", 0xED}, {"icircumflex", 0xEE}, {"idieresis", 0xEF}, {"eth", 0xF0},
    {"ntilde", 0xF1}, {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocircumflex", 0xF4}, {"otilde", 0xF5},
    {"odieresis", 0xF6}, {"divide", 0xF7}, {"oslash", 0xF8}, {"ugrave", 0xF9}, {"uacute", 0xFA},
    {"ucircumflex", 0xFB}, {"udieresis", 0xFC}, {"yacute", 0xFD}, {"thorn", 0xFE}, {"ydieresis", 0xFF},
    // Latin Extended-A
    {"Amacron", 0x100}, {"amacron", 0x101}, {"Abreve", 0x102}, {"abreve", 0x103}, {"Aogonek", 0x104},
    {"aogonek", 0x105}, {"Cacute", 0x106}, {"cacute", 0x107}, {"Ccircumflex", 0x108}, {"ccircumflex", 0x109},
    {"Cdotaccent", 0x10A}, {"cdotaccent", 0x10B}, {"Ccaron", 0x10C}, {"ccaron", 0x10D}, {"Dcaron", 0x10E},
    {"dcaron", 0x10F}, {"Dcroat", 0x110}, {"dcroat", 0x111}, {"Emacron", 0x112}, {"emacron", 0x113},
    {"Ebreve", 0x114}, {"ebreve", 0x115}, {"Edotaccent", 0x116}, {"edotaccent", 0x117}, {"Eogonek", 0x118},
    {"eogonek", 0x119}, {"Ecaron", 0x11A}, {"ecaron", 0x11B}, {"Gcircumflex", 0x11C}, {"gcircumflex", 0x11D},
    {"Gbreve", 0x11E}, {"gbreve", 0x11F}, {"Gdotaccent", 0x120}, {"gdotaccent", 0x121}, {"Gcommaaccent", 0x122},
    {"gcommaaccent", 0x123}, {"Hcircumflex", 0x124}, {"hcircumflex", 0x125}, {"Hbar", 0x126}, {"hbar", 0x127},
    {"Itilde", 0x128}, {"itilde", 0x129}, {"Imacron", 0x12A}, {"imacron", 0x12B}, {"Ibreve", 0x12C},
    {"ibreve", 0x12D}, {"Iogonek", 0x12E}, {"iogonek", 0x12F}, {"Idotaccent", 0x130}, {"dotlessi", 0x131},
    {"IJ", 0x132}, {"ij", 0x133}, {"Jcircumflex", 0x134}, {"jcircumflex", 0x135}, {"Kcommaaccent", 0x136},
    {"kcommaaccent", 0x137}, {"kgreenlandic", 0x138}, {"Lacute", 0x139}, {"lacute", 0x13A},
    {"Lcommaaccent", 0x13B}, {"lcommaaccent", 0x13C}, {"Lcaron", 0x13D}, {"lcaron", 0x13E}, {"Ldot", 0x13F},
    {"ldot", 0x140}, {"Lslash", 0x141}, {"lslash", 0x142}, {"Nacute", 0x143}, {"nacute", 0x144},
    {"Ncommaaccent", 0x145}, {"ncommaaccent", 0x146}, {"Ncaron", 0x147}, {"ncaron", 0x148},
    {"napostrophe", 0x149}, {"Eng", 0x14A}, {"eng", 0x14B}, {"Omacron", 0x14C}, {"omacron", 0x14D},
    {"Obreve", 0x14E}, {"obreve", 0x14F}, {"Ohungarumlaut", 0x150}, {"ohungarumlaut", 0x151}, {"OE", 0x152},
    {"oe", 0x153}, {"Racute", 0x154}, {"racute", 0x155}, {"Rcommaaccent", 0x156}, {"rcommaaccent", 0x157},
    {"Rcaron", 0x158}, {"rcaron", 0x159}, {"Sacute", 0x15A}, {"sacute", 0x15B}, {"Scircumflex", 0x15C},
    {"scircumflex", 0x15D}, {"Scedilla", 0x15E}, {"scedilla", 0x15F}, {"Scaron", 0x160}, {"scaron", 0x161},
    {"Tcommaaccent", 0x162}, {"tcommaaccent", 0x163}, {"Tcaron", 0x164}, {"tcaron", 0x165}, {"Tbar", 0x166},
    {"tbar", 0x167}, {"Utilde", 0x168}, {"utilde", 0x169}, {"Umacron", 0x16A}, {"umacron", 0x16B},
    {"Ubreve", 0x16C}, {"ubreve", 0x16D}, {"Uring", 0x16E}, {"uring", 0x16F}, {"Uhungarumlaut", 0x170},
    {"uhungarumlaut", 0x171}, {"Uogonek", 0x172}, {"uogonek", 0x173}, {"Wcircumflex", 0x174},
    {"wcircumflex", 0x175}, {"Ycircumflex", 0x176}, {"ycircumflex", 0x177}, {"Ydieresis", 0x178},
    {"Zacute", 0x179}, {"zacute", 0x17A}, {"Zdotaccent", 0x17B}, {"zdotaccent", 0x17C}, {"Zcaron", 0x17D},
    {"zcaron", 0x17E}, {"longs", 0x17F}, {"florin", 0x192}, {"Ohorn", 0x1A0}, {"ohorn", 0x1A1}, {"Uhorn", 0x1AF},
    {"uhorn", 0x1B0}, {"Gcaron", 0x1E6}, {"gcaron", 0x1E7}, {"Aringacute", 0x1FA}, {"aringacute", 0x1FB},
    {"AEacute", 0x1FC}, {"aeacute", 0x1FD}, {"Oslashacute", 0x1FE}, {"oslashacute", 0x1FF},
    {"Scommaaccent", 0x218}, {"scommaaccent", 0x219}, {"Tcedilla", 0x162}, {"tcedilla", 0x163},
    {"dotlessj", 0x237}, {"afii57929", 0x2BC}, {"afii64937", 0x2BD}, {"circumflex", 0x2C6}, {"caron", 0x2C7},
    {"breve", 0x2D8}, {"dotaccent", 0x2D9}, {"ring", 0x2DA}, {"ogonek", 0x2DB}, {"tilde", 0x2DC},
    {"hungarumlaut", 0x2DD}, {"gravecomb", 0x300}, {"acutecomb", 0x301}, {"tildecomb", 0x303},
    {"hookabovecomb", 0x309}, {"dotbelowcomb", 0x323}, {"tonos", 0x384}, {"dieresistonos", 0x385},
    // Greek
    {"Alphatonos", 0x386}, {"anoteleia", 0x387}, {"Epsilontonos", 0x388}, {"Etatonos", 0x389},
    {"Iotatonos", 0x38A}, {"Omicrontonos", 0x38C}, {"Upsilontonos", 0x38E}, {"Omegatonos", 0x38F},
    {"iotadieresistonos", 0x390}, {"Alpha", 0x391}, {"Beta", 0x392}, {"Gamma", 0x393}, {"Delta", 0x2206},
    {"Epsilon", 0x395}, {"Zeta", 0x396}, {"Eta", 0x397}, {"Theta", 0x398}, {"Iota", 0x399}, {"Kappa", 0x39A},
    {"Lambda", 0x39B}, {"Mu", 0x39C}, {"Nu", 0x39D}, {"Xi", 0x39E}, {"Omicron", 0x39F}, {"Pi", 0x3A0},
    {"Rho", 0x3A1}, {"Sigma", 0x3A3}, {"Tau", 0x3A4}, {"Upsilon", 0x3A5}, {"Phi", 0x3A6}, {"Chi", 0x3A7},
    {"Psi", 0x3A8}, {"Omega", 0x2126}, {"Iotadieresis", 0x3AA}, {"Upsilondieresis", 0x3AB},
    {"alphatonos", 0x3AC}, {"epsilontonos", 0x3AD}, {"etatonos", 0x3AE}, {"iotatonos", 0x3AF},
    {"upsilondieresistonos", 0x3B0}, {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4},
    {"epsilon", 0x3B5}, {"zeta", 0x3B6}, {"eta", 0x3B7}, {"theta", 0x3B8}, {"iota", 0x3B9}, {"kappa", 0x3BA},
    {"lambda", 0x3BB}, {"mu1", 0x3BC}, {"nu", 0x3BD}, {"xi", 0x3BE}, {"omicron", 0x3BF}, {"pi", 0x3C0},
    {"rho", 0x3C1}, {"sigma1", 0x3C2}, {"sigma", 0x3C3}, {"tau", 0x3C4}, {"upsilon", 0x3C5}, {"phi", 0x3C6},
    {"chi", 0x3C7}, {"psi", 0x3C8}, {"omega", 0x3C9}, {"iotadieresis", 0x3CA}, {"upsilondieresis", 0x3CB},
    {"omicrontonos", 0x3CC}, {"upsilontonos", 0x3CD}, {"omegatonos", 0x3CE}, {"theta1", 0x3D1},
    {"Upsilon1", 0x3D2}, {"phi1", 0x3D5}, {"omega1", 0x3D6},
    // General punctuation and symbols
    {"figuredash", 0x2012}, {"endash", 0x2013}, {"emdash", 0x2014}, {"afii00208", 0x2015},
    {"underscoredbl", 0x2017}, {"quoteleft", 0x2018}, {"quoteright", 0x2019}, {"quotesinglbase", 0x201A},
    {"quotereversed", 0x201B}, {"quotedblleft", 0x201C}, {"quotedblright", 0x201D}, {"quotedblbase", 0x201E},
    {"dagger", 0x2020}, {"daggerdbl", 0x2021}, {"bullet", 0x2022}, {"onedotenleader", 0x2024},
    {"twodotenleader", 0x2025}, {"ellipsis", 0x2026}, {"perthousand", 0x2030}, {"minute", 0x2032},
    {"second", 0x2033}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A}, {"exclamdbl", 0x203C},
    {"fraction", 0x2044}, {"zerosuperior", 0x2070}, {"foursuperior", 0x2074}, {"fivesuperior", 0x2075},
    {"sixsuperior", 0x2076}, {"sevensuperior", 0x2077}, {"eightsuperior", 0x2078}, {"ninesuperior", 0x2079},
    {"nsuperior", 0x207F}, {"zeroinferior", 0x2080}, {"oneinferior", 0x2081}, {"twoinferior", 0x2082},
    {"threeinferior", 0x2083}, {"fourinferior", 0x2084}, {"fiveinferior", 0x2085}, {"sixinferior", 0x2086},
    {"seveninferior", 0x2087}, {"eightinferior", 0x2088}, {"nineinferior", 0x2089}, {"colonmonetary", 0x20A1},
    {"franc", 0x20A3}, {"lira", 0x20A4}, {"peseta", 0x20A7}, {"dong", 0x20AB}, {"Euro", 0x20AC},
    {"afii61248", 0x2105}, {"Ifraktur", 0x2111}, {"afii61289", 0x2113}, {"afii61352", 0x2116},
    {"weierstrass", 0x2118}, {"Rfraktur", 0x211C}, {"prescription", 0x211E}, {"trademark", 0x2122},
    {"estimated", 0x212E}, {"aleph", 0x2135}, {"onethird", 0x2153}, {"twothirds", 0x2154},
    {"oneeighth", 0x215B}, {"threeeighths", 0x215C}, {"fiveeighths", 0x215D}, {"seveneighths", 0x215E},
    {"arrowleft", 0x2190}, {"arrowup", 0x2191}, {"arrowright", 0x2192}, {"arrowdown", 0x2193},
    {"arrowboth", 0x2194}, {"arrowupdn", 0x2195}, {"arrowupdnbse", 0x21A8}, {"carriagereturn", 0x21B5},
    {"arrowdblleft", 0x21D0}, {"arrowdblup", 0x21D1}, {"arrowdblright", 0x21D2}, {"arrowdbldown", 0x21D3},
    {"arrowdblboth", 0x21D4}, {"universal", 0x2200}, {"partialdiff", 0x2202}, {"existential", 0x2203},
    {"emptyset", 0x2205}, {"increment", 0x2206}, {"gradient", 0x2207}, {"nabla", 0x2207}, {"element", 0x2208},
    {"notelement", 0x2209}, {"suchthat", 0x220B}, {"product", 0x220F}, {"summation", 0x2211}, {"minus", 0x2212},
    {"asteriskmath", 0x2217}, {"radical", 0x221A}, {"proportional", 0x221D}, {"infinity", 0x221E},
    {"orthogonal", 0x221F}, {"angle", 0x2220}, {"logicaland", 0x2227}, {"logicalor", 0x2228},
    {"intersection", 0x2229}, {"union", 0x222A}, {"integral", 0x222B}, {"therefore", 0x2234},
    {"similar", 0x223C}, {"congruent", 0x2245}, {"approxequal", 0x2248}, {"notequal", 0x2260},
    {"equivalence", 0x2261}, {"lessequal", 0x2264}, {"greaterequal", 0x2265}, {"propersubset", 0x2282},
    {"propersuperset", 0x2283}, {"notsubset", 0x2284}, {"reflexsubset", 0x2286}, {"reflexsuperset", 0x2287},
    {"circleplus", 0x2295}, {"circlemultiply", 0x2297}, {"perpendicular", 0x22A5}, {"dotmath", 0x22C5},
    {"house", 0x2302}, {"revlogicalnot", 0x2310}, {"integraltp", 0x2320}, {"integralbt", 0x2321},
    {"angleleft", 0x2329}, {"angleright", 0x232A}, {"SF100000", 0x2500}, {"filledbox", 0x25A0},
    {"H22073", 0x25A1}, {"triagup", 0x25B2}, {"triagrt", 0x25BA}, {"triagdn", 0x25BC}, {"triaglf", 0x25C4},
    {"lozenge", 0x25CA}, {"circle", 0x25CB}, {"H18533", 0x25CF}, {"invbullet", 0x25D8}, {"invcircle", 0x25D9},
    {"openbullet", 0x25E6}, {"smileface", 0x263A}, {"invsmileface", 0x263B}, {"sun", 0x263C}, {"female", 0x2640},
    {"male", 0x2642}, {"spade", 0x2660}, {"club", 0x2663}, {"heart", 0x2665}, {"diamond", 0x2666},
    {"musicalnote", 0x266A}, {"musicalnotedbl", 0x266B}, {"checkmark", 0x2713}, {"registerserif", 0xAE},
    {"copyrightserif", 0xA9}, {"trademarkserif", 0x2122}, {"registersans", 0xAE}, {"copyrightsans", 0xA9},
    {"trademarksans", 0x2122}, {"dotlessj", 0x237}, {"Ohm", 0x2126}, {"micro", 0xB5},
    // Ligatures decompose (handled as multi-character text below)
    {"apple", 0xF8FF}, {"visiblespace", 0x2423}, {"cwm", 0x200C}, {"perthousandzero", 0x2030},
    {"dieresisacute", 0x385}, {"commaaccent", 0x326}, {"hyphenminus", 0x2D}, {"uni00A0", 0xA0},
    {"quoteleftreversed", 0x201B}, {"periodcentered.loclCAT", 0xB7}, {"Dslash", 0x110}, {"dslash", 0x111},
    {"Gamma1", 0x393}, {"Sigma1", 0x3A3}, {"Omega1", 0x3A9}, {"Deltagreek", 0x394}, {"Omegagreek", 0x3A9},
    {"mugreek", 0x3BC}, {"bulletoperator", 0x2219}, {"divisionslash", 0x2215}, {"degreesign", 0xB0},
    {"arrowhorizex", 0x23AF}, {"arrowvertex", 0x23D0}, {"braceex", 0x23AA}, {"bracelefttp", 0x23A7},
    {"braceleftmid", 0x23A8}, {"braceleftbt", 0x23A9}, {"bracerighttp", 0x23AB}, {"bracerightmid", 0x23AC},
    {"bracerightbt", 0x23AD}, {"bracketlefttp", 0x23A1}, {"bracketleftex", 0x23A2}, {"bracketleftbt", 0x23A3},
    {"bracketrighttp", 0x23A4}, {"bracketrightex", 0x23A5}, {"bracketrightbt", 0x23A6}, {"parenlefttp", 0x239B},
    {"parenleftex", 0x239C}, {"parenleftbt", 0x239D}, {"parenrighttp", 0x239E}, {"parenrightex", 0x239F},
    {"parenrightbt", 0x23A0}, {"integralex", 0x23AE}, {"radicalex", 0x203E},
    // Hebrew basics (afii)
    {"afii57664", 0x5D0}, {"afii57665", 0x5D1}, {"afii57666", 0x5D2}, {"afii57667", 0x5D3}, {"afii57668", 0x5D4},
    {"afii57669", 0x5D5}, {"afii57670", 0x5D6}, {"afii57671", 0x5D7}, {"afii57672", 0x5D8}, {"afii57673", 0x5D9},
    {"afii57674", 0x5DA}, {"afii57675", 0x5DB}, {"afii57676", 0x5DC}, {"afii57677", 0x5DD}, {"afii57678", 0x5DE},
    {"afii57679", 0x5DF}, {"afii57680", 0x5E0}, {"afii57681", 0x5E1}, {"afii57682", 0x5E2}, {"afii57683", 0x5E3},
    {"afii57684", 0x5E4}, {"afii57685", 0x5E5}, {"afii57686", 0x5E6}, {"afii57687", 0x5E7}, {"afii57688", 0x5E8},
    {"afii57689", 0x5E9}, {"afii57690", 0x5EA},
    // Cyrillic irregulars (the regular runs are computed)
    {"afii10023", 0x401}, {"afii10051", 0x402}, {"afii10052", 0x403}, {"afii10053", 0x404}, {"afii10054", 0x405},
    {"afii10055", 0x406}, {"afii10056", 0x407}, {"afii10057", 0x408}, {"afii10058", 0x409}, {"afii10059", 0x40A},
    {"afii10060", 0x40B}, {"afii10061", 0x40C}, {"afii10062", 0x40E}, {"afii10145", 0x40F}, {"afii10071", 0x451},
    {"afii10099", 0x452}, {"afii10100", 0x453}, {"afii10101", 0x454}, {"afii10102", 0x455}, {"afii10103", 0x456},
    {"afii10104", 0x457}, {"afii10105", 0x458}, {"afii10106", 0x459}, {"afii10107", 0x45A}, {"afii10108", 0x45B},
    {"afii10109", 0x45C}, {"afii10110", 0x45E}, {"afii10193", 0x45F}, {"afii10050", 0x490}, {"afii10098", 0x491},
};

constexpr std::size_t kGlyphCount = std::size(kGlyphListUnsorted);

constexpr std::array<NameCode, kGlyphCount> make_sorted_glyph_list() {
  std::array<NameCode, kGlyphCount> list{};
  for (std::size_t i = 0; i < kGlyphCount; ++i) list[i] = kGlyphListUnsorted[i];
  std::sort(list.begin(), list.end(), [](const NameCode& a, const NameCode& b) { return a.name < b.name; });
  return list;
}

constexpr std::array<NameCode, kGlyphCount> kGlyphList = make_sorted_glyph_list();

std::optional<char32_t> lookup_glyph(string_view name) {
  const auto it = std::lower_bound(kGlyphList.begin(), kGlyphList.end(), name,
                                   [](const NameCode& e, string_view n) { return e.name < n; });
  if (it != kGlyphList.end() && it->name == name) return it->cp;
  return std::nullopt;
}

std::optional<std::uint32_t> parse_hex(string_view s) {
  if (s.empty() || s.size() > 6) return std::nullopt;
  std::uint32_t v = 0;
  for (const char c : s) {
    int d = -1;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;  // lowercase is not AGL-conformant but common
    if (d < 0) return std::nullopt;
    v = v * 16 + static_cast<std::uint32_t>(d);
  }
  return v;
}

bool valid_scalar(std::uint32_t v) { return v < 0x110000 && (v < 0xD800 || v > 0xDFFF); }

std::optional<std::u32string> component_to_unicode(string_view part) {
  if (part.empty()) return std::nullopt;
  // Ligatures by name decompose to their letters.
  if (part == "ff") return U"ff";
  if (part == "fi") return U"fi";
  if (part == "fl") return U"fl";
  if (part == "ffi") return U"ffi";
  if (part == "ffl") return U"ffl";
  if (part == "st" || part == "longs_t") return U"st";
  if (part == "IJ") return U"IJ";
  if (part == "ij") return U"ij";
  if (const auto cp = lookup_glyph(part)) return std::u32string(1, *cp);
  if (part.size() == 1) {
    const char c = part[0];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return std::u32string(1, static_cast<char32_t>(c));
  }
  if (part.starts_with("uni") && part.size() >= 7 && (part.size() - 3) % 4 == 0) {
    std::u32string out;
    for (std::size_t i = 3; i + 4 <= part.size(); i += 4) {
      const auto v = parse_hex(part.substr(i, 4));
      if (!v || !valid_scalar(*v)) return std::nullopt;
      out.push_back(static_cast<char32_t>(*v));
    }
    return out;
  }
  if (part.size() >= 5 && part.size() <= 7 && part[0] == 'u') {
    const auto v = parse_hex(part.substr(1));
    if (v && valid_scalar(*v)) return std::u32string(1, static_cast<char32_t>(*v));
  }
  if (part.starts_with("afii")) {
    const auto n = util::parse_int(part.substr(4));
    if (n) {
      if (*n >= 10017 && *n <= 10022) return std::u32string(1, static_cast<char32_t>(0x410 + (*n - 10017)));
      if (*n >= 10024 && *n <= 10049) return std::u32string(1, static_cast<char32_t>(0x416 + (*n - 10024)));
      if (*n >= 10065 && *n <= 10070) return std::u32string(1, static_cast<char32_t>(0x430 + (*n - 10065)));
      if (*n >= 10072 && *n <= 10097) return std::u32string(1, static_cast<char32_t>(0x436 + (*n - 10072)));
    }
  }
  // Common "Xsmall" names of expert fonts: map to the letter.
  if (part.size() == 6 && part.ends_with("small") && part[0] >= 'A' && part[0] <= 'Z') {
    return std::u32string(1, static_cast<char32_t>(part[0] - 'A' + 'a'));
  }
  if (part.ends_with("oldstyle")) {
    return component_to_unicode(part.substr(0, part.size() - 8));
  }
  return std::nullopt;
}

// --------------------------------------------------------------------------- standard 14 metrics

// Widths of the printable ASCII characters 32..126 (by code), per family.
constexpr std::uint16_t kHelveticaAscii[95] = {
    278, 278, 355, 556, 556, 889, 667, 222, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556,
    556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778,
    722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278,
    278, 278, 469, 556, 222, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};
constexpr std::uint16_t kHelveticaBoldAscii[95] = {
    278, 333, 474, 556, 556, 889, 722, 278, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556,
    556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778,
    722, 278, 556, 722, 611, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333,
    278, 333, 584, 556, 278, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
    611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584};
constexpr std::uint16_t kTimesAscii[95] = {
    250, 333, 408, 500, 500, 833, 778, 333, 333, 333, 500, 564, 250, 333, 250, 278, 500, 500, 500, 500,
    500, 500, 500, 500, 500, 500, 278, 278, 564, 564, 564, 444, 921, 722, 667, 667, 722, 611, 556, 722,
    722, 333, 389, 722, 611, 889, 722, 722, 556, 722, 667, 556, 611, 722, 722, 944, 722, 722, 611, 333,
    278, 333, 469, 500, 333, 444, 500, 444, 500, 444, 333, 500, 500, 278, 278, 500, 278, 778, 500, 500,
    500, 500, 333, 389, 278, 500, 500, 722, 500, 500, 444, 480, 200, 480, 541};
constexpr std::uint16_t kTimesBoldAscii[95] = {
    250, 333, 555, 500, 500, 1000, 833, 333, 333, 333, 500, 570, 250, 333, 250, 278, 500, 500, 500, 500,
    500, 500, 500, 500, 500, 500, 333, 333, 570, 570, 570, 500, 930, 722, 667, 722, 722, 667, 611, 778,
    778, 389, 500, 778, 667, 944, 722, 778, 611, 778, 722, 556, 667, 722, 722, 1000, 722, 722, 667, 333,
    278, 333, 581, 500, 333, 500, 556, 444, 556, 444, 333, 500, 556, 278, 333, 556, 278, 833, 556, 500,
    556, 556, 444, 389, 333, 556, 500, 722, 500, 500, 444, 394, 220, 394, 520};
constexpr std::uint16_t kTimesItalicAscii[95] = {
    250, 333, 420, 500, 500, 833, 778, 333, 333, 333, 500, 675, 250, 333, 250, 278, 500, 500, 500, 500,
    500, 500, 500, 500, 500, 500, 333, 333, 675, 675, 675, 500, 920, 611, 611, 667, 722, 611, 611, 722,
    722, 333, 444, 667, 556, 833, 667, 722, 611, 722, 611, 500, 556, 722, 611, 833, 611, 556, 556, 389,
    278, 389, 422, 500, 333, 500, 500, 444, 500, 444, 278, 500, 500, 278, 278, 444, 278, 722, 500, 500,
    500, 500, 389, 389, 278, 500, 444, 667, 444, 444, 389, 400, 275, 400, 541};
constexpr std::uint16_t kTimesBoldItalicAscii[95] = {
    250, 389, 555, 500, 500, 833, 778, 333, 333, 333, 500, 570, 250, 333, 250, 278, 500, 500, 500, 500,
    500, 500, 500, 500, 500, 500, 333, 333, 570, 570, 570, 500, 832, 667, 667, 667, 722, 667, 667, 722,
    778, 389, 500, 667, 611, 889, 722, 722, 611, 722, 667, 556, 611, 722, 667, 889, 667, 611, 611, 333,
    278, 333, 570, 500, 333, 500, 500, 444, 500, 444, 333, 500, 556, 278, 278, 500, 278, 778, 556, 500,
    500, 500, 389, 389, 278, 556, 444, 667, 500, 444, 389, 348, 220, 348, 570};

struct ExtraWidth {
  string_view name;
  std::uint16_t helvetica, helvetica_bold, times, times_bold, times_italic, times_bold_italic;
};

constexpr ExtraWidth kExtraWidths[] = {
    {"quotesingle", 191, 238, 180, 278, 214, 278}, {"grave", 333, 333, 333, 333, 333, 333},
    {"endash", 556, 556, 500, 500, 500, 500}, {"emdash", 1000, 1000, 1000, 1000, 889, 1000},
    {"bullet", 350, 350, 350, 350, 350, 350}, {"quotedblleft", 333, 500, 444, 500, 556, 500},
    {"quotedblright", 333, 500, 444, 500, 556, 500}, {"quotesinglbase", 222, 278, 333, 333, 333, 333},
    {"quotedblbase", 333, 500, 444, 500, 556, 500}, {"ellipsis", 1000, 1000, 1000, 1000, 889, 1000},
    {"dagger", 556, 556, 500, 500, 500, 500}, {"daggerdbl", 556, 556, 500, 500, 500, 500},
    {"fi", 500, 611, 556, 556, 500, 556}, {"fl", 500, 611, 556, 556, 500, 556},
    {"perthousand", 1000, 1000, 1000, 1000, 1000, 1000}, {"trademark", 1000, 1000, 980, 1000, 980, 1000},
    {"copyright", 737, 737, 760, 747, 760, 747}, {"registered", 737, 737, 760, 747, 760, 747},
    {"degree", 400, 400, 400, 400, 400, 400}, {"plusminus", 584, 584, 564, 570, 675, 570},
    {"multiply", 584, 584, 564, 570, 675, 570}, {"divide", 584, 584, 564, 570, 675, 570},
    {"germandbls", 611, 611, 500, 556, 500, 500}, {"AE", 1000, 1000, 889, 1000, 889, 944},
    {"ae", 889, 889, 667, 722, 667, 722}, {"OE", 1000, 1000, 889, 1000, 944, 944},
    {"oe", 944, 944, 722, 722, 667, 722}, {"Oslash", 778, 778, 722, 778, 722, 722},
    {"oslash", 611, 611, 500, 500, 500, 500}, {"dotlessi", 278, 278, 278, 278, 278, 278},
    {"Lslash", 556, 611, 611, 667, 556, 611}, {"lslash", 222, 278, 278, 278, 278, 278},
    {"Eth", 722, 722, 722, 722, 722, 722}, {"eth", 556, 611, 500, 500, 500, 500},
    {"Thorn", 667, 667, 556, 611, 611, 611}, {"thorn", 556, 611, 500, 556, 500, 500},
    {"section", 556, 556, 500, 500, 500, 500}, {"paragraph", 537, 556, 453, 540, 523, 500},
    {"periodcentered", 278, 278, 250, 250, 250, 250}, {"guillemotleft", 556, 556, 500, 500, 500, 500},
    {"guillemotright", 556, 556, 500, 500, 500, 500}, {"guilsinglleft", 333, 333, 333, 333, 333, 333},
    {"guilsinglright", 333, 333, 333, 333, 333, 333}, {"exclamdown", 333, 333, 333, 333, 389, 389},
    {"questiondown", 611, 611, 444, 500, 500, 500}, {"cent", 556, 556, 500, 500, 500, 500},
    {"sterling", 556, 556, 500, 500, 500, 500}, {"yen", 556, 556, 500, 500, 500, 500},
    {"Euro", 556, 556, 500, 500, 500, 500}, {"currency", 556, 556, 500, 500, 500, 500},
    {"florin", 556, 556, 500, 500, 500, 500}, {"fraction", 167, 167, 167, 167, 167, 167},
    {"brokenbar", 260, 280, 200, 220, 275, 220}, {"logicalnot", 584, 584, 564, 570, 675, 606},
    {"mu", 556, 611, 500, 556, 500, 576}, {"minus", 584, 584, 564, 570, 675, 606},
    {"onehalf", 834, 834, 750, 750, 750, 750}, {"onequarter", 834, 834, 750, 750, 750, 750},
    {"threequarters", 834, 834, 750, 750, 750, 750}, {"onesuperior", 333, 333, 300, 300, 300, 300},
    {"twosuperior", 333, 333, 300, 300, 300, 300}, {"threesuperior", 333, 333, 300, 300, 300, 300},
    {"ordfeminine", 370, 370, 276, 300, 276, 266}, {"ordmasculine", 365, 365, 310, 330, 310, 300},
    {"acute", 333, 333, 333, 333, 333, 333}, {"dieresis", 333, 333, 333, 333, 333, 333},
    {"macron", 333, 333, 333, 333, 333, 333}, {"cedilla", 333, 333, 333, 333, 333, 333},
    {"circumflex", 333, 333, 333, 333, 333, 333}, {"tilde", 333, 333, 333, 333, 333, 333},
    {"caron", 333, 333, 333, 333, 333, 333}, {"breve", 333, 333, 333, 333, 333, 333},
    {"ring", 333, 333, 333, 333, 333, 333}, {"dotaccent", 333, 333, 333, 333, 333, 333},
    {"ogonek", 333, 333, 333, 333, 333, 333}, {"hungarumlaut", 333, 333, 333, 333, 333, 333},
    {"nbspace", 278, 278, 250, 250, 250, 250},
};

// Base letter of an accented glyph name ("Aacute" -> "A", "ccedilla" -> "c").
string_view base_letter(string_view name) {
  static constexpr string_view kAccents[] = {"acute", "grave", "circumflex", "dieresis", "tilde", "ring", "cedilla",
                                             "caron", "macron", "breve", "ogonek", "dotaccent", "hungarumlaut",
                                             "commaaccent", "slash", "acute"};
  if (name.size() < 2) return name;
  for (const string_view a : kAccents) {
    if (name.size() == a.size() + 1 && name.substr(1) == a) return name.substr(0, 1);
  }
  return name;
}

int family_index(Standard14 f) {
  switch (f) {
    case Standard14::Helvetica:
    case Standard14::HelveticaOblique: return 0;
    case Standard14::HelveticaBold:
    case Standard14::HelveticaBoldOblique: return 1;
    case Standard14::TimesRoman: return 2;
    case Standard14::TimesBold: return 3;
    case Standard14::TimesItalic: return 4;
    case Standard14::TimesBoldItalic: return 5;
    default: return -1;
  }
}

const std::uint16_t* ascii_table(int family) {
  switch (family) {
    case 0: return kHelveticaAscii;
    case 1: return kHelveticaBoldAscii;
    case 2: return kTimesAscii;
    case 3: return kTimesBoldAscii;
    case 4: return kTimesItalicAscii;
    default: return kTimesBoldItalicAscii;
  }
}

}  // namespace

string_view encoding_glyph_name(BaseEncoding encoding, std::uint8_t code) noexcept {
  const char* name = nullptr;
  switch (encoding) {
    case BaseEncoding::Standard: name = kStandard.names[code]; break;
    case BaseEncoding::WinAnsi: name = kWinAnsi.names[code]; break;
    case BaseEncoding::MacRoman: name = kMacRoman.names[code]; break;
    case BaseEncoding::MacExpert: name = kMacExpertTable.names[code]; break;
    case BaseEncoding::PdfDoc: name = kPdfDoc.names[code]; break;
    case BaseEncoding::Symbol: name = kSymbolTable.names[code]; break;
    case BaseEncoding::ZapfDingbats: name = kZapfNames.names[code][0] != 0 ? kZapfNames.names[code] : nullptr; break;
  }
  return name != nullptr ? string_view(name) : string_view();
}

std::optional<std::u32string> glyph_name_to_unicode(std::string_view name) {
  if (name.empty() || name == ".notdef") return std::nullopt;
  const std::size_t dot = name.find('.');
  if (dot == 0) return std::nullopt;
  if (dot != std::string_view::npos) {
    // Exact names containing a dot first (rare), then the base.
    if (const auto cp = lookup_glyph(name)) return std::u32string(1, *cp);
    name = name.substr(0, dot);
  }
  std::u32string out;
  std::size_t start = 0;
  while (start <= name.size()) {
    const std::size_t end = name.find('_', start);
    const string_view part = name.substr(start, end == string_view::npos ? string_view::npos : end - start);
    auto value = component_to_unicode(part);
    if (!value) {
      if (end == string_view::npos && start == 0) return std::nullopt;
      if (!part.empty()) return out.empty() ? std::nullopt : std::optional<std::u32string>(out);
    } else {
      out += *value;
    }
    if (end == string_view::npos) break;
    start = end + 1;
  }
  if (out.empty()) return std::nullopt;
  return out;
}

std::optional<char32_t> glyph_name_to_codepoint(std::string_view name) {
  const auto text = glyph_name_to_unicode(name);
  if (!text || text->size() != 1) {
    if (name == "fi") return 0xFB01;
    if (name == "fl") return 0xFB02;
    if (name == "ff") return 0xFB00;
    if (name == "ffi") return 0xFB03;
    if (name == "ffl") return 0xFB04;
    return std::nullopt;
  }
  return (*text)[0];
}

std::string codepoint_to_glyph_name(char32_t cp) {
  for (const NameCode& e : kGlyphList) {
    if (e.cp == cp && e.name.find('.') == string_view::npos) {
      // Prefer the AGLFN names that encodings use.
      if (cp < 0x80 && e.name.size() == 1) return std::string(e.name);
    }
  }
  if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return std::string(1, static_cast<char>(cp));
  for (int enc = 0; enc < 3; ++enc) {
    const Table256& t = enc == 0 ? kWinAnsi : (enc == 1 ? kStandard : kMacRoman);
    for (const char* n : t.names) {
      if (n != nullptr) {
        const auto c = lookup_glyph(n);
        if (c && *c == cp) return n;
      }
    }
  }
  constexpr char hex[] = "0123456789ABCDEF";
  std::string out = cp > 0xFFFF ? "u" : "uni";
  const int digits = cp > 0xFFFF ? 6 : 4;
  for (int i = digits - 1; i >= 0; --i) out.push_back(hex[(cp >> (4 * i)) & 15]);
  return out;
}

char32_t pdfdoc_to_unicode(std::uint8_t byte) noexcept {
  if (byte == 9 || byte == 10 || byte == 13) return byte;
  if (byte >= 0x20 && byte < 0x7F) return byte;
  if (byte >= 0xA1 && byte != 0xAD) return byte;
  const char* name = kPdfDoc.names[byte];
  if (name == nullptr) return 0;
  const string_view n(name);
  if (n == "fi") return 0xFB01;
  if (n == "fl") return 0xFB02;
  const auto cp = lookup_glyph(n);
  return cp.value_or(0);
}

std::optional<Standard14> standard14_from_name(std::string_view base) noexcept {
  if (base.size() > 7 && base[6] == '+') base.remove_prefix(7);
  std::string n;
  for (const char c : base) {
    if (c != ' ' && c != '-' && c != ',' && c != '_') n.push_back(util::ascii_lower(c));
  }
  const auto has = [&](string_view s) { return n.find(s) != std::string::npos; };
  const bool bold = has("bold") || has("black") || has("heavy") || has("semibold") || has("demi");
  const bool italic = has("italic") || has("oblique") || has("slanted");
  if (has("zapfdingbats") || has("dingbats")) return Standard14::ZapfDingbats;
  if (n == "symbol" || n == "symbolmt" || n.starts_with("symbol")) return Standard14::Symbol;
  if (has("courier") || has("mono") || has("consolas")) {
    if (bold && italic) return Standard14::CourierBoldOblique;
    if (bold) return Standard14::CourierBold;
    if (italic) return Standard14::CourierOblique;
    return Standard14::Courier;
  }
  const bool sans = has("sans");
  if (has("times") || (has("serif") && !sans) || has("georgia") || has("garamond") || has("cambria") ||
      has("minion") || has("palatino") || has("bookman") || has("century") || (has("roman") && !sans)) {
    if (bold && italic) return Standard14::TimesBoldItalic;
    if (bold) return Standard14::TimesBold;
    if (italic) return Standard14::TimesItalic;
    return Standard14::TimesRoman;
  }
  if (has("helvetica") || has("arial") || has("sans") || has("verdana") || has("tahoma") || has("calibri") ||
      has("segoe") || has("frutiger") || has("myriad") || has("inter")) {
    if (bold && italic) return Standard14::HelveticaBoldOblique;
    if (bold) return Standard14::HelveticaBold;
    if (italic) return Standard14::HelveticaOblique;
    return Standard14::Helvetica;
  }
  return std::nullopt;
}

std::optional<double> standard14_width(Standard14 font, std::string_view glyph_name) noexcept {
  if (font == Standard14::Courier || font == Standard14::CourierBold || font == Standard14::CourierOblique ||
      font == Standard14::CourierBoldOblique) {
    return 600.0;
  }
  if (font == Standard14::ZapfDingbats) return 788.0;
  const int family = font == Standard14::Symbol ? 2 : family_index(font);
  if (family < 0) return std::nullopt;
  const auto ascii_width = [&](string_view name) -> std::optional<double> {
    for (int i = 0; i < 95; ++i) {
      const char* common = kAsciiCommon[i];
      if (name == common) return static_cast<double>(ascii_table(family)[i]);
    }
    return std::nullopt;
  };
  if (font == Standard14::Symbol) {
    if (auto w = ascii_width(glyph_name)) return w;
    return 600.0;
  }
  for (const ExtraWidth& e : kExtraWidths) {
    if (e.name == glyph_name) {
      const std::uint16_t values[6] = {e.helvetica, e.helvetica_bold, e.times, e.times_bold, e.times_italic,
                                       e.times_bold_italic};
      return static_cast<double>(values[family]);
    }
  }
  if (auto w = ascii_width(glyph_name)) return w;
  if (auto w = ascii_width(base_letter(glyph_name))) return w;
  return std::nullopt;
}

Standard14Info standard14_info(Standard14 font) noexcept {
  Standard14Info info;
  switch (font) {
    case Standard14::Helvetica:
    case Standard14::HelveticaOblique:
      info = {718, -207, 718, false, false, false, font == Standard14::HelveticaOblique, false};
      break;
    case Standard14::HelveticaBold:
    case Standard14::HelveticaBoldOblique:
      info = {718, -207, 718, false, false, true, font == Standard14::HelveticaBoldOblique, false};
      break;
    case Standard14::TimesRoman: info = {683, -217, 662, true, false, false, false, false}; break;
    case Standard14::TimesBold: info = {683, -217, 676, true, false, true, false, false}; break;
    case Standard14::TimesItalic: info = {683, -217, 653, true, false, false, true, false}; break;
    case Standard14::TimesBoldItalic: info = {683, -217, 669, true, false, true, true, false}; break;
    case Standard14::Courier:
    case Standard14::CourierOblique:
      info = {629, -157, 562, true, true, false, font == Standard14::CourierOblique, false};
      break;
    case Standard14::CourierBold:
    case Standard14::CourierBoldOblique:
      info = {629, -157, 562, true, true, true, font == Standard14::CourierBoldOblique, false};
      break;
    case Standard14::Symbol: info = {1010, -293, 700, false, false, false, false, true}; break;
    case Standard14::ZapfDingbats: info = {820, -143, 700, false, false, false, false, true}; break;
  }
  return info;
}

}  // namespace cvt::pdf::font
