// Convertible core - pdf family: PDF font dictionaries.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/pdf_font.hpp"

#include <algorithm>
#include <cmath>

#include "families/pdf/embedded_fonts.hpp"
#include "families/pdf/font_data.hpp"
#include "util/text.hpp"

namespace cvt::pdf {

void FontCache::ensure_loaded() {
  if (loaded) return;
  loaded = true;
  if (auto f = font::TrueType::parse(fonts::inter_regular())) sans = std::make_shared<const font::TrueType>(std::move(*f));
  if (auto f = font::TrueType::parse(fonts::inter_semibold())) sans_bold = std::make_shared<const font::TrueType>(std::move(*f));
  if (auto f = font::TrueType::parse(fonts::mono_regular())) mono = std::make_shared<const font::TrueType>(std::move(*f));
}

namespace {

bool name_has(std::string_view name, std::string_view what) {
  return util::to_lower(name).find(what) != std::string::npos;
}

std::optional<font::BaseEncoding> encoding_by_name(std::string_view name) {
  if (name == "WinAnsiEncoding") return font::BaseEncoding::WinAnsi;
  if (name == "MacRomanEncoding") return font::BaseEncoding::MacRoman;
  if (name == "StandardEncoding") return font::BaseEncoding::Standard;
  if (name == "MacExpertEncoding") return font::BaseEncoding::MacExpert;
  if (name == "PDFDocEncoding") return font::BaseEncoding::PdfDoc;
  return std::nullopt;
}

}  // namespace

std::shared_ptr<PdfFont> PdfFont::load(PdfFile& file, const Object& font_obj, FontCache& cache) {
  auto font = std::make_shared<PdfFont>();
  const Object obj = file.resolve(font_obj);
  const Dict& dict = obj.as_dict();
  const std::string subtype = file.get(dict, "Subtype").as_name();
  font->base_font_ = file.get(dict, "BaseFont").as_name();
  {
    std::string_view base = font->base_font_;
    if (base.size() > 7 && base[6] == '+') base.remove_prefix(7);
    font->bold_ = name_has(base, "bold") || name_has(base, "black") || name_has(base, "heavy") ||
                  name_has(base, "semibold") || name_has(base, "demi");
    font->italic_ = name_has(base, "italic") || name_has(base, "oblique");
    font->mono_ = name_has(base, "mono") || name_has(base, "courier") || name_has(base, "consol") ||
                  name_has(base, "typewriter") || name_has(base, "cmtt");
    font->serif_ = name_has(base, "times") || name_has(base, "serif") || name_has(base, "roman") ||
                   name_has(base, "georgia") || name_has(base, "garamond") || name_has(base, "cmr") ||
                   name_has(base, "minion") || name_has(base, "palatino");
    // TeX font names: CMBX (bold extended), CMTI (text italic), CMSL, LMRoman*-Bold...
    if (name_has(base, "cmbx") || name_has(base, "cmb10")) font->bold_ = true;
    if (name_has(base, "cmti") || name_has(base, "cmsl") || name_has(base, "cmmi")) font->italic_ = true;
  }
  if (subtype == "Type0") {
    font->kind_ = Kind::Type0;
    font->load_type0(file, dict, cache);
  } else {
    font->kind_ = subtype == "TrueType" ? Kind::TrueType : (subtype == "Type3" ? Kind::Type3 : Kind::Type1);
    font->load_simple(file, dict, cache);
  }
  const Object tu = file.get(dict, "ToUnicode");
  if (tu.is_stream()) {
    const Bytes program = file.decode_or_empty(tu);
    CMap map = CMap::parse(program);
    if (!map.empty()) font->to_unicode_ = std::move(map);
  } else if (tu.is_name() && (tu.as_name().find("UCS2") != std::string::npos || tu.as_name() == "Identity-H")) {
    // Rare: predefined Unicode CMap as ToUnicode.
  }
  return font;
}

void PdfFont::apply_descriptor(PdfFile& file, const Dict& descriptor) {
  const std::int64_t flags = file.get(descriptor, "Flags").as_int(0);
  symbolic_ = (flags & 4) != 0;
  if (flags & 1) mono_ = true;
  if (flags & 2) serif_ = true;
  if (flags & 64) italic_ = true;
  if (flags & (1 << 18)) bold_ = true;
  const double weight = file.get(descriptor, "FontWeight").as_number(0);
  if (weight >= 600) bold_ = true;
  if (file.get(descriptor, "ItalicAngle").as_number(0) < -4) italic_ = true;
  const double asc = file.get(descriptor, "Ascent").as_number(0), desc = file.get(descriptor, "Descent").as_number(0);
  if (asc > 0 && asc < 3000) ascent_ = asc / 1000;
  if (desc < 0 && desc > -3000) descent_ = desc / 1000;
  missing_width_ = file.get(descriptor, "MissingWidth").as_number(0);
}

void PdfFont::load_program(PdfFile& file, const Dict& descriptor, FontCache& cache, bool cid) {
  const Object ff = file.get(descriptor, "FontFile");
  const Object ff2 = file.get(descriptor, "FontFile2");
  const Object ff3 = file.get(descriptor, "FontFile3");
  if (ff.is_stream()) {
    const Bytes data = file.decode_or_empty(ff);
    const auto length1 = static_cast<std::size_t>(std::max<std::int64_t>(0, file.get(ff.as_dict(), "Length1").as_int(0)));
    if (auto t1 = font::Type1::parse(data, length1)) {
      type1_ = std::make_shared<const font::Type1>(std::move(*t1));
    } else if (auto c = font::Cff::parse(data)) {
      cff_ = std::make_shared<const font::Cff>(std::move(*c));
    }
  } else if (ff2.is_stream()) {
    const Bytes data = file.decode_or_empty(ff2);
    if (auto tt = font::TrueType::parse(data)) {
      truetype_ = std::make_shared<const font::TrueType>(std::move(*tt));
    } else if (auto c = font::Cff::parse(data)) {
      cff_ = std::make_shared<const font::Cff>(std::move(*c));
    }
  } else if (ff3.is_stream()) {
    const Bytes data = file.decode_or_empty(ff3);
    if (auto c = font::Cff::parse(data)) {
      cff_ = std::make_shared<const font::Cff>(std::move(*c));
    } else if (auto tt = font::TrueType::parse(data)) {
      truetype_ = std::make_shared<const font::TrueType>(std::move(*tt));
    }
  }
  embedded_ = truetype_ || cff_ || type1_;
  if (truetype_) program_unicode_ = truetype_->unicode_map();
  if (!embedded_) {
    cache.ensure_loaded();
    substitute_ = mono_ ? cache.mono : (bold_ ? cache.sans_bold : cache.sans);
    if (!substitute_) substitute_ = cache.sans;
  }
  (void)cid;
}

std::string PdfFont::glyph_name(std::uint32_t code) const {
  if (code < 256) return names_[code];
  return {};
}

void PdfFont::load_simple(PdfFile& file, const Dict& dict, FontCache& cache) {
  const Dict& descriptor = file.get(dict, "FontDescriptor").as_dict();
  apply_descriptor(file, descriptor);
  const auto std14 = font::standard14_from_name(base_font_);
  if (std14) {
    const auto info = font::standard14_info(*std14);
    if (descriptor.empty()) {
      bold_ = bold_ || info.bold;
      italic_ = italic_ || info.italic;
      mono_ = mono_ || info.fixed_pitch;
      serif_ = serif_ || info.serif;
      symbolic_ = info.symbolic;
      ascent_ = info.ascent / 1000;
      descent_ = info.descent / 1000;
    }
  }
  if (kind_ == Kind::Type3) {
    const auto& m = file.get(dict, "FontMatrix").as_array();
    if (m.size() == 6) {
      type3_matrix_ = Matrix{file.resolve(m[0]).as_number(), file.resolve(m[1]).as_number(),
                             file.resolve(m[2]).as_number(), file.resolve(m[3]).as_number(),
                             file.resolve(m[4]).as_number(), file.resolve(m[5]).as_number()};
      if (!type3_matrix_.invertible() || !type3_matrix_.finite()) type3_matrix_ = Matrix{0.001, 0, 0, 0.001, 0, 0};
    }
    type3_procs_ = file.get(dict, "CharProcs").as_dict();
    type3_resources_ = file.get(dict, "Resources");
  } else {
    load_program(file, descriptor, cache, false);
  }
  // Base encoding.
  std::array<std::string, 256> builtin;
  bool have_builtin = false;
  if (type1_) {
    builtin = type1_->builtin_encoding();
    have_builtin = true;
  } else if (cff_ && !cff_->is_cid()) {
    builtin = cff_->builtin_encoding();
    have_builtin = true;
  }
  const bool is_symbol_font = std14 && (*std14 == font::Standard14::Symbol || *std14 == font::Standard14::ZapfDingbats);
  const auto fill_from = [&](font::BaseEncoding e) {
    for (int c = 0; c < 256; ++c) {
      names_[static_cast<std::size_t>(c)] = std::string(font::encoding_glyph_name(e, static_cast<std::uint8_t>(c)));
    }
  };
  const auto default_base = [&]() {
    if (have_builtin && (symbolic_ || kind_ == Kind::Type1)) {
      names_ = builtin;
    } else if (is_symbol_font) {
      fill_from(*std14 == font::Standard14::Symbol ? font::BaseEncoding::Symbol : font::BaseEncoding::ZapfDingbats);
    } else if (kind_ == Kind::TrueType) {
      fill_from(font::BaseEncoding::WinAnsi);
    } else {
      fill_from(font::BaseEncoding::Standard);
    }
  };
  const Object encoding = file.get(dict, "Encoding");
  if (encoding.is_name()) {
    if (const auto e = encoding_by_name(encoding.as_name())) {
      fill_from(*e);
      encoding_from_name_ = true;
    } else {
      default_base();
    }
  } else if (encoding.is_dict()) {
    const Dict& ed = encoding.as_dict();
    const Object base = file.get(ed, "BaseEncoding");
    if (const auto e = encoding_by_name(base.as_name())) {
      fill_from(*e);
      encoding_from_name_ = true;
    } else if (!symbolic_ && kind_ != Kind::Type3 && !have_builtin) {
      fill_from(kind_ == Kind::TrueType ? font::BaseEncoding::WinAnsi : font::BaseEncoding::Standard);
    } else {
      default_base();
      if (kind_ == Kind::Type3) {
        for (auto& n : names_) n.clear();
      }
    }
    int code = 0;
    for (const Object& item_ref : file.get(ed, "Differences").as_array()) {
      const Object item = file.resolve(item_ref);
      if (item.is_number()) {
        code = static_cast<int>(item.as_int());
      } else if (item.is_name() && code >= 0 && code < 256) {
        names_[static_cast<std::size_t>(code++)] = item.as_name();
        has_differences_ = true;
      }
    }
  } else {
    if (kind_ == Kind::Type3) {
      for (auto& n : names_) n.clear();
    } else {
      default_base();
    }
  }
  // Widths.
  const std::int64_t first = file.get(dict, "FirstChar").as_int(0);
  const Array& w = file.get(dict, "Widths").as_array();
  for (std::size_t i = 0; i < w.size() && i < 256; ++i) {
    const std::int64_t code = first + static_cast<std::int64_t>(i);
    if (code < 0 || code > 255) continue;
    double value = file.resolve(w[i]).as_number(0);
    if (kind_ == Kind::Type3) {
      // Type 3 widths are in glyph space; convert to 1/1000 text space.
      value = value * type3_matrix_.a * 1000;
    }
    widths_[static_cast<std::size_t>(code)] = value;
    has_width_[static_cast<std::size_t>(code)] = true;
  }
  for (int c = 0; c < 256; ++c) {
    const auto k = static_cast<std::size_t>(c);
    if (has_width_[k]) continue;
    std::optional<double> v;
    if (std14) v = font::standard14_width(*std14, names_[k]);
    if (!v && type1_ && !names_[k].empty()) {
      const double adv = type1_->advance(names_[k]);
      if (adv > 0) v = adv * type1_->font_matrix().a * 1000;
    }
    if (v) {
      widths_[k] = *v;
      has_width_[k] = true;
    } else if (w.empty() && missing_width_ == 0) {
      widths_[k] = std14 ? 0 : 500;
    } else {
      widths_[k] = missing_width_;
    }
  }
}

void PdfFont::load_type0(PdfFile& file, const Dict& dict, FontCache& cache) {
  const Object enc = file.get(dict, "Encoding");
  if (enc.is_name()) {
    cmap_ = CMap::predefined(enc.as_name());
  } else if (enc.is_stream()) {
    cmap_ = CMap::parse(file.decode_or_empty(enc));
    if (cmap_.empty()) cmap_ = CMap::predefined("Identity-H");
  } else {
    cmap_ = CMap::predefined("Identity-H");
  }
  vertical_ = cmap_.vertical();
  const Array& descendants = file.get(dict, "DescendantFonts").as_array();
  const Dict& cid_font = descendants.empty() ? Dict{} : file.resolve(descendants[0]).as_dict();
  const Dict& info = file.get(cid_font, "CIDSystemInfo").as_dict();
  ordering_ = file.get(info, "Ordering").as_string();
  const Dict& descriptor = file.get(cid_font, "FontDescriptor").as_dict();
  apply_descriptor(file, descriptor);
  load_program(file, descriptor, cache, true);
  default_width_ = file.get(cid_font, "DW").as_number(1000);
  const Array& w = file.get(cid_font, "W").as_array();
  for (std::size_t i = 0; i < w.size();) {
    const Object a = file.resolve(w[i]);
    if (i + 1 >= w.size()) break;
    const Object b = file.resolve(w[i + 1]);
    if (b.is_array()) {
      std::uint32_t cid = static_cast<std::uint32_t>(std::max<std::int64_t>(0, a.as_int()));
      for (const Object& v : b.as_array()) {
        if (cid_widths_.size() > 200000) break;
        cid_widths_[cid++] = file.resolve(v).as_number();
      }
      i += 2;
    } else {
      if (i + 2 >= w.size()) break;
      const std::int64_t lo = a.as_int(), hi = b.as_int();
      const double value = file.resolve(w[i + 2]).as_number();
      if (lo >= 0 && hi >= lo && hi - lo < 70000) {
        for (std::int64_t c = lo; c <= hi && cid_widths_.size() < 200000; ++c) cid_widths_[static_cast<std::uint32_t>(c)] = value;
      }
      i += 3;
    }
  }
  if (vertical_) {
    const Array& dw2 = file.get(cid_font, "DW2").as_array();
    if (dw2.size() == 2) default_vmetrics_ = {file.resolve(dw2[0]).as_number(880), file.resolve(dw2[1]).as_number(-1000)};
    const Array& w2 = file.get(cid_font, "W2").as_array();
    for (std::size_t i = 0; i + 1 < w2.size();) {
      const Object a = file.resolve(w2[i]);
      const Object b = file.resolve(w2[i + 1]);
      if (b.is_array()) {
        std::uint32_t cid = static_cast<std::uint32_t>(std::max<std::int64_t>(0, a.as_int()));
        const Array& v = b.as_array();
        for (std::size_t k = 0; k + 2 < v.size() && cid_vmetrics_.size() < 200000; k += 3) {
          cid_vmetrics_[cid++] = {file.resolve(v[k]).as_number(), file.resolve(v[k + 1]).as_number(),
                                  file.resolve(v[k + 2]).as_number()};
        }
        i += 2;
      } else {
        if (i + 4 >= w2.size()) break;
        const std::int64_t lo = a.as_int(), hi = b.as_int();
        const std::array<double, 3> m{file.resolve(w2[i + 2]).as_number(), file.resolve(w2[i + 3]).as_number(),
                                      file.resolve(w2[i + 4]).as_number()};
        if (lo >= 0 && hi >= lo && hi - lo < 70000) {
          for (std::int64_t c = lo; c <= hi && cid_vmetrics_.size() < 200000; ++c) cid_vmetrics_[static_cast<std::uint32_t>(c)] = m;
        }
        i += 5;
      }
    }
  }
  const Object map = file.get(cid_font, "CIDToGIDMap");
  if (map.is_stream()) {
    const Bytes data = file.decode_or_empty(map);
    cid_to_gid_identity_ = false;
    cid_to_gid_.resize(data.size() / 2);
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
      cid_to_gid_[i / 2] = static_cast<std::uint16_t>((std::to_integer<int>(data[i]) << 8) | std::to_integer<int>(data[i + 1]));
    }
  }
}

