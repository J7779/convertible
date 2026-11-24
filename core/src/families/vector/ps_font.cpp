// Convertible core - vector family: PostScript fonts and text.
//
// Embedded Type 1 (eexec, charstrings), Type 3 (BuildChar / BuildGlyph),
// Type 42 (sfnts -> TrueTypeFont) and Type 0 composite fonts (8/8 and
// Identity CMaps over CIDFontType 2); fonts that are not embedded are drawn
// with the bundled fallback faces. Every `show` produces a Text node whose
// outline is the glyph shapes and whose content is the decoded string.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/ps_internal.hpp"
#include "util/text.hpp"

namespace cvt::vector::ps {

namespace {

constexpr double slant_tan = 0.2126;  // tan(12 degrees)

struct FontStyle {
  std::string family = "sans-serif";
  int weight = 400;
  bool italic = false;
  std::string base;
};

FontStyle style_from_name(std::string_view name) {
  FontStyle st;
  std::string n(name);
  if (n.size() > 7 && n[6] == '+') n.erase(0, 7);  // subset prefix ABCDEF+
  while (!n.empty() && n[0] == '_') n.erase(0, 1);
  std::string base = n, style;
  const std::size_t dash = n.find('-');
  if (dash != std::string::npos) {
    base = n.substr(0, dash);
    style = n.substr(dash + 1);
  }
  const std::string lower = util::to_lower(n);
  if (lower.find("bold") != std::string::npos || lower.find("black") != std::string::npos ||
      lower.find("heavy") != std::string::npos) {
    st.weight = 700;
  } else if (lower.find("semibold") != std::string::npos || lower.find("demi") != std::string::npos) {
    st.weight = 600;
  } else if (lower.find("light") != std::string::npos) {
    st.weight = 300;
  }
  st.italic = lower.find("italic") != std::string::npos || lower.find("oblique") != std::string::npos;
  const std::string lb = util::to_lower(base);
  std::string generic;
  if (lb.find("times") != std::string::npos || lb.find("serif") != std::string::npos ||
      lb.find("georgia") != std::string::npos || lb.find("garamond") != std::string::npos ||
      lb.find("palatino") != std::string::npos || lb.find("bookman") != std::string::npos ||
      lb.find("century") != std::string::npos || lb.find("roman") != std::string::npos) {
    generic = "serif";
  }
  if (lb.find("sans") != std::string::npos || lb.find("helvetica") != std::string::npos ||
      lb.find("arial") != std::string::npos || lb.find("verdana") != std::string::npos) {
    generic = "sans-serif";
  }
  if (lb.find("courier") != std::string::npos || lb.find("mono") != std::string::npos ||
      lb.find("consolas") != std::string::npos) {
    generic = "monospace";
  }
  st.base = base;
  if (base.empty()) st.family = generic.empty() ? "sans-serif" : generic;
  else st.family = generic.empty() ? base : base + ", " + generic;
  return st;
}

}  // namespace

// --------------------------------------------------------------------------- encodings

void Interpreter::init_encodings() {
  global_alloc_ = true;
  auto build = [&](const std::array<std::string_view, 256>& names) {
    Object arr = make_array(256);
    for (std::size_t i = 0; i < 256; ++i) arrays_[arr.ref].items[i] = make_name(names[i]);
    arr.readonly = true;
    return arr;
  };
  standard_encoding_ = build(standard_encoding());
  isolatin1_encoding_ = build(isolatin1_encoding());
  symbol_encoding_ = standard_encoding_;
  dict_put(systemdict_, "StandardEncoding", standard_encoding_);
  dict_put(systemdict_, "ISOLatin1Encoding", isolatin1_encoding_);
  // Glyph names the fallback fonts answer to (programs test `CharStrings known`).
  fallback_charstrings_ = make_dict(320);
  for (const auto* table : {&standard_encoding(), &isolatin1_encoding()}) {
    for (const std::string_view n : *table) {
      if (n != ".notdef") dict_put(fallback_charstrings_, n, Object::integer(0));
    }
  }
  dict_put(fallback_charstrings_, ".notdef", Object::integer(0));
  global_alloc_ = false;
}

// --------------------------------------------------------------------------- font dictionaries

Object Interpreter::find_font(const Object& name_in) {
  Object name = name_in;
  if (name.type == Type::String) name = make_name(string_view(name_in));
  if (const Object* f = dict_find(fontdir_, name); f != nullptr && f->type == Type::Dict) return *f;
  if (const Object* resources = dict_find(systemdict_, ".resources")) {
    if (const Object* fonts = dict_find(*resources, "Font")) {
      if (const Object* f = dict_find(*fonts, name); f != nullptr && f->type == Type::Dict) return *f;
    }
  }
  if (name.type != Type::Name) error("invalidfont");
  // Not embedded: a stand-in drawn with the bundled faces.
  const std::string text(name_text(name));
  if (fonts_.size() > 10'000) error("limitcheck", "too many fonts");
  const bool keep_global = global_alloc_;
  global_alloc_ = true;
  Object d = make_dict(16);
  dict_put(d, "FontType", Object::integer(1));
  dict_put(d, "FontName", name);
  Object fm = make_array(6);
  store_matrix(fm, Matrix::scale(0.001, 0.001));
  dict_put(d, "FontMatrix", fm);
  Object fm0 = make_array(6);
  store_matrix(fm0, Matrix::scale(0.001, 0.001));
  dict_put(d, ".OrigFontMatrix", fm0);
  Object bbox = make_array(4);
  array_put(bbox, 0, Object::integer(-200));
  array_put(bbox, 1, Object::integer(-250));
  array_put(bbox, 2, Object::integer(1200));
  array_put(bbox, 3, Object::integer(950));
  dict_put(d, "FontBBox", bbox);
  dict_put(d, "Encoding", standard_encoding_);
  dict_put(d, "PaintType", Object::integer(0));
  dict_put(d, "CharStrings", fallback_charstrings_);
  dict_put(d, ".Fallback", Object::boolean(true));
  Object fid;
  fid.type = Type::FontID;
  fid.ref = ++font_id_counter_;
  dict_put(d, "FID", fid);
  global_alloc_ = keep_global;
  dict_put(fontdir_, name, d);
  warn_once(std::format("font {} is not embedded; it is drawn with a bundled substitute", text));
  return d;
}

LoadedFont* Interpreter::font_for(const Object& font_dict) {
  std::uint32_t key = font_dict.ref | 0x80000000u;
  if (const Object* fid = dict_find(font_dict, "FID"); fid != nullptr && fid->type == Type::FontID) key = fid->ref;
  if (auto it = fonts_.find(key); it != fonts_.end()) return it->second.get();
  auto lf = std::make_unique<LoadedFont>();
  const Object* ft = dict_find(font_dict, "FontType");
  lf->type = ft != nullptr && ft->is_number() ? static_cast<int>(ft->number()) : 1;
  const Object* fb = dict_find(font_dict, ".Fallback");
  if (fb != nullptr && fb->type == Type::Boolean && fb->ival != 0) lf->fallback = true;
  if (lf->type == 1 && !lf->fallback) {
    const Object* cs = dict_find(font_dict, "CharStrings");
    if (cs == nullptr || cs->type != Type::Dict) lf->fallback = true;
  }
  if (lf->type == 2 || lf->type == 9 || lf->type == 10 || lf->type == 11 || lf->type == 32) {
    lf->fallback = true;
    warn_once(std::format("font type {} is not supported; text is drawn with a bundled substitute", lf->type));
  }
  if (lf->type == 42 || lf->type == 11) {
    // Concatenate the sfnts strings (a trailing pad byte of odd-length strings is dropped).
    const Object* sfnts = dict_find(font_dict, "sfnts");
    auto data = std::make_shared<std::string>();
    if (sfnts != nullptr && sfnts->type == Type::Array) {
      for (std::uint32_t i = 0; i < sfnts->size; ++i) {
        const Object s = array_get(*sfnts, i);
        if (s.type != Type::String) continue;
        std::string_view piece = string_view(s);
        if (piece.size() % 2 == 1) piece.remove_suffix(1);
        data->append(piece);
        if (data->size() > 64u * 1024u * 1024u) break;
      }
    }
    lf->sfnt = data;
    auto parsed = TrueTypeFont::parse(as_bytes(*lf->sfnt));
    if (parsed) {
      lf->ttf = std::make_shared<TrueTypeFont>(std::move(*parsed));
      lf->type = 42;
      lf->fallback = false;
    } else {
      lf->fallback = true;
      warn_once("an embedded TrueType (Type 42) font could not be read; text is drawn with a bundled substitute");
    }
  }
  if (lf->type == 1 && !lf->fallback) {
    if (const Object* priv = dict_find(font_dict, "Private"); priv != nullptr && priv->type == Type::Dict) {
      if (const Object* liv = dict_find(*priv, "lenIV"); liv != nullptr && liv->is_number()) {
        lf->len_iv = static_cast<int>(liv->number());
      }
      if (const Object* subrs = dict_find(*priv, "Subrs"); subrs != nullptr && subrs->type == Type::Array) {
        for (std::uint32_t i = 0; i < subrs->size && i < 65536; ++i) {
          const Object s = array_get(*subrs, i);
          if (s.type != Type::String) {
            lf->subrs.emplace_back();
            continue;
          }
          const std::string_view raw = string_view(s);
          lf->subrs.push_back(lf->len_iv < 0 ? std::string(raw)
                                             : decrypt(raw, 4330, static_cast<std::size_t>(lf->len_iv)));
        }
      }
    }
  }
  if (lf->fallback) {
    const Object* fname = dict_find(font_dict, "FontName");
    const FontStyle st = style_from_name(fname != nullptr ? object_string(*fname) : std::string("Helvetica"));
    auto face = fallback_font(choose_fallback(st.family, st.weight));
    if (face) lf->ttf = std::make_shared<TrueTypeFont>(std::move(*face));
    lf->slant = st.italic;
  }
  LoadedFont* raw = lf.get();
  fonts_.emplace(key, std::move(lf));
  return raw;
}

namespace {
std::string glyph_name_of(Interpreter& in, const Object& font_dict, std::uint32_t code) {
  const Object* enc = in.dict_find(font_dict, "Encoding");
  if (enc == nullptr || enc->type != Type::Array || code >= enc->size) return {};
  const Object n = in.array_get(*enc, code);
  if (n.type == Type::Name || n.type == Type::String) return in.object_string(n);
  return {};
}
}  // namespace

GlyphOutline Interpreter::glyph(LoadedFont& font, const Object& font_dict, std::uint32_t code, std::string* name_out) {
  std::string name = glyph_name_of(*this, font_dict, code);
  if (name_out != nullptr) *name_out = name;
  if (auto it = font.cache.find(name); it != font.cache.end()) return it->second;
  GlyphOutline g;
  if (font.fallback) {
    if (font.ttf) {
      char32_t cp = glyph_unicode(name);
      if (cp == 0 && name.empty()) cp = code;
      if (cp == 0xA0) cp = 0x20;
      const std::uint16_t gid = cp != 0 ? font.ttf->glyph_index(cp) : 0;
      const double k = 1000.0 / std::max<double>(1, font.ttf->units_per_em());
      Matrix m = Matrix::scale(k, k);
      if (font.slant) m = Matrix{1, 0, slant_tan, 1, 0, 0} * m;
      if (cp != 0) {
        g.path = transformed(font.ttf->outline(gid), m);
        g.advance = {font.ttf->advance(gid) * k, 0};
        g.ok = true;
      }
    }
  } else if (font.type == 42 && font.ttf) {
    std::uint16_t gid = 0;
    if (const Object* cs = dict_find(font_dict, "CharStrings"); cs != nullptr && cs->type == Type::Dict) {
      if (const Object* v = dict_find(*cs, make_name(name)); v != nullptr && v->is_number()) {
        gid = static_cast<std::uint16_t>(std::clamp<double>(v->number(), 0, 65535));
      }
    }
    if (gid == 0 && name.empty()) gid = font.ttf->glyph_index(code);
    const double k = 1.0 / std::max<double>(1, font.ttf->units_per_em());
    if (gid < font.ttf->glyph_count()) {
      g.path = transformed(font.ttf->outline(gid), Matrix::scale(k, k));
      g.advance = {font.ttf->advance(gid) * k, 0};
      g.ok = true;
    }
  } else if (font.type == 1) {
    const Object* cs = dict_find(font_dict, "CharStrings");
    auto run_named = [&](const std::string& gname) -> Type1Glyph {
      if (cs == nullptr || cs->type != Type::Dict) return {};
      const Object* v = dict_find(*cs, make_name(gname));
      if (v == nullptr || v->type != Type::String) return {};
      const std::string_view raw = string_view(*v);
      const std::string plain =
          font.len_iv < 0 ? std::string(raw) : decrypt(raw, 4330, static_cast<std::size_t>(font.len_iv));
      return run_type1_charstring(plain, font.subrs);
    };
    Type1Glyph t = run_named(name.empty() ? std::string(".notdef") : name);
    if (t.seac_base >= 0 && t.seac_accent >= 0 && t.seac_base < 256 && t.seac_accent < 256) {
      const Type1Glyph base = run_named(std::string(standard_encoding()[static_cast<std::size_t>(t.seac_base)]));
      const Type1Glyph accent = run_named(std::string(standard_encoding()[static_cast<std::size_t>(t.seac_accent)]));
      Path p = base.path;
      append(p, accent.path, Matrix::translate(t.seac_offset.x, t.seac_offset.y));
      t.path = std::move(p);
      t.ok = base.ok;
    }
    g.path = std::move(t.path);
    g.advance = t.advance;
    g.ok = t.ok;
  }
  if (font.cache.size() < 4096) font.cache.emplace(name, g);
  return g;
}

// --------------------------------------------------------------------------- text runs

void Interpreter::finish_text_run(const Object& font_dict, const std::string& content, const Path& node_outline,
                                  const Matrix& node_to_device, double size, double advance, bool invisible) {
  if (content.empty() && node_outline.empty()) return;
  Text t;
  t.content = content;
  const Object* fname = dict_find(font_dict, "FontName");
  const FontStyle st = style_from_name(fname != nullptr ? object_string(*fname) : std::string());
  t.font.family = st.family;
  t.font.weight = st.weight;
  t.font.italic = st.italic;
  t.font.size = size > 0 && std::isfinite(size) ? size : 12.0;
  t.origin = {0, 0};
  t.advance = advance;
  t.outline = node_outline;
  t.fill = invisible ? Paint::none() : current_paint();
  if (!invisible && !t.fill.visible()) return;
  Group g;
  g.transform = node_to_device;
  g.children.push_back(Node{std::move(t)});
  emit_node(Node{std::move(g)});
}

void Interpreter::show_string(const Object& str, int mode, double ax, double ay, double cx, double cy, int wchar,
                              const Object& proc) {
  // mode: 0 show, 1 ashow, 2 widthshow, 3 awidthshow, 4 kshow, 5 xyshow (proc = numarray, wchar = axes),
  //       6 cshow, 7 stringwidth, 8 charpath, 9 glyphshow (str is a name)
  const Object font_dict = gstate_.font;
  if (font_dict.type != Type::Dict) error("invalidfont", "no current font");
  const bool measuring = mode == 7;
  if (!measuring && mode != 6 && !gstate_.has_point) error("nocurrentpoint");
  if (!gstate_.ctm.invertible()) error("undefinedresult");

  const Object* ftype = dict_find(font_dict, "FontType");
  const int type = ftype != nullptr && ftype->is_number() ? static_cast<int>(ftype->number()) : 1;

  // Composite fonts: decode the string into (descendant font, code) pairs.
  struct Piece {
    Object font;
    std::uint32_t code = 0;
    std::uint32_t char_code = 0;  // the code as it appears in the string (for kshow/cshow/widthshow)
  };
  std::vector<Piece> pieces;
  const std::string bytes = str.type == Type::Name ? std::string() : std::string(string_view(str));
  if (type == 0) {
    const Object* fmap = dict_find(font_dict, "FMapType");
    const Object* deps = dict_find(font_dict, "FDepVector");
    const int fmap_type = fmap != nullptr && fmap->is_number() ? static_cast<int>(fmap->number()) : 2;
    if (deps == nullptr || deps->type != Type::Array || deps->size == 0) error("invalidfont");
    if (fmap_type == 2) {
      for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const auto f = static_cast<unsigned char>(bytes[i]);
        const auto c = static_cast<unsigned char>(bytes[i + 1]);
        const Object* enc = dict_find(font_dict, "Encoding");
        std::uint32_t fi = f;
        if (enc != nullptr && enc->type == Type::Array && f < enc->size) {
          const Object e = array_get(*enc, f);
          if (e.is_number()) fi = static_cast<std::uint32_t>(e.number());
        }
        if (fi >= deps->size) fi = 0;
        pieces.push_back({array_get(*deps, fi), c, static_cast<std::uint32_t>(f) * 256u + c});
      }
    } else {
      // FMapType 9 (CMap): Identity-H/V two-byte codes = CIDs into FDepVector[0].
      for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const std::uint32_t cid = static_cast<unsigned char>(bytes[i]) * 256u + static_cast<unsigned char>(bytes[i + 1]);
        pieces.push_back({array_get(*deps, 0), cid, cid});
      }
    }
  } else if (str.type == Type::Name) {
    pieces.push_back({font_dict, 0xFFFFFFFFu, 0});
  } else {
    for (const char ch : bytes) {
      const auto c = static_cast<unsigned char>(ch);
      pieces.push_back({font_dict, c, c});
    }
  }

