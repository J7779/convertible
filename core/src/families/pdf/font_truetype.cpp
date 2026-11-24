// Convertible core - pdf family: TrueType / sfnt parsing, outlines and subsetting.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <set>

#include "families/pdf/font_program.hpp"
#include "util/bytes.hpp"

namespace cvt::pdf::font {

namespace {

struct Reader {
  ByteView data;
  [[nodiscard]] bool has(std::size_t offset, std::size_t size) const noexcept {
    return offset <= data.size() && size <= data.size() - offset;
  }
  [[nodiscard]] std::uint8_t u8(std::size_t o) const noexcept {
    return o < data.size() ? std::to_integer<std::uint8_t>(data[o]) : 0;
  }
  [[nodiscard]] std::uint16_t u16(std::size_t o) const noexcept {
    return static_cast<std::uint16_t>((u8(o) << 8) | u8(o + 1));
  }
  [[nodiscard]] std::int16_t i16(std::size_t o) const noexcept { return static_cast<std::int16_t>(u16(o)); }
  [[nodiscard]] std::uint32_t u32(std::size_t o) const noexcept {
    return (static_cast<std::uint32_t>(u16(o)) << 16) | u16(o + 2);
  }
};

}  // namespace

std::optional<TrueType::Table> TrueType::table(std::string_view tag) const noexcept {
  const auto it = tables_.find(tag);
  if (it == tables_.end()) return std::nullopt;
  return it->second;
}

Matrix TrueType::font_matrix() const noexcept {
  const double s = 1.0 / std::max<int>(16, units_per_em_);
  return Matrix{s, 0, 0, s, 0, 0};
}

Result<TrueType> TrueType::parse(ByteView data, int face_index) {
  TrueType font;
  Reader r{data};
  if (data.size() < 12) return fail(Status::Decode, "font data is too short");
  std::size_t base = 0;
  const std::uint32_t tag = r.u32(0);
  if (tag == 0x74746366) {  // 'ttcf'
    const std::uint32_t count = r.u32(8);
    if (face_index < 0 || static_cast<std::uint32_t>(face_index) >= count || !r.has(12, 4ull * count)) {
      return fail(Status::Decode, "bad TrueType collection");
    }
    base = r.u32(12 + 4 * static_cast<std::size_t>(face_index));
  }
  const std::uint32_t version = r.u32(base);
  if (version != 0x00010000 && version != 0x74727565 && version != 0x00020000) {
    return fail(Status::Decode, "not a TrueType font");
  }
  const std::uint16_t num_tables = r.u16(base + 4);
  if (!r.has(base + 12, 16ull * num_tables)) return fail(Status::Decode, "truncated font table directory");
  font.data_.assign(data.begin(), data.end());
  const Reader fr{font.data_};
  for (std::size_t i = 0; i < num_tables; ++i) {
    const std::size_t rec = base + 12 + 16 * i;
    std::string name;
    for (int k = 0; k < 4; ++k) name.push_back(static_cast<char>(fr.u8(rec + static_cast<std::size_t>(k))));
    Table t{fr.u32(rec + 8), fr.u32(rec + 12)};
    if (!fr.has(t.offset, 0)) continue;
    t.length = static_cast<std::uint32_t>(std::min<std::uint64_t>(t.length, font.data_.size() - t.offset));
    font.tables_.emplace(name, t);
  }
  if (const auto head = font.table("head"); head && head->length >= 54) {
    font.units_per_em_ = fr.u16(head->offset + 18);
    if (font.units_per_em_ < 16 || font.units_per_em_ > 16384) font.units_per_em_ = 1000;
    font.bbox_ = Rect{static_cast<double>(fr.i16(head->offset + 36)), static_cast<double>(fr.i16(head->offset + 38)),
                      static_cast<double>(fr.i16(head->offset + 40)), static_cast<double>(fr.i16(head->offset + 42))};
    font.long_loca_ = fr.i16(head->offset + 50) != 0;
  }
  if (const auto maxp = font.table("maxp"); maxp && maxp->length >= 6) font.num_glyphs_ = fr.u16(maxp->offset + 4);
  if (const auto loca = font.table("loca"); loca && font.num_glyphs_ == 0) {
    font.num_glyphs_ = static_cast<std::uint16_t>(std::min<std::uint32_t>(
        0xFFFF, loca->length / (font.long_loca_ ? 4u : 2u) > 0 ? loca->length / (font.long_loca_ ? 4u : 2u) - 1 : 0));
  }
  if (const auto loca = font.table("loca")) {
    // Clamp the glyph count to what loca can index.
    const std::uint32_t entries = loca->length / (font.long_loca_ ? 4u : 2u);
    if (entries == 0) font.num_glyphs_ = 0;
    else font.num_glyphs_ = static_cast<std::uint16_t>(std::min<std::uint32_t>(font.num_glyphs_, entries - 1));
  }
  if (const auto hhea = font.table("hhea"); hhea && hhea->length >= 36) {
    font.ascent_ = fr.i16(hhea->offset + 4);
    font.descent_ = fr.i16(hhea->offset + 6);
    font.line_gap_ = fr.i16(hhea->offset + 8);
    font.num_hmetrics_ = fr.u16(hhea->offset + 34);
  }
  if (const auto os2 = font.table("OS/2"); os2 && os2->length >= 78) {
    font.weight_class_ = fr.u16(os2->offset + 4);
    font.italic_ = (fr.u16(os2->offset + 62) & 1) != 0;
    const double typo_asc = fr.i16(os2->offset + 68), typo_desc = fr.i16(os2->offset + 70);
    if (typo_asc > 0) {
      font.ascent_ = typo_asc;
      font.descent_ = typo_desc;
      font.line_gap_ = fr.i16(os2->offset + 72);
    }
    if (os2->length >= 90 && fr.u16(os2->offset) >= 2) font.cap_height_ = fr.i16(os2->offset + 88);
  }
  if (font.cap_height_ == 0) font.cap_height_ = font.ascent_ * 0.7;
  if (const auto post = font.table("post"); post && post->length >= 32) {
    font.italic_angle_ = static_cast<double>(static_cast<std::int32_t>(fr.u32(post->offset + 4))) / 65536.0;
    font.fixed_pitch_ = fr.u32(post->offset + 12) != 0;
    if (fr.u32(post->offset) == 0x00020000 && post->length >= 34) {
      const std::uint16_t n = fr.u16(post->offset + 32);
      const std::size_t idx = post->offset + 34;
      if (fr.has(idx, 2ull * n)) {
        std::vector<std::string> extra;
        std::size_t p = idx + 2ull * n;
        const std::size_t end = post->offset + post->length;
        while (p < end && extra.size() < 65536) {
          const std::size_t len = fr.u8(p);
          if (p + 1 + len > end) break;
          extra.emplace_back(reinterpret_cast<const char*>(font.data_.data()) + p + 1, len);
          p += 1 + len;
        }
        static constexpr const char* kMacNames[] = {
            ".notdef", ".null", "nonmarkingreturn", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent",
            "ampersand", "quotesingle", "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period",
            "slash", "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "colon",
            "semicolon", "less", "equal", "greater", "question", "at", "A", "B", "C", "D", "E", "F", "G", "H", "I",
            "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "bracketleft",
            "backslash", "bracketright", "asciicircum", "underscore", "grave", "a", "b", "c", "d", "e", "f", "g",
            "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
            "braceleft", "bar", "braceright", "asciitilde", "Adieresis", "Aring", "Ccedilla", "Eacute", "Ntilde",
            "Odieresis", "Udieresis", "aacute", "agrave", "acircumflex", "adieresis", "atilde", "aring", "ccedilla",
            "eacute", "egrave", "ecircumflex", "edieresis", "iacute", "igrave", "icircumflex", "idieresis",
            "ntilde", "oacute", "ograve", "ocircumflex", "odieresis", "otilde", "uacute", "ugrave", "ucircumflex",
            "udieresis", "dagger", "degree", "cent", "sterling", "section", "bullet", "paragraph", "germandbls",
            "registered", "copyright", "trademark", "acute", "dieresis", "notequal", "AE", "Oslash", "infinity",
            "plusminus", "lessequal", "greaterequal", "yen", "mu", "partialdiff", "summation", "product", "pi",
            "integral", "ordfeminine", "ordmasculine", "Omega", "ae", "oslash", "questiondown", "exclamdown",
            "logicalnot", "radical", "florin", "approxequal", "Delta", "guillemotleft", "guillemotright",
            "ellipsis", "nonbreakingspace", "Agrave", "Atilde", "Otilde", "OE", "oe", "endash", "emdash",
            "quotedblleft", "quotedblright", "quoteleft", "quoteright", "divide", "lozenge", "ydieresis",
            "Ydieresis", "fraction", "currency", "guilsinglleft", "guilsinglright", "fi", "fl", "daggerdbl",
            "periodcentered", "quotesinglbase", "quotedblbase", "perthousand", "Acircumflex", "Ecircumflex",
            "Aacute", "Edieresis", "Egrave", "Iacute", "Icircumflex", "Idieresis", "Igrave", "Oacute",
            "Ocircumflex", "apple", "Ograve", "Uacute", "Ucircumflex", "Ugrave", "dotlessi", "circumflex", "tilde",
            "macron", "breve", "dotaccent", "ring", "cedilla", "hungarumlaut", "ogonek", "caron", "Lslash",
            "lslash", "Scaron", "scaron", "Zcaron", "zcaron", "brokenbar", "Eth", "eth", "Yacute", "yacute", "Thorn",
            "thorn", "minus", "multiply", "onesuperior", "twosuperior", "threesuperior", "onehalf", "onequarter",
            "threequarters", "franc", "Gbreve", "gbreve", "Idotaccent", "Scedilla", "scedilla", "Cacute", "cacute",
            "Ccaron", "ccaron", "dcroat"};
        for (std::uint16_t g = 0; g < n; ++g) {
          const std::uint16_t index = fr.u16(idx + 2ull * g);
          std::string name;
          if (index < 258) name = kMacNames[index];
          else if (index - 258u < extra.size()) name = extra[index - 258u];
          if (!name.empty() && !font.post_names_.contains(name)) font.post_names_.emplace(std::move(name), g);
        }
      }
    }
  }
  if (const auto name = font.table("name"); name && name->length >= 6) {
    const std::uint16_t count = fr.u16(name->offset + 2);
    const std::size_t strings = name->offset + fr.u16(name->offset + 4);
    for (std::size_t i = 0; i < count && fr.has(name->offset + 6 + 12 * i, 12); ++i) {
      const std::size_t rec = name->offset + 6 + 12 * i;
      if (fr.u16(rec + 6) != 6) continue;
      const std::uint16_t platform = fr.u16(rec);
      const std::size_t len = fr.u16(rec + 8), off = strings + fr.u16(rec + 10);
      if (!fr.has(off, len)) continue;
      std::string ps;
      if (platform == 3 || platform == 0) {
        for (std::size_t k = 0; k + 1 < len; k += 2) ps.push_back(static_cast<char>(fr.u8(off + k + 1)));
      } else {
        for (std::size_t k = 0; k < len; ++k) ps.push_back(static_cast<char>(fr.u8(off + k)));
      }
      font.ps_name_ = ps;
      if (!ps.empty()) break;
    }
  }
  if (const auto cmap = font.table("cmap"); cmap && cmap->length >= 4) {
    const std::uint16_t count = fr.u16(cmap->offset + 2);
    for (std::size_t i = 0; i < count && fr.has(cmap->offset + 4 + 8 * i, 8); ++i) {
      const std::size_t rec = cmap->offset + 4 + 8 * i;
      CmapSub sub;
      sub.platform = fr.u16(rec);
      sub.encoding = fr.u16(rec + 2);
      sub.offset = cmap->offset + fr.u32(rec + 4);
      if (fr.has(sub.offset, 4)) font.cmaps_.push_back(sub);
    }
  }
  if (const auto kern = font.table("kern"); kern && kern->length >= 4 && fr.u16(kern->offset) == 0) {
    const std::uint16_t n = fr.u16(kern->offset + 2);
    std::size_t p = kern->offset + 4;
    for (std::uint16_t t = 0; t < n && fr.has(p, 6); ++t) {
      const std::uint16_t length = fr.u16(p + 2);
      const std::uint16_t coverage = fr.u16(p + 4);
      if ((coverage >> 8) == 0 && (coverage & 1) != 0 && fr.has(p + 6, 8)) {
        const std::uint16_t pairs = fr.u16(p + 6);
        for (std::size_t k = 0; k < pairs && fr.has(p + 14 + 6 * k, 6) && font.kern_pairs_.size() < 100000; ++k) {
          const std::size_t e = p + 14 + 6 * k;
          font.kern_pairs_[fr.u32(e)] = fr.i16(e + 4);
        }
      }
      if (length < 6) break;
      p += length;
    }
  }
  return font;
}

bool TrueType::has_cmap(std::uint16_t platform, std::uint16_t encoding) const noexcept {
  return std::any_of(cmaps_.begin(), cmaps_.end(),
                     [&](const CmapSub& s) { return s.platform == platform && s.encoding == encoding; });
}

std::optional<std::uint16_t> TrueType::glyph_for_cmap(std::uint16_t platform, std::uint16_t encoding,
                                                      std::uint32_t code) const {
  const Reader r{data_};
  for (const CmapSub& sub : cmaps_) {
    if (sub.platform != platform || sub.encoding != encoding) continue;
    const std::size_t o = sub.offset;
    const std::uint16_t format = r.u16(o);
    std::uint32_t gid = 0;
    switch (format) {
      case 0:
        if (code < 256 && r.has(o + 6, 256)) gid = r.u8(o + 6 + code);
        break;
      case 2: {
        if (!r.has(o + 6, 512)) break;
        const std::uint32_t high = code > 0xFF ? (code >> 8) & 0xFF : 0;
        const std::uint32_t low = code & 0xFF;
        const std::uint16_t key = r.u16(o + 6 + 2 * high) / 8;
        const std::size_t sh = o + 6 + 512 + 8ull * key;
        if (!r.has(sh, 8)) break;
        if (key == 0 && code > 0xFF) break;
        const std::uint16_t first = r.u16(sh), count = r.u16(sh + 2);
        const std::int16_t delta = r.i16(sh + 4);
        const std::uint16_t range_offset = r.u16(sh + 6);
        const std::uint32_t c = key == 0 ? code : low;
        if (c < first || c >= first + count) break;
        const std::size_t at = sh + 6 + range_offset + 2ull * (c - first);
        std::uint32_t g = r.has(at, 2) ? r.u16(at) : 0;
        if (g != 0) g = (g + static_cast<std::uint32_t>(delta)) & 0xFFFF;
        gid = g;
        break;
      }
      case 4: {
        if (code > 0xFFFF) break;
        const std::uint16_t segx2 = r.u16(o + 6);
        const std::size_t ends = o + 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;
        if (!r.has(ranges, segx2)) break;
        // Binary search over end codes.
        std::size_t lo = 0, hi = segx2 / 2;
        while (lo < hi) {
          const std::size_t mid = (lo + hi) / 2;
          if (r.u16(ends + 2 * mid) < code) lo = mid + 1;
          else hi = mid;
        }
        if (lo >= segx2 / 2u) break;
        const std::uint16_t start = r.u16(starts + 2 * lo);
        if (code < start) break;
        const std::uint16_t delta = r.u16(deltas + 2 * lo);
        const std::uint16_t range_offset = r.u16(ranges + 2 * lo);
        if (range_offset == 0) {
          gid = (code + delta) & 0xFFFF;
        } else {
          const std::size_t at = ranges + 2 * lo + range_offset + 2ull * (code - start);
          std::uint32_t g = r.has(at, 2) ? r.u16(at) : 0;
          if (g != 0) g = (g + delta) & 0xFFFF;
          gid = g;
        }
        break;
      }
      case 6: {
        const std::uint16_t first = r.u16(o + 6), count = r.u16(o + 8);
        if (code >= first && code < static_cast<std::uint32_t>(first) + count) gid = r.u16(o + 10 + 2ull * (code - first));
        break;
      }
      case 10: {
        const std::uint32_t first = r.u32(o + 12), count = r.u32(o + 16);
        if (code >= first && code - first < count && r.has(o + 20 + 2ull * (code - first), 2)) {
          gid = r.u16(o + 20 + 2ull * (code - first));
        }
        break;
      }
      case 12:
      case 13: {
        const std::uint32_t groups = r.u32(o + 12);
        if (!r.has(o + 16, 12ull * groups)) break;
        std::size_t lo = 0, hi = groups;
        while (lo < hi) {
          const std::size_t mid = (lo + hi) / 2;
          if (r.u32(o + 16 + 12 * mid + 4) < code) lo = mid + 1;
          else hi = mid;
        }
        if (lo < groups) {
          const std::size_t g = o + 16 + 12 * lo;
          const std::uint32_t start = r.u32(g), start_gid = r.u32(g + 8);
          if (code >= start) gid = format == 12 ? start_gid + (code - start) : start_gid;
        }
        break;
      }
      default: break;
    }
    if (gid != 0 && gid < num_glyphs_) return static_cast<std::uint16_t>(gid);
  }
  return std::nullopt;
}

std::optional<std::uint16_t> TrueType::glyph_for_unicode(char32_t cp) const {
  if (auto g = glyph_for_cmap(3, 10, cp)) return g;
  if (cp <= 0xFFFF) {
    if (auto g = glyph_for_cmap(3, 1, cp)) return g;
  }
  for (std::uint16_t enc = 0; enc <= 6; ++enc) {
    if (auto g = glyph_for_cmap(0, enc, cp)) return g;
  }
  return std::nullopt;
}

std::optional<std::uint16_t> TrueType::glyph_for_name(std::string_view name) const {
  const auto it = post_names_.find(name);
  if (it == post_names_.end() || it->second >= num_glyphs_) return std::nullopt;
  return it->second;
}

std::map<std::uint16_t, char32_t> TrueType::unicode_map() const {
  std::map<std::uint16_t, char32_t> out;
  const Reader r{data_};
  for (const CmapSub& sub : cmaps_) {
    const bool unicode = sub.platform == 0 || (sub.platform == 3 && (sub.encoding == 1 || sub.encoding == 10));
    if (!unicode) continue;
    const std::size_t o = sub.offset;
    const std::uint16_t format = r.u16(o);
    if (format == 4) {
      const std::uint16_t segx2 = r.u16(o + 6);
      const std::size_t ends = o + 14, starts = ends + segx2 + 2;
      for (std::size_t s = 0; s < segx2 / 2u && r.has(starts + 2 * s, 2); ++s) {
        const std::uint32_t start = r.u16(starts + 2 * s), end = r.u16(ends + 2 * s);
        for (std::uint32_t c = start; c <= end && c != 0xFFFF; ++c) {
          if (const auto g = glyph_for_cmap(sub.platform, sub.encoding, c); g && !out.contains(*g)) out[*g] = c;
        }
      }
    } else if (format == 12) {
      const std::uint32_t groups = r.u32(o + 12);
      for (std::size_t k = 0; k < groups && r.has(o + 16 + 12 * k, 12); ++k) {
        const std::size_t g = o + 16 + 12 * k;
        const std::uint32_t start = r.u32(g), end = r.u32(g + 4), sg = r.u32(g + 8);
        for (std::uint32_t c = start; c <= end && c - start < 0x10000 && c < 0x110000; ++c) {
          const std::uint32_t gid = sg + (c - start);
          if (gid < num_glyphs_ && !out.contains(static_cast<std::uint16_t>(gid))) out[static_cast<std::uint16_t>(gid)] = c;
        }
      }
    }
  }
  return out;
}

double TrueType::advance(std::uint16_t gid) const noexcept {
  const auto hmtx = table("hmtx");
  if (!hmtx || num_hmetrics_ == 0) return 0;
  const Reader r{data_};
  const std::uint16_t index = gid < num_hmetrics_ ? gid : static_cast<std::uint16_t>(num_hmetrics_ - 1);
  const std::size_t at = hmtx->offset + 4ull * index;
  if (at + 2 > hmtx->offset + hmtx->length) return 0;
  return r.u16(at);
}

double TrueType::kerning(std::uint16_t left, std::uint16_t right) const noexcept {
  const auto it = kern_pairs_.find((static_cast<std::uint32_t>(left) << 16) | right);
  return it == kern_pairs_.end() ? 0.0 : it->second;
}

void TrueType::outline_into(std::uint16_t gid, const Matrix& m, Path& out, int depth) const {
  if (depth > 8 || gid >= num_glyphs_) return;
  const auto loca = table("loca");
  const auto glyf = table("glyf");
  if (!loca || !glyf) return;
  const Reader r{data_};
  std::uint32_t start = 0, end = 0;
  if (long_loca_) {
    if (loca->length < 4ull * (gid + 2u)) return;
    start = r.u32(loca->offset + 4ull * gid);
    end = r.u32(loca->offset + 4ull * gid + 4);
  } else {
    if (loca->length < 2ull * (gid + 2u)) return;
    start = 2u * r.u16(loca->offset + 2ull * gid);
    end = 2u * r.u16(loca->offset + 2ull * gid + 2);
  }
  if (end <= start || end > glyf->length || end - start < 10) return;
  const std::size_t g = glyf->offset + start;
  const std::size_t gend = glyf->offset + end;
  const std::int16_t contours = r.i16(g);
  if (contours >= 0) {
    const std::size_t n_contours = static_cast<std::size_t>(contours);
    if (!r.has(g + 10, 2 * n_contours + 2)) return;
    std::vector<std::uint16_t> end_points(n_contours);
    for (std::size_t c = 0; c < n_contours; ++c) end_points[c] = r.u16(g + 10 + 2 * c);
    const std::size_t n_points = n_contours == 0 ? 0 : static_cast<std::size_t>(end_points.back()) + 1;
    if (n_points > 65535) return;
    const std::size_t instr_len = r.u16(g + 10 + 2 * n_contours);
    std::size_t p = g + 12 + 2 * n_contours + instr_len;
    std::vector<std::uint8_t> flags(n_points);
    for (std::size_t i = 0; i < n_points;) {
      if (p >= gend) return;
      const std::uint8_t f = r.u8(p++);
      flags[i++] = f;
      if (f & 8) {
        if (p >= gend) return;
        std::uint8_t repeat = r.u8(p++);
        while (repeat-- > 0 && i < n_points) flags[i++] = f;
      }
    }
    std::vector<double> xs(n_points), ys(n_points);
    int value = 0;
    for (std::size_t i = 0; i < n_points; ++i) {
      const std::uint8_t f = flags[i];
      if (f & 2) {
        if (p >= gend) return;
        const int d = r.u8(p++);
        value += (f & 16) ? d : -d;
      } else if (!(f & 16)) {
        if (p + 2 > gend) return;
        value += r.i16(p);
        p += 2;
      }
      xs[i] = value;
    }
    value = 0;
    for (std::size_t i = 0; i < n_points; ++i) {
      const std::uint8_t f = flags[i];
      if (f & 4) {
        if (p >= gend) return;
        const int d = r.u8(p++);
        value += (f & 32) ? d : -d;
      } else if (!(f & 32)) {
        if (p + 2 > gend) return;
        value += r.i16(p);
        p += 2;
      }
      ys[i] = value;
    }
    std::size_t first = 0;
    for (std::size_t c = 0; c < n_contours; ++c) {
      const std::size_t last = end_points[c];
      if (last < first || last >= n_points) break;
      const std::size_t count = last - first + 1;
      const auto pt = [&](std::size_t k) { return m.apply(Point{xs[first + k % count], ys[first + k % count]}); };
      const auto on = [&](std::size_t k) { return (flags[first + k % count] & 1) != 0; };
      // Find a starting on-curve point (or the midpoint of two off points).
      std::size_t s = 0;
      while (s < count && !on(s)) ++s;
      Point start_pt;
      if (s == count) {
        const Point a = pt(0), b = pt(1);
        start_pt = {(a.x + b.x) / 2, (a.y + b.y) / 2};
        s = 0;
      } else {
        start_pt = pt(s);
      }
      out.move_to(start_pt.x, start_pt.y);
      bool have_control = false;
      Point control;
      for (std::size_t k = 1; k <= count; ++k) {
        const std::size_t idx = s + k;
        const Point q = pt(idx);
        if (on(idx)) {
          if (have_control) out.quad_to(control.x, control.y, q.x, q.y);
          else out.line_to(q.x, q.y);
          have_control = false;
        } else {
          if (have_control) {
            const Point mid{(control.x + q.x) / 2, (control.y + q.y) / 2};
            out.quad_to(control.x, control.y, mid.x, mid.y);
          }
          control = q;
          have_control = true;
        }
      }
      if (have_control) out.quad_to(control.x, control.y, start_pt.x, start_pt.y);
      out.close();
      first = last + 1;
    }
    return;
  }
  // Composite glyph.
  std::size_t p = g + 10;
  for (int components = 0; components < 64; ++components) {
    if (p + 4 > gend) return;
    const std::uint16_t flags = r.u16(p);
    const std::uint16_t component = r.u16(p + 2);
    p += 4;
    double dx = 0, dy = 0;
    if (flags & 1) {
      if (p + 4 > gend) return;
      if (flags & 2) {
        dx = r.i16(p);
        dy = r.i16(p + 2);
      }
      p += 4;
    } else {
      if (p + 2 > gend) return;
      if (flags & 2) {
        dx = static_cast<std::int8_t>(r.u8(p));
        dy = static_cast<std::int8_t>(r.u8(p + 1));
      }
      p += 2;
    }
    double a = 1, b = 0, c = 0, d = 1;
    const auto f2dot14 = [&](std::size_t o) { return r.i16(o) / 16384.0; };
    if (flags & 8) {
      if (p + 2 > gend) return;
      a = d = f2dot14(p);
      p += 2;
    } else if (flags & 0x40) {
      if (p + 4 > gend) return;
      a = f2dot14(p);
      d = f2dot14(p + 2);
      p += 4;
    } else if (flags & 0x80) {
      if (p + 8 > gend) return;
      a = f2dot14(p);
      b = f2dot14(p + 2);
      c = f2dot14(p + 4);
      d = f2dot14(p + 6);
      p += 8;
    }
    const Matrix local{a, b, c, d, dx, dy};
    outline_into(component, Matrix::concat(local, m), out, depth + 1);
    if (!(flags & 0x20)) break;
  }
}

Path TrueType::outline(std::uint16_t gid) const {
  Path path;
  outline_into(gid, Matrix{}, path, 0);
  return path;
}

// --------------------------------------------------------------------------- subsetting

namespace {

void put16(Bytes& b, std::size_t at, std::uint16_t v) {
  b[at] = static_cast<std::byte>(v >> 8);
  b[at + 1] = static_cast<std::byte>(v & 0xFF);
}
void put32(Bytes& b, std::size_t at, std::uint32_t v) {
  put16(b, at, static_cast<std::uint16_t>(v >> 16));
  put16(b, at + 2, static_cast<std::uint16_t>(v & 0xFFFF));
}
void push16(Bytes& b, std::uint16_t v) {
  b.push_back(static_cast<std::byte>(v >> 8));
  b.push_back(static_cast<std::byte>(v & 0xFF));
}
void push32(Bytes& b, std::uint32_t v) {
  push16(b, static_cast<std::uint16_t>(v >> 16));
  push16(b, static_cast<std::uint16_t>(v & 0xFFFF));
}
std::uint32_t checksum(const Bytes& b) {
  std::uint32_t sum = 0;
  for (std::size_t i = 0; i < b.size(); i += 4) {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | (i + k < b.size() ? std::to_integer<std::uint32_t>(b[i + k]) : 0u);
    sum += v;
  }
  return sum;
}

}  // namespace

Result<SubsetResult> subset_truetype(const TrueType& font, const std::vector<std::uint16_t>& glyphs) {
  const ByteView data = font.data();
  const Reader r{data};
  const auto table_bytes = [&](std::string_view tag) -> ByteView {
    // Re-derive table positions from the directory (TrueType keeps them private).
    const std::uint16_t n = r.u16(4);
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t rec = 12 + 16 * i;
      if (!r.has(rec, 16)) break;
      std::string name;
      for (int k = 0; k < 4; ++k) name.push_back(static_cast<char>(r.u8(rec + static_cast<std::size_t>(k))));
      if (name != tag) continue;
      const std::uint32_t off = r.u32(rec + 8), len = r.u32(rec + 12);
      if (!r.has(off, len)) return {};
      return data.subspan(off, len);
    }
    return {};
  };
  const ByteView head = table_bytes("head"), loca = table_bytes("loca"), glyf = table_bytes("glyf");
  const ByteView hhea = table_bytes("hhea"), maxp = table_bytes("maxp");
  if (head.size() < 54 || loca.empty() || glyf.empty() || hhea.size() < 36 || maxp.size() < 6) {
    return fail(Status::Encode, "font cannot be subset (missing tables)");
  }
  const bool long_loca = Reader{head}.i16(50) != 0;
  const std::uint16_t num_glyphs = font.num_glyphs();
  const auto glyph_range = [&](std::uint16_t gid) -> std::pair<std::uint32_t, std::uint32_t> {
    const Reader lr{loca};
    if (long_loca) return {lr.u32(4ull * gid), lr.u32(4ull * gid + 4)};
    return {2u * lr.u16(2ull * gid), 2u * lr.u16(2ull * gid + 2)};
  };
  // Closure over composite components.
  std::set<std::uint16_t> keep{0};
  std::vector<std::uint16_t> todo(glyphs.begin(), glyphs.end());
  while (!todo.empty()) {
    const std::uint16_t gid = todo.back();
    todo.pop_back();
    if (gid >= num_glyphs || keep.contains(gid)) continue;
    keep.insert(gid);
    const auto [s, e] = glyph_range(gid);
    if (e <= s || e > glyf.size() || e - s < 10) continue;
    const Reader gr{glyf.subspan(s, e - s)};
    if (gr.i16(0) >= 0) continue;
    std::size_t p = 10;
    for (int c = 0; c < 64 && gr.has(p, 4); ++c) {
      const std::uint16_t flags = gr.u16(p);
      todo.push_back(gr.u16(p + 2));
      p += 4 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);
      if (!(flags & 0x20)) break;
    }
  }
  SubsetResult result;
  std::uint16_t next = 0;
  for (const std::uint16_t g : keep) result.new_gid[g] = next++;
  const std::uint16_t count = next;
  // glyf + loca
  Bytes new_glyf, new_loca;
  std::vector<std::uint32_t> offsets;
  for (const std::uint16_t g : keep) {
    offsets.push_back(static_cast<std::uint32_t>(new_glyf.size()));
    const auto [s, e] = glyph_range(g);
    if (e > s && e <= glyf.size()) {
      Bytes piece(glyf.begin() + s, glyf.begin() + e);
      if (piece.size() >= 10 && Reader{piece}.i16(0) < 0) {
        std::size_t p = 10;
        for (int c = 0; c < 64 && p + 4 <= piece.size(); ++c) {
          const Reader pr{piece};
          const std::uint16_t flags = pr.u16(p);
          const std::uint16_t comp = pr.u16(p + 2);
          const auto it = result.new_gid.find(comp);
          put16(piece, p + 2, it == result.new_gid.end() ? 0 : it->second);
          p += 4 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);
          if (!(flags & 0x20)) break;
        }
      }
      new_glyf.insert(new_glyf.end(), piece.begin(), piece.end());
      while (new_glyf.size() % 4 != 0) new_glyf.push_back(std::byte{0});
    }
  }
  offsets.push_back(static_cast<std::uint32_t>(new_glyf.size()));
  for (const std::uint32_t o : offsets) push32(new_loca, o);
  // hmtx: full metrics for every kept glyph.
  Bytes new_hmtx;
  const ByteView hmtx = table_bytes("hmtx");
  const std::uint16_t n_hmetrics = Reader{hhea}.u16(34);
  for (const std::uint16_t g : keep) {
    std::uint16_t adv = 0;
    std::int16_t lsb = 0;
    const Reader hr{hmtx};
    if (n_hmetrics > 0) {
      const std::uint16_t idx = g < n_hmetrics ? g : static_cast<std::uint16_t>(n_hmetrics - 1);
      adv = hr.u16(4ull * idx);
      if (g < n_hmetrics) lsb = hr.i16(4ull * g + 2);
      else lsb = hr.i16(4ull * n_hmetrics + 2ull * (g - n_hmetrics));
    }
    push16(new_hmtx, adv);
    push16(new_hmtx, static_cast<std::uint16_t>(lsb));
  }
  Bytes new_head(head.begin(), head.end());
  put32(new_head, 8, 0);
  put16(new_head, 50, 1);
  Bytes new_hhea(hhea.begin(), hhea.end());
  put16(new_hhea, 34, count);
  Bytes new_maxp(maxp.begin(), maxp.end());
  put16(new_maxp, 4, count);
  Bytes new_post;
  push32(new_post, 0x00030000);
  const ByteView post = table_bytes("post");
  for (std::size_t i = 4; i < 32; ++i) new_post.push_back(i < post.size() ? post[i] : std::byte{0});
  // cmap (3,1) format 4 for the kept glyphs with BMP code points.
  std::map<std::uint32_t, std::uint16_t> uni;
  for (const auto& [old_gid, cp] : font.unicode_map()) {
    const auto it = result.new_gid.find(old_gid);
    if (it != result.new_gid.end() && cp <= 0xFFFF && cp != 0) uni[cp] = it->second;
  }
  struct Seg {
    std::uint16_t start, end;
    std::vector<std::uint16_t> gids;
  };
  std::vector<Seg> segs;
  for (const auto& [cp, g] : uni) {
    if (!segs.empty() && segs.back().end + 1u == cp) {
      segs.back().end = static_cast<std::uint16_t>(cp);
      segs.back().gids.push_back(g);
    } else {
      segs.push_back(Seg{static_cast<std::uint16_t>(cp), static_cast<std::uint16_t>(cp), {g}});
    }
  }
  segs.push_back(Seg{0xFFFF, 0xFFFF, {0}});
  Bytes sub;
  const auto seg_count = static_cast<std::uint16_t>(segs.size());
  push16(sub, 4);
  push16(sub, 0);  // length, patched
  push16(sub, 0);
  push16(sub, static_cast<std::uint16_t>(seg_count * 2));
  std::uint16_t search = 1, selector = 0;
  while (search * 2 <= seg_count) {
    search = static_cast<std::uint16_t>(search * 2);
    ++selector;
  }
  push16(sub, static_cast<std::uint16_t>(search * 2));
  push16(sub, selector);
  push16(sub, static_cast<std::uint16_t>(seg_count * 2 - search * 2));
  for (const Seg& s : segs) push16(sub, s.end);
  push16(sub, 0);
  for (const Seg& s : segs) push16(sub, s.start);
  for (std::size_t i = 0; i < segs.size(); ++i) push16(sub, i + 1 == segs.size() ? 1 : 0);  // deltas
  // range offsets point into the glyph id array that follows.
  std::size_t array_index = 0;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    if (i + 1 == segs.size()) {
      push16(sub, 0);
    } else {
      const std::size_t remaining_ranges = segs.size() - i;
      push16(sub, static_cast<std::uint16_t>(2 * (remaining_ranges + array_index)));
      array_index += segs[i].gids.size();
    }
  }
  for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
    for (const std::uint16_t g : segs[i].gids) push16(sub, g);
  }
  put16(sub, 2, static_cast<std::uint16_t>(std::min<std::size_t>(sub.size(), 0xFFFF)));
  Bytes new_cmap;
  push16(new_cmap, 0);
  push16(new_cmap, 1);
  push16(new_cmap, 3);
  push16(new_cmap, 1);
  push32(new_cmap, 12);
  new_cmap.insert(new_cmap.end(), sub.begin(), sub.end());

  std::map<std::string, Bytes> tables;
  tables["head"] = std::move(new_head);
  tables["hhea"] = std::move(new_hhea);
  tables["maxp"] = std::move(new_maxp);
  tables["loca"] = std::move(new_loca);
  tables["glyf"] = std::move(new_glyf);
  tables["hmtx"] = std::move(new_hmtx);
  tables["post"] = std::move(new_post);
  tables["cmap"] = std::move(new_cmap);
  for (const char* tag : {"cvt ", "fpgm", "prep", "OS/2"}) {
    const ByteView t = table_bytes(tag);
    if (!t.empty()) tables[tag] = Bytes(t.begin(), t.end());
  }
  // Assemble.
  Bytes out;
  const auto n = static_cast<std::uint16_t>(tables.size());
  push32(out, 0x00010000);
  push16(out, n);
  std::uint16_t sr = 1, es = 0;
  while (sr * 2 <= n) {
    sr = static_cast<std::uint16_t>(sr * 2);
    ++es;
  }
  push16(out, static_cast<std::uint16_t>(sr * 16));
  push16(out, es);
  push16(out, static_cast<std::uint16_t>(n * 16 - sr * 16));
  std::size_t offset = 12 + 16ull * n;
  std::size_t head_offset = 0;
  for (const auto& [tag, bytes] : tables) {
    for (const char c : tag) out.push_back(static_cast<std::byte>(c));
    push32(out, checksum(bytes));
    push32(out, static_cast<std::uint32_t>(offset));
    push32(out, static_cast<std::uint32_t>(bytes.size()));
    if (tag == "head") head_offset = offset;
    offset += (bytes.size() + 3) / 4 * 4;
  }
  for (const auto& [tag, bytes] : tables) {
    out.insert(out.end(), bytes.begin(), bytes.end());
    while (out.size() % 4 != 0) out.push_back(std::byte{0});
  }
  put32(out, head_offset + 8, 0xB1B0AFBAu - checksum(out));
  result.font = std::move(out);
  return result;
}

}  // namespace cvt::pdf::font