void PdfFont::decode(std::string_view bytes, std::vector<GlyphCode>& out) const {
  if (kind_ != Kind::Type0) {
    for (const char ch : bytes) {
      GlyphCode g;
      g.code = static_cast<std::uint8_t>(ch);
      g.cid = g.code;
      g.length = 1;
      out.push_back(g);
    }
    return;
  }
  std::size_t pos = 0;
  while (pos < bytes.size()) {
    const CMap::Code c = cmap_.next_code(bytes, pos);
    GlyphCode g;
    g.code = c.value;
    g.length = c.length;
    g.cid = cmap_.cid(c);
    out.push_back(g);
    pos += static_cast<std::size_t>(std::max(1, c.length));
  }
}

double PdfFont::width(const GlyphCode& g) const {
  if (kind_ == Kind::Type0) {
    const auto it = cid_widths_.find(g.cid);
    return (it != cid_widths_.end() ? it->second : default_width_) / 1000.0;
  }
  return g.code < 256 ? widths_[g.code] / 1000.0 : 0.0;
}

double PdfFont::vertical_advance(const GlyphCode& g) const {
  const auto it = cid_vmetrics_.find(g.cid);
  return (it != cid_vmetrics_.end() ? it->second[0] : default_vmetrics_[1]) / 1000.0;
}