  const Matrix ctm = gstate_.ctm;
  const Matrix inv_ctm = ctm.inverted();
  const Point start_user = measuring || mode == 6 ? Point{0, 0} : inv_ctm.apply(gstate_.point);
  Point pen = start_user;

  // Text node space of the run (see ps_internal.hpp): node -> device.
  auto font_matrix = [&](const Object& fd) {
    const Object* fm = dict_find(fd, "FontMatrix");
    return fm != nullptr && fm->type == Type::Array ? to_matrix(*fm) : Matrix::scale(0.001, 0.001);
  };
  const Matrix FM = font_matrix(type == 0 && !pieces.empty() ? pieces[0].font : font_dict);
  Matrix FMroot = font_matrix(font_dict);
  Matrix FM0 = FM;
  if (const Object* o = dict_find(type == 0 && !pieces.empty() ? pieces[0].font : font_dict, ".OrigFontMatrix");
      o != nullptr && o->type == Type::Array) {
    FM0 = to_matrix(*o);
  }
  const Matrix FMtotal = type == 0 ? FMroot * FM : FM;
  double size = 1.0;
  if (FM0.invertible()) size = std::sqrt(std::abs(FMtotal.determinant()) / std::abs(FM0.determinant()));
  if (!(size > 0) || !std::isfinite(size)) size = 1.0;
  const Matrix S = Matrix::scale(size, size) * FM0;
  const Matrix flip = Matrix::scale(1, -1);
  Matrix N = FMtotal * S.inverted();
  N.e = 0;
  N.f = 0;
  const Matrix node_to_user_lin = N * flip;
  const Matrix user_to_node_lin = node_to_user_lin.inverted();
  const Matrix node_to_device = ctm * Matrix::translate(start_user.x, start_user.y) * node_to_user_lin;

