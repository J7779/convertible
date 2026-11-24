// Convertible core - vector family: PostScript standard encodings and glyph names.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <array>
#include <string>
#include <unordered_map>

#include "families/vector/ps_internal.hpp"
#include "util/text.hpp"

namespace cvt::vector::ps {

namespace {

constexpr std::array<std::string_view, 95> ascii_names = {
    "space",      "exclam",    "quotedbl",     "numbersign", "dollar",     "percent",    "ampersand",
    "quoteright", "parenleft", "parenright",   "asterisk",   "plus",       "comma",      "hyphen",
    "period",     "slash",     "zero",         "one",        "two",        "three",      "four",
    "five",       "six",       "seven",        "eight",      "nine",       "colon",      "semicolon",
    "less",       "equal",     "greater",      "question",   "at",         "A",          "B",
    "C",          "D",         "E",            "F",          "G",          "H",          "I",
    "J",          "K",         "L",            "M",          "N",          "O",          "P",
    "Q",          "R",         "S",            "T",          "U",          "V",          "W",
    "X",          "Y",         "Z",            "bracketleft", "backslash", "bracketright", "asciicircum",
    "underscore", "quoteleft", "a",            "b",          "c",          "d",          "e",
    "f",          "g",         "h",            "i",          "j",          "k",          "l",
    "m",          "n",         "o",            "p",          "q",          "r",          "s",
    "t",          "u",         "v",            "w",          "x",          "y",          "z",
    "braceleft",  "bar",       "braceright",   "asciitilde"};

constexpr std::array<std::string_view, 96> latin1_upper = {
    "space",       "exclamdown",  "cent",         "sterling",    "currency",     "yen",        "brokenbar",
    "section",     "dieresis",    "copyright",    "ordfeminine", "guillemotleft", "logicalnot", "hyphen",
    "registered",  "macron",      "degree",       "plusminus",   "twosuperior",  "threesuperior", "acute",
    "mu",          "paragraph",   "periodcentered", "cedilla",   "onesuperior",  "ordmasculine", "guillemotright",
    "onequarter",  "onehalf",     "threequarters", "questiondown", "Agrave",     "Aacute",     "Acircumflex",
    "Atilde",      "Adieresis",   "Aring",        "AE",          "Ccedilla",     "Egrave",     "Eacute",
    "Ecircumflex", "Edieresis",   "Igrave",       "Iacute",      "Icircumflex",  "Idieresis",  "Eth",
    "Ntilde",      "Ograve",      "Oacute",       "Ocircumflex", "Otilde",       "Odieresis",  "multiply",
    "Oslash",      "Ugrave",      "Uacute",       "Ucircumflex", "Udieresis",    "Yacute",     "Thorn",
    "germandbls",  "agrave",      "aacute",       "acircumflex", "atilde",       "adieresis",  "aring",
    "ae",          "ccedilla",    "egrave",       "eacute",      "ecircumflex",  "edieresis",  "igrave",
    "iacute",      "icircumflex", "idieresis",    "eth",         "ntilde",       "ograve",     "oacute",
    "ocircumflex", "otilde",      "odieresis",    "divide",      "oslash",       "ugrave",     "uacute",
    "ucircumflex", "udieresis",   "yacute",       "thorn",       "ydieresis"};

constexpr std::pair<std::string_view, char32_t> extra_names[] = {
    {"quoteright", 0x2019}, {"quoteleft", 0x2018}, {"quotesingle", 0x27}, {"grave", 0x60},
    {"minus", 0x2212}, {"fraction", 0x2044}, {"florin", 0x192}, {"quotedblleft", 0x201C},
    {"quotedblright", 0x201D}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A}, {"fi", 0xFB01},
    {"fl", 0xFB02}, {"endash", 0x2013}, {"emdash", 0x2014}, {"dagger", 0x2020}, {"daggerdbl", 0x2021},
    {"bullet", 0x2022}, {"quotesinglbase", 0x201A}, {"quotedblbase", 0x201E}, {"ellipsis", 0x2026},
    {"perthousand", 0x2030}, {"circumflex", 0x2C6}, {"tilde", 0x2DC}, {"breve", 0x2D8},
    {"dotaccent", 0x2D9}, {"ring", 0x2DA}, {"hungarumlaut", 0x2DD}, {"ogonek", 0x2DB}, {"caron", 0x2C7},
    {"Lslash", 0x141}, {"lslash", 0x142}, {"OE", 0x152}, {"oe", 0x153}, {"dotlessi", 0x131},
    {"Scaron", 0x160}, {"scaron", 0x161}, {"Zcaron", 0x17D}, {"zcaron", 0x17E}, {"Ydieresis", 0x178},
    {"trademark", 0x2122}, {"Euro", 0x20AC}, {"nbspace", 0xA0}, {"sfthyphen", 0xAD}, {"middot", 0xB7},
    {"hyphen", 0x2D}, {"space", 0x20}, {"dieresis", 0xA8}, {"acute", 0xB4}, {"cedilla", 0xB8},
    {"macron", 0xAF}, {"Delta", 0x2206}, {"Omega", 0x2126}, {"mu", 0xB5}, {"pi", 0x3C0},
    {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4}, {"epsilon", 0x3B5},
    {"theta", 0x3B8}, {"lambda", 0x3BB}, {"sigma", 0x3C3}, {"tau", 0x3C4}, {"phi", 0x3C6},
    {"omega", 0x3C9}, {"infinity", 0x221E}, {"lessequal", 0x2264}, {"greaterequal", 0x2265},
    {"notequal", 0x2260}, {"approxequal", 0x2248}, {"partialdiff", 0x2202}, {"summation", 0x2211},
    {"product", 0x220F}, {"radical", 0x221A}, {"integral", 0x222B}, {"lozenge", 0x25CA},
    {"periodcentered", 0xB7}, {"exclamdown", 0xA1}, {"section", 0xA7}, {"degree", 0xB0},
    {"Gbreve", 0x11E}, {"gbreve", 0x11F}, {"Idotaccent", 0x130}, {"Scedilla", 0x15E}, {"scedilla", 0x15F},
    {"Cacute", 0x106}, {"cacute", 0x107}, {"Ccaron", 0x10C}, {"ccaron", 0x10D}, {"dcroat", 0x111},
    {"uni00A0", 0xA0}, {"ff", 0xFB00}, {"ffi", 0xFB03}, {"ffl", 0xFB04}};

std::array<std::string_view, 256> make_standard() {
  std::array<std::string_view, 256> e{};
  for (std::size_t i = 0; i < 256; ++i) e[i] = ".notdef";
  for (std::size_t i = 0; i < ascii_names.size(); ++i) e[32 + i] = ascii_names[i];
  const std::pair<int, std::string_view> high[] = {
      {161, "exclamdown"},   {162, "cent"},          {163, "sterling"},     {164, "fraction"},
      {165, "yen"},          {166, "florin"},        {167, "section"},      {168, "currency"},
      {169, "quotesingle"},  {170, "quotedblleft"},  {171, "guillemotleft"}, {172, "guilsinglleft"},
      {173, "guilsinglright"}, {174, "fi"},          {175, "fl"},           {177, "endash"},
      {178, "dagger"},       {179, "daggerdbl"},     {180, "periodcentered"}, {182, "paragraph"},
      {183, "bullet"},       {184, "quotesinglbase"}, {185, "quotedblbase"}, {186, "quotedblright"},
      {187, "guillemotright"}, {188, "ellipsis"},    {189, "perthousand"},  {191, "questiondown"},
      {193, "grave"},        {194, "acute"},         {195, "circumflex"},   {196, "tilde"},
      {197, "macron"},       {198, "breve"},         {199, "dotaccent"},    {200, "dieresis"},
      {202, "ring"},         {203, "cedilla"},       {205, "hungarumlaut"}, {206, "ogonek"},
      {207, "caron"},        {208, "emdash"},        {225, "AE"},           {227, "ordfeminine"},
      {232, "Lslash"},       {233, "Oslash"},        {234, "OE"},           {235, "ordmasculine"},
      {241, "ae"},           {245, "dotlessi"},      {248, "lslash"},       {249, "oslash"},
      {250, "oe"},           {251, "germandbls"}};
  for (const auto& [code, name] : high) e[static_cast<std::size_t>(code)] = name;
  return e;
}

std::array<std::string_view, 256> make_isolatin1() {
  std::array<std::string_view, 256> e{};
  for (std::size_t i = 0; i < 256; ++i) e[i] = ".notdef";
  for (std::size_t i = 0; i < ascii_names.size(); ++i) e[32 + i] = ascii_names[i];
  e[45] = "minus";
  const std::pair<int, std::string_view> accents[] = {
      {144, "dotlessi"}, {145, "grave"}, {146, "acute"}, {147, "circumflex"}, {148, "tilde"},
      {149, "macron"},   {150, "breve"}, {151, "dotaccent"}, {152, "dieresis"}, {154, "ring"},
      {155, "cedilla"},  {157, "hungarumlaut"}, {158, "ogonek"}, {159, "caron"}};
  for (const auto& [code, name] : accents) e[static_cast<std::size_t>(code)] = name;
  for (std::size_t i = 0; i < latin1_upper.size(); ++i) e[160 + i] = latin1_upper[i];
  return e;
}

const std::unordered_map<std::string_view, char32_t>& name_table() {
  static const std::unordered_map<std::string_view, char32_t> table = [] {
    std::unordered_map<std::string_view, char32_t> t;
    for (std::size_t i = 0; i < ascii_names.size(); ++i) t.emplace(ascii_names[i], static_cast<char32_t>(32 + i));
    for (std::size_t i = 0; i < latin1_upper.size(); ++i) t.emplace(latin1_upper[i], static_cast<char32_t>(160 + i));
    for (const auto& [name, cp] : extra_names) t[name] = cp;
    return t;
  }();
  return table;
}

}  // namespace