Point PdfFont::vertical_origin(const GlyphCode& g) const {
  const auto it = cid_vmetrics_.find(g.cid);
  if (it != cid_vmetrics_.end()) return {it->second[1] / 1000.0, it->second[2] / 1000.0};
  return {width(g) / 2, default_vmetrics_[0] / 1000.0};
}

std::u32string PdfFont::unicode(const GlyphCode& g) const {
  if (to_unicode_) {
    if (auto text = to_unicode_->unicode(CMap::Code{g.code, g.length})) {
      bool useful = !text->empty();
      for (const char32_t c : *text) useful = useful && c != 0 && c != 0xFFFD;
      if (useful) return *text;
    }
  }
  if (kind_ == Kind::Type0) {
    if (cmap_.code_is_unicode()) {
      std::string bytes;
      for (int i = g.length - 1; i >= 0; --i) bytes.push_back(static_cast<char>((g.code >> (8 * i)) & 0xFF));
      return utf16be_to_u32(bytes);
    }
    if (truetype_) {
      std::uint16_t gid = static_cast<std::uint16_t>(g.cid);
      if (!cid_to_gid_identity_) gid = g.cid < cid_to_gid_.size() ? cid_to_gid_[g.cid] : 0;
      if (const auto it = program_unicode_.find(gid); it != program_unicode_.end()) return std::u32string(1, it->second);
    }
    if (cff_ && !cff_->is_cid()) {
      const auto name = cff_->glyph_name(static_cast<std::uint16_t>(g.cid));
      if (auto text = font::glyph_name_to_unicode(name)) return *text;
    }
    return {};
  }
  const std::string name = glyph_name(g.code);
  if (!name.empty()) {
    if (auto text = font::glyph_name_to_unicode(name)) return *text;
  }
  if (truetype_ && kind_ == Kind::TrueType) {
    // Symbolic TrueType: find the glyph, then its Unicode in another subtable.
    std::optional<std::uint16_t> gid;
    if (truetype_->has_cmap(3, 0)) {
      gid = truetype_->glyph_for_cmap(3, 0, g.code);
      if (!gid) gid = truetype_->glyph_for_cmap(3, 0, 0xF000 + g.code);
    }
    if (!gid && truetype_->has_cmap(1, 0)) gid = truetype_->glyph_for_cmap(1, 0, g.code);
    if (gid) {
      if (const auto it = program_unicode_.find(*gid); it != program_unicode_.end()) return std::u32string(1, it->second);
    }
  }
  if (cff_ && !cff_->is_cid()) {
    if (const auto gid = cff_->glyph_for_code(static_cast<std::uint8_t>(g.code))) {
      if (auto text = font::glyph_name_to_unicode(cff_->glyph_name(*gid))) return *text;
    }
  }
  if (!symbolic_ && g.code >= 32 && g.code < 127) return std::u32string(1, static_cast<char32_t>(g.code));
  if (symbolic_ && g.code >= 32 && g.code < 127 && name.empty()) return std::u32string(1, static_cast<char32_t>(g.code));
  return {};
}