  Path outline;     // node space
  std::string content;
  bool invisible = false;
  std::size_t index = 0;
  std::uint32_t prev_code = 0;
  std::vector<double> displacements;
  if (mode == 5 && proc.type == Type::Array) {
    for (std::uint32_t i = 0; i < proc.size && i < 1'000'000; ++i) {
      const Object e = array_get(proc, i);
      if (e.is_number()) displacements.push_back(e.number());
    }
  }
  std::size_t disp_at = 0;

  for (const Piece& piece : pieces) {
    tick();
    if (mode == 4 && index > 0) {
      // kshow: the procedure runs between characters with the pen at the current position.
      gstate_.point = ctm.apply(pen);
      gstate_.has_point = true;
      push_int(prev_code);
      push_int(piece.char_code);
      call(proc);
      if (!gstate_.has_point) error("nocurrentpoint");
      pen = gstate_.ctm.inverted().apply(gstate_.point);
    }
    const Object fd = piece.font;
    const Object* sub_type = dict_find(fd, "FontType");
    const int st = sub_type != nullptr && sub_type->is_number() ? static_cast<int>(sub_type->number()) : 1;
    const Matrix fm = type == 0 ? FMroot * font_matrix(fd) : font_matrix(fd);
    Point w{0, 0};  // character space advance
    std::string gname;

    if (st == 3) {
      // Type 3: run BuildGlyph / BuildChar.
      invisible = true;
      std::uint32_t code = piece.code;
      Object name_obj;
      if (str.type == Type::Name) {
        name_obj = str;
        gname = std::string(name_text(str));
      } else {
        gname = glyph_name_of(*this, fd, code);
        name_obj = make_name(gname.empty() ? ".notdef" : gname);
      }
      const Object* bg = dict_find(fd, "BuildGlyph");
      const Object* bc = dict_find(fd, "BuildChar");
      op_gsave();
      gstate_.ctm = ctm * Matrix::translate(pen.x, pen.y) * fm;
      gstate_.path.clear();
      gstate_.has_point = false;
      const bool was_in = in_build_char_;
      in_build_char_ = true;
      build_char_width_ = {0, 0};
      const bool draw = mode != 7 && mode != 6 && mode != 8;
      if (!draw) begin_capture();
      try {
        push(fd);
        if (bg != nullptr) {
          push(name_obj);
          call(*bg);
        } else if (bc != nullptr) {
          push_int(code);
          call(*bc);
        } else {
          pop();
        }
      } catch (...) {
        if (!draw) end_capture();
        in_build_char_ = was_in;
        op_grestore();
        throw;
      }
      if (!draw) end_capture();
      in_build_char_ = was_in;
      op_grestore();
      w = build_char_width_;
    } else {
      LoadedFont* lf = font_for(fd);
      GlyphOutline g;
      if (str.type == Type::Name) {
        // glyphshow: look the name up directly.
        gname = std::string(name_text(str));
        // Temporarily resolve via a one-entry encoding.
        Object tmp_enc = make_array(1);
        array_put(tmp_enc, 0, str);
        Object tmp = make_dict(dict(fd).entries.size() + 1);
        for (const auto& [k, v] : dict(fd).entries) dict_put(tmp, k, v);
        dict_put(tmp, "Encoding", tmp_enc);
        g = glyph(*lf, tmp, 0, nullptr);
      } else if (type == 0 && st == 42 && lf->ttf) {
        // CIDFontType 2: CID -> GID through CIDMap (identity when absent).
        std::uint32_t gid = piece.code;
        if (const Object* map = dict_find(fd, "CIDMap"); map != nullptr && map->type == Type::String) {
          const std::string_view m = string_view(*map);
          if (static_cast<std::size_t>(piece.code) * 2 + 1 < m.size()) {
            gid = static_cast<unsigned char>(m[piece.code * 2]) * 256u + static_cast<unsigned char>(m[piece.code * 2 + 1]);
          }
        }
        const double k = 1.0 / std::max<double>(1, lf->ttf->units_per_em());
        if (gid < lf->ttf->glyph_count()) {
          g.path = transformed(lf->ttf->outline(static_cast<std::uint16_t>(gid)), Matrix::scale(k, k));
          g.advance = {lf->ttf->advance(static_cast<std::uint16_t>(gid)) * k, 0};
          g.ok = true;
        }
      } else {
        g = glyph(*lf, fd, piece.code, &gname);
      }
      w = g.advance;
      if (mode == 8) {
        // charpath: append the outline to the current path (device space).
        const Matrix to_dev = ctm * Matrix::translate(pen.x, pen.y) * fm;
        append(gstate_.path, g.path, to_dev);
      } else if (mode != 7 && mode != 6) {
        // Outline in node space: char -> user (relative to the start) -> node.
        const Point offset = user_to_node_lin.apply_vector(pen - start_user);
        const Matrix char_to_node = Matrix::translate(offset.x, offset.y) * user_to_node_lin * fm;
        append(outline, g.path, char_to_node);
        if (outline.verbs.size() > 2'000'000) error("limitcheck", "text too long");
      }
    }
    // Content text.
    if (mode != 7 && mode != 8) {
      char32_t cp = glyph_unicode(gname);
      if (cp == 0 && type == 0) cp = 0;
      if (cp == 0 && type != 0 && piece.code < 127 && piece.code >= 32) cp = piece.code;
      if (cp != 0) util::append_utf8(content, cp);
    }
    // Advance.
    Point d = fm.apply_vector(w);
    if (mode == 1 || mode == 3) d = d + Point{ax, ay};
    if ((mode == 2 || mode == 3) && static_cast<int>(piece.char_code) == wchar) d = d + Point{cx, cy};
    if (mode == 5) {
      double dx = 0, dy = 0;
      if (wchar == 0) {  // xyshow
        dx = disp_at < displacements.size() ? displacements[disp_at] : 0;
        dy = disp_at + 1 < displacements.size() ? displacements[disp_at + 1] : 0;
        disp_at += 2;
      } else if (wchar == 1) {  // xshow
        dx = disp_at < displacements.size() ? displacements[disp_at] : 0;
        ++disp_at;
      } else {  // yshow
        dy = disp_at < displacements.size() ? displacements[disp_at] : 0;
        ++disp_at;
      }
      d = {dx, dy};
    }
    if (mode == 6) {
      // cshow: hand the character and its width to the procedure.
      const Point uw = fm.apply_vector(w);
      push_int(piece.char_code);
      push_real(uw.x);
      push_real(uw.y);
      call(proc);
    } else {
      pen = pen + d;
    }
    prev_code = piece.char_code;
    ++index;
  }