const std::array<std::string_view, 256>& standard_encoding() {
  static const std::array<std::string_view, 256> e = make_standard();
  return e;
}

const std::array<std::string_view, 256>& isolatin1_encoding() {
  static const std::array<std::string_view, 256> e = make_isolatin1();
  return e;
}

char32_t glyph_unicode(std::string_view name) {
  if (name.empty() || name == ".notdef") return 0;
  const auto& t = name_table();
  if (const auto it = t.find(name); it != t.end()) return it->second;
  // uniXXXX / uXXXX[XX]
  if (name.size() == 7 && name.starts_with("uni")) {
    if (const auto v = util::parse_int(name.substr(3), 16)) return static_cast<char32_t>(*v);
  }
  if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u') {
    if (const auto v = util::parse_int(name.substr(1), 16); v && *v > 0 && *v <= 0x10FFFF) {
      return static_cast<char32_t>(*v);
    }
  }
  // Suffixed variants (a.sc, one.oldstyle).
  if (const std::size_t dot = name.find('.'); dot != std::string_view::npos && dot > 0) {
    return glyph_unicode(name.substr(0, dot));
  }
  return 0;
}

std::string_view unicode_glyph_name(char32_t cp) {
  static const std::unordered_map<char32_t, std::string_view> reverse = [] {
    // Deterministic: the standard encodings' names first, then the extras.
    std::unordered_map<char32_t, std::string_view> r;
    for (std::size_t i = 0; i < ascii_names.size(); ++i) r.emplace(static_cast<char32_t>(32 + i), ascii_names[i]);
    for (const auto& [name, code] : extra_names) r.emplace(code, name);
    for (std::size_t i = 0; i < latin1_upper.size(); ++i) r.emplace(static_cast<char32_t>(160 + i), latin1_upper[i]);
    r[0x2D] = "hyphen";
    r[0x27] = "quotesingle";
    r[0x60] = "grave";
    r[0x20] = "space";
    r[0xA0] = "space";
    r[0xAD] = "hyphen";
    return r;
  }();
  if (const auto it = reverse.find(cp); it != reverse.end()) return it->second;
  return {};
}

}  // namespace cvt::vector::ps