Object PdfFont::type3_charproc(const GlyphCode& g) const {
  const std::string name = glyph_name(g.code);
  if (name.empty()) return {};
  return type3_procs_.get(name);
}

const Path* PdfFont::outline(const GlyphCode& g) const {
  if (kind_ == Kind::Type3) return nullptr;
  const std::uint64_t key = (static_cast<std::uint64_t>(g.length) << 40) | ((static_cast<std::uint64_t>(g.cid) << 20) ^ g.code);
  auto it = outline_cache_.find(key);
  if (it == outline_cache_.end()) {
    if (outline_cache_.size() > 20000) outline_cache_.clear();
    it = outline_cache_.emplace(key, std::make_unique<Path>(build_outline(g))).first;
  }
  return it->second->empty() ? nullptr : it->second.get();
}

Path PdfFont::build_outline(const GlyphCode& g) const {
  Path path;
  if (type1_) {
    std::string name = glyph_name(g.code);
    if (name.empty() || !type1_->has_glyph(name)) {
      const auto& builtin = type1_->builtin_encoding();
      if (g.code < 256 && !builtin[g.code].empty()) name = builtin[g.code];
    }
    path = type1_->outline(name);
    path.transform(type1_->font_matrix());
    return path;
  }
  if (cff_) {
    std::optional<std::uint16_t> gid;
    if (kind_ == Kind::Type0) {
      gid = cff_->is_cid() ? cff_->glyph_for_cid(static_cast<std::uint16_t>(std::min<std::uint32_t>(g.cid, 0xFFFF)))
                           : std::optional<std::uint16_t>(static_cast<std::uint16_t>(std::min<std::uint32_t>(g.cid, 0xFFFF)));
    } else {
      const std::string name = glyph_name(g.code);
      if (!name.empty()) gid = cff_->glyph_for_name(name);
      if (!gid) gid = cff_->glyph_for_code(static_cast<std::uint8_t>(g.code & 0xFF));
      if (!gid && cff_->is_cid()) gid = cff_->glyph_for_cid(static_cast<std::uint16_t>(g.code));
    }
    if (!gid) return path;
    path = cff_->outline(*gid);
    path.transform(cff_->font_matrix(*gid));
    return path;
  }
  if (truetype_) {
    std::optional<std::uint16_t> gid;
    if (kind_ == Kind::Type0) {
      std::uint32_t v = g.cid;
      if (!cid_to_gid_identity_) v = g.cid < cid_to_gid_.size() ? cid_to_gid_[g.cid] : 0;
      gid = static_cast<std::uint16_t>(std::min<std::uint32_t>(v, 0xFFFF));
    } else {
      const std::uint32_t code = g.code;
      const std::string name = glyph_name(code);
      const bool has30 = truetype_->has_cmap(3, 0), has31 = truetype_->has_cmap(3, 1);
      if (has31 && !(symbolic_ && has30 && !has_differences_ && !encoding_from_name_)) {
        if (auto cp = font::glyph_name_to_codepoint(name)) gid = truetype_->glyph_for_unicode(*cp);
        if (!gid && !name.empty()) gid = truetype_->glyph_for_name(name);
      }
      if (!gid && has30) {
        gid = truetype_->glyph_for_cmap(3, 0, code);
        if (!gid) gid = truetype_->glyph_for_cmap(3, 0, 0xF000 + code);
        if (!gid) gid = truetype_->glyph_for_cmap(3, 0, 0xF100 + code);
        if (!gid) gid = truetype_->glyph_for_cmap(3, 0, 0xF200 + code);
      }
      if (!gid && truetype_->has_cmap(1, 0)) gid = truetype_->glyph_for_cmap(1, 0, code);
      if (!gid && !name.empty()) gid = truetype_->glyph_for_name(name);
      if (!gid && has31) {
        if (auto cp = font::glyph_name_to_codepoint(name)) gid = truetype_->glyph_for_unicode(*cp);
      }
      if (!gid && code < truetype_->num_glyphs() && !has31) gid = static_cast<std::uint16_t>(code);
    }
    if (!gid) return path;
    path = truetype_->outline(*gid);
    path.transform(truetype_->font_matrix());
    return path;
  }
  if (substitute_) {
    const std::u32string text = unicode(g);
    if (text.empty()) return path;
    char32_t cp = text[0];
    if (cp == 0xA0) cp = ' ';
    const auto gid = substitute_->glyph_for_unicode(cp);
    if (!gid) return path;
    path = substitute_->outline(*gid);
    Matrix m = substitute_->font_matrix();
    const double sub_adv = substitute_->advance(*gid) * m.a;
    const double want = width(g);
    if (sub_adv > 0 && want > 0 && text.size() == 1) {
      const double sx = std::clamp(want / sub_adv, 0.5, 1.6);
      m = Matrix::concat(m, Matrix::scale(sx, 1));
    }
    if (italic_) m = Matrix::concat(m, Matrix{1, 0, 0.2, 1, 0, 0});
    path.transform(m);
  }
  return path;
}

}  // namespace cvt::pdf