  if (measuring) {
    text_width_ = pen - start_user;
    return;
  }
  if (mode == 6) return;
  // Leave the current point after the text.
  path_moveto(ctm.apply(pen));
  if (mode == 8) return;
  const Point total_node = user_to_node_lin.apply_vector(pen - start_user);
  finish_text_run(font_dict, content, outline, node_to_device, size, total_node.x, invisible);
  if (invisible && !content.empty()) {
    // Type 3 glyphs were drawn as shapes; the run keeps its text invisibly.
  }
}

// --------------------------------------------------------------------------- operators

void Interpreter::op_findfont() { push(find_font(pop())); }

void Interpreter::op_definefont() {
  need(2);
  const Object font = pop_dict();
  const Object key = pop();
  if (!dict_find(font, "FontMatrix")) {
    Object fm = make_array(6);
    store_matrix(fm, Matrix::scale(0.001, 0.001));
    dict_put(font, "FontMatrix", fm);
  }
  if (!dict_find(font, ".OrigFontMatrix")) {
    const Object* fm = dict_find(font, "FontMatrix");
    Object copy = make_array(6);
    store_matrix(copy, fm != nullptr && fm->type == Type::Array && fm->size == 6 ? to_matrix(*fm) : Matrix{});
    dict_put(font, ".OrigFontMatrix", copy);
  }
  if (!dict_find(font, "FID")) {
    Object fid;
    fid.type = Type::FontID;
    fid.ref = ++font_id_counter_;
    dict_put(font, "FID", fid);
  }
  if (!dict_find(font, "Encoding")) {
    const Object* ft = dict_find(font, "FontType");
    if (ft == nullptr || !ft->is_number() || ft->number() != 0) dict_put(font, "Encoding", standard_encoding_);
  }
  dict_put(fontdir_, key, font);
  push(font);
}
void Interpreter::op_undefinefont() { dict_remove(fontdir_, pop()); }

namespace {
Object derive_font(Interpreter& in, const Object& font, const Matrix& m) {
  const Object* fm = in.dict_find(font, "FontMatrix");
  const Matrix base = fm != nullptr && fm->type == Type::Array && fm->size == 6 ? in.to_matrix(*fm) : Matrix{};
  Object copy = in.make_dict(in.dict(font).entries.size() + 2);
  const auto entries = in.dict(font).entries;
  for (const auto& [k, v] : entries) in.dict_put(copy, k, v);
  Object arr = in.make_array(6);
  in.store_matrix(arr, m * base);
  in.dict_put(copy, "FontMatrix", arr);
  if (!in.dict_find(copy, ".OrigFontMatrix")) {
    Object orig = in.make_array(6);
    in.store_matrix(orig, base);
    in.dict_put(copy, ".OrigFontMatrix", orig);
  }
  return copy;
}
}  // namespace

void Interpreter::op_scalefont() {
  const double s = pop_number();
  const Object font = pop_dict();
  push(derive_font(*this, font, Matrix::scale(s, s)));
}
void Interpreter::op_makefont() {
  const Matrix m = pop_matrix();
  const Object font = pop_dict();
  push(derive_font(*this, font, m));
}
void Interpreter::op_setfont() { gstate_.font = pop_dict(); }
void Interpreter::op_currentfont() {
  if (gstate_.font.type == Type::Dict) {
    push(gstate_.font);
  } else {
    push(find_font(make_name("Helvetica")));
  }
}
void Interpreter::op_rootfont() { op_currentfont(); }
void Interpreter::op_selectfont() {
  need(2);
  const Object scale = pop();
  const Object key = pop();
  const Object font = find_font(key);
  if (scale.type == Type::Array) gstate_.font = derive_font(*this, font, to_matrix(scale));
  else if (scale.is_number()) gstate_.font = derive_font(*this, font, Matrix::scale(scale.number(), scale.number()));
  else error("typecheck");
}
void Interpreter::op_show() {
  const Object s = pop_string();
  show_string(s, 0, 0, 0, 0, 0, -1, Object::null());
}
void Interpreter::op_ashow() {
  const Object s = pop_string();
  const double ay = pop_number(), ax = pop_number();
  show_string(s, 1, ax, ay, 0, 0, -1, Object::null());
}
void Interpreter::op_widthshow() {
  const Object s = pop_string();
  const std::int64_t c = pop_int();
  const double cy = pop_number(), cx = pop_number();
  show_string(s, 2, 0, 0, cx, cy, static_cast<int>(c), Object::null());
}
void Interpreter::op_awidthshow() {
  const Object s = pop_string();
  const double ay = pop_number(), ax = pop_number();
  const std::int64_t c = pop_int();
  const double cy = pop_number(), cx = pop_number();
  show_string(s, 3, ax, ay, cx, cy, static_cast<int>(c), Object::null());
}
void Interpreter::op_kshow() {
  const Object s = pop_string();
  const Object proc = pop_proc();
  show_string(s, 4, 0, 0, 0, 0, -1, proc);
}
namespace {
Object numarray(Interpreter& in, const Object& o) {
  if (o.type == Type::Array) return o;
  if (o.type == Type::String) {
    // Encoded number string (binary token 149): decode the common forms.
    const std::string_view s = in.string_view(o);
    std::vector<double> values;
    if (s.size() >= 4 && static_cast<unsigned char>(s[0]) == 149) {
      const int repr = static_cast<unsigned char>(s[1]);
      const bool big = repr < 128;
      const int r = repr % 128;
      const std::size_t count = big ? (static_cast<unsigned char>(s[2]) * 256u + static_cast<unsigned char>(s[3]))
                                     : (static_cast<unsigned char>(s[3]) * 256u + static_cast<unsigned char>(s[2]));
      std::size_t pos = 4;
      for (std::size_t i = 0; i < count; ++i) {
        if (r < 32) {
          if (pos + 4 > s.size()) break;
          std::uint32_t u = 0;
          for (int k = 0; k < 4; ++k) {
            const auto b = static_cast<std::uint32_t>(static_cast<unsigned char>(s[pos + static_cast<std::size_t>(big ? k : 3 - k)]));
            u = (u << 8) | b;
          }
          pos += 4;
          values.push_back(static_cast<double>(static_cast<std::int32_t>(u)) / std::ldexp(1.0, r));
        } else if (r < 48) {
          if (pos + 2 > s.size()) break;
          const auto hi = static_cast<unsigned char>(s[pos + (big ? 0 : 1)]);
          const auto lo = static_cast<unsigned char>(s[pos + (big ? 1 : 0)]);
          pos += 2;
          values.push_back(static_cast<double>(static_cast<std::int16_t>(hi * 256 + lo)) / std::ldexp(1.0, r - 32));
        } else {
          if (pos + 4 > s.size()) break;
          std::uint32_t u = 0;
          for (int k = 0; k < 4; ++k) {
            const auto b = static_cast<std::uint32_t>(static_cast<unsigned char>(s[pos + static_cast<std::size_t>(big ? k : 3 - k)]));
            u = (u << 8) | b;
          }
          pos += 4;
          values.push_back(static_cast<double>(std::bit_cast<float>(u)));
        }
      }
    }
    Object arr = in.make_array(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) in.array_put(arr, i, Object::real(values[i]));
    return arr;
  }
  in.error("typecheck");
}
}  // namespace
void Interpreter::op_xshow() {
  const Object nums = numarray(*this, pop());
  const Object s = pop_string();
  show_string(s, 5, 0, 0, 0, 0, 1, nums);
}
void Interpreter::op_yshow() {
  const Object nums = numarray(*this, pop());
  const Object s = pop_string();
  show_string(s, 5, 0, 0, 0, 0, 2, nums);
}
void Interpreter::op_xyshow() {
  const Object nums = numarray(*this, pop());
  const Object s = pop_string();
  show_string(s, 5, 0, 0, 0, 0, 0, nums);
}
void Interpreter::op_glyphshow() {
  const Object n = pop();
  if (n.type != Type::Name) error("typecheck");
  show_string(n, 0, 0, 0, 0, 0, -1, Object::null());
}
void Interpreter::op_cshow() {
  const Object s = pop_string();
  const Object proc = pop_proc();
  show_string(s, 6, 0, 0, 0, 0, -1, proc);
}
void Interpreter::op_stringwidth() {
  const Object s = pop_string();
  text_width_ = {0, 0};
  show_string(s, 7, 0, 0, 0, 0, -1, Object::null());
  push_real(text_width_.x);
  push_real(text_width_.y);
}
void Interpreter::op_charpath() {
  pop_bool();
  const Object s = pop_string();
  show_string(s, 8, 0, 0, 0, 0, -1, Object::null());
}
void Interpreter::op_findencoding() {
  const std::string k = object_string(pop());
  if (k == "ISOLatin1Encoding") push(isolatin1_encoding_);
  else if (k == "StandardEncoding") push(standard_encoding_);
  else error("undefinedresource", k);
}
void Interpreter::op_composefont() {
  need(3);
  pop();
  pop();
  pop();
  error("undefinedresource", "composefont is not supported");
}

void Interpreter::install_font_operators() {
  using I = Interpreter;
  const std::pair<std::string_view, void (I::*)()> ops[] = {
      {"findfont", &I::op_findfont}, {"definefont", &I::op_definefont}, {"undefinefont", &I::op_undefinefont},
      {"scalefont", &I::op_scalefont}, {"makefont", &I::op_makefont}, {"setfont", &I::op_setfont},
      {"currentfont", &I::op_currentfont}, {"rootfont", &I::op_rootfont}, {"selectfont", &I::op_selectfont},
      {"show", &I::op_show}, {"ashow", &I::op_ashow}, {"widthshow", &I::op_widthshow},
      {"awidthshow", &I::op_awidthshow}, {"kshow", &I::op_kshow}, {"xshow", &I::op_xshow}, {"yshow", &I::op_yshow},
      {"xyshow", &I::op_xyshow}, {"glyphshow", &I::op_glyphshow}, {"cshow", &I::op_cshow},
      {"stringwidth", &I::op_stringwidth}, {"charpath", &I::op_charpath}, {"findencoding", &I::op_findencoding},
      {".findencoding", &I::op_findencoding}, {"composefont", &I::op_composefont},
  };
  for (const auto& [name, fn] : ops) define_operator(name, fn);
}

}  // namespace cvt::vector::ps
