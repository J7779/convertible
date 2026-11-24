// Convertible core - pdf family: content stream interpreter.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/interp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "families/pdf/syntax.hpp"

namespace cvt::pdf {

Bytes page_content(PdfFile& file, const Page& page) {
  Bytes out;
  const Object contents = file.resolve(page.dict.get("Contents"));
  const auto add = [&](const Object& stream) {
    const Bytes data = file.decode_or_empty(stream);
    out.insert(out.end(), data.begin(), data.end());
    out.push_back(std::byte{'\n'});
  };
  if (contents.is_stream()) {
    add(contents);
  } else {
    std::size_t count = 0;
    for (const Object& part : contents.as_array()) {
      if (++count > 10000) break;
      add(file.resolve(part));
    }
  }
  return out;
}

PageTransform page_transform(const Page& page, double scale) {
  const Rect box = page.crop_box;
  const double w = box.width() * page.user_unit, h = box.height() * page.user_unit;
  PageTransform t;
  // Move the crop box to the origin, scale, flip y, rotate clockwise.
  Matrix m = Matrix::translate(-box.x0, -box.y0);
  m = Matrix::concat(m, Matrix::scale(scale * page.user_unit, scale * page.user_unit));
  const double sw = w * scale, sh = h * scale;
  switch (page.rotate) {
    case 90:
      // (x, y) -> (y, x) in device with y down: rotate 90 clockwise.
      m = Matrix::concat(m, Matrix{0, 1, 1, 0, 0, 0});
      t.width = sh;
      t.height = sw;
      break;
    case 180:
      m = Matrix::concat(m, Matrix{-1, 0, 0, 1, sw, 0});
      t.width = sw;
      t.height = sh;
      break;
    case 270:
      m = Matrix::concat(m, Matrix{0, -1, -1, 0, sh, sw});
      t.width = sh;
      t.height = sw;
      break;
    default:
      m = Matrix::concat(m, Matrix{1, 0, 0, -1, 0, sh});
      t.width = sw;
      t.height = sh;
      break;
  }
  t.matrix = m;
  return t;
}

struct Interpreter::State {
  GState gs;
  std::vector<GState> stack;
  Path path;
  bool pending_clip = false;
  raster::FillRule clip_rule = raster::FillRule::NonZero;
  Matrix tm, tlm;
  bool in_text = false;
  const Dict* resources = nullptr;
  Object resources_obj;
  std::vector<Object> operands;
  Point current;
};

Interpreter::Interpreter(PdfFile& file, Device& device, FontCache& fonts, const Context* ctx)
    : file_(file), device_(device), fonts_(fonts), ctx_(ctx) {}

std::shared_ptr<PdfFont> Interpreter::font_for(const Object& font_ref) {
  if (font_ref.is_ref()) {
    const auto it = font_cache_.find(font_ref.as_ref().num);
    if (it != font_cache_.end()) return it->second;
    auto font = PdfFont::load(file_, font_ref, fonts_);
    if (font_cache_.size() < 4096) font_cache_[font_ref.as_ref().num] = font;
    return font;
  }
  const Object resolved = file_.resolve(font_ref);
  const void* key = resolved.dict_ptr().get();
  if (key != nullptr) {
    const auto it = direct_font_cache_.find(key);
    if (it != direct_font_cache_.end()) return it->second;
  }
  auto font = PdfFont::load(file_, resolved, fonts_);
  if (key != nullptr && direct_font_cache_.size() < 4096) direct_font_cache_[key] = font;
  return font;
}

namespace {

double num(const std::vector<Object>& ops, std::size_t i) {
  return i < ops.size() ? ops[i].as_number(0.0) : 0.0;
}

BlendMode blend_from(const Object& o) {
  const Object& name = o.is_array() && !o.as_array().empty() ? o.as_array()[0] : o;
  const std::string& n = name.as_name();
  static constexpr std::pair<std::string_view, BlendMode> kModes[] = {
      {"Multiply", BlendMode::Multiply}, {"Screen", BlendMode::Screen}, {"Overlay", BlendMode::Overlay},
      {"Darken", BlendMode::Darken}, {"Lighten", BlendMode::Lighten}, {"ColorDodge", BlendMode::ColorDodge},
      {"ColorBurn", BlendMode::ColorBurn}, {"HardLight", BlendMode::HardLight}, {"SoftLight", BlendMode::SoftLight},
      {"Difference", BlendMode::Difference}, {"Exclusion", BlendMode::Exclusion}, {"Hue", BlendMode::Hue},
      {"Saturation", BlendMode::Saturation}, {"Color", BlendMode::Color}, {"Luminosity", BlendMode::Luminosity}};
  for (const auto& [key, mode] : kModes) {
    if (n == key) return mode;
  }
  return BlendMode::Normal;
}

std::string expand_inline_key(const std::string& key) {
  if (key == "BPC") return "BitsPerComponent";
  if (key == "CS") return "ColorSpace";
  if (key == "D") return "Decode";
  if (key == "DP") return "DecodeParms";
  if (key == "F") return "Filter";
  if (key == "H") return "Height";
  if (key == "IM") return "ImageMask";
  if (key == "I") return "Interpolate";
  if (key == "W") return "Width";
  if (key == "L") return "Length";
  return key;
}

}  // namespace

void Interpreter::run_page(const Page& page, const Matrix& base) {
  const Bytes content = page_content(file_, page);
  GState gs;
  gs.ctm = base;
  device_.save();
  // Clip to the crop box.
  Path clip;
  clip.rect(page.crop_box.x0, page.crop_box.y0, page.crop_box.width(), page.crop_box.height());
  device_.clip(clip, raster::FillRule::NonZero, gs);
  run(content, page.resources, gs, 0);
  device_.restore();
}

void Interpreter::run_form(const Object& form_ref, GState gs, int depth, const Object& fallback_resources) {
  if (depth > limits.max_depth || aborted_) return;
  if (++invocations_ > limits.max_invocations) {
    aborted_ = true;
    file_.add_warning("a page draws too many nested objects; it was cut short");
    return;
  }
  const Object form = file_.resolve(form_ref);
  if (!form.is_stream()) return;
  const Dict& dict = form.as_dict();
  const Array& m = file_.get(dict, "Matrix").as_array();
  if (m.size() == 6) {
    const Matrix fm{file_.resolve(m[0]).as_number(1), file_.resolve(m[1]).as_number(), file_.resolve(m[2]).as_number(),
                    file_.resolve(m[3]).as_number(1), file_.resolve(m[4]).as_number(), file_.resolve(m[5]).as_number()};
    if (fm.finite()) gs.ctm = Matrix::concat(fm, gs.ctm);
  }
  device_.save();
  const Array& bbox = file_.get(dict, "BBox").as_array();
  Rect box;
  if (bbox.size() == 4) {
    box = Rect{file_.resolve(bbox[0]).as_number(), file_.resolve(bbox[1]).as_number(), file_.resolve(bbox[2]).as_number(),
               file_.resolve(bbox[3]).as_number()}
              .normalized();
    Path clip;
    clip.rect(box.x0, box.y0, box.width(), box.height());
    device_.clip(clip, raster::FillRule::NonZero, gs);
  }
  bool group = false;
  const Dict& group_dict = file_.get(dict, "Group").as_dict();
  if (file_.get(group_dict, "S").is_name("Transparency")) {
    GroupInfo info;
    info.bbox = box;
    info.isolated = file_.get(group_dict, "I").as_bool(false);
    info.knockout = file_.get(group_dict, "K").as_bool(false);
    group = device_.begin_group(info, gs);
  }
  Object resources = file_.get(dict, "Resources");
  if (!resources.is_dict()) resources = fallback_resources;
  std::shared_ptr<const Bytes> content;
  const void* key = form.stream_ptr().get();
  if (const auto it = form_content_cache_.find(key); it != form_content_cache_.end()) {
    content = it->second;
  } else {
    content = std::make_shared<const Bytes>(file_.decode_or_empty(form));
    if (form_content_cache_.size() < 256) form_content_cache_[key] = content;
  }
  GState inner = gs;
  if (group) {
    // Inside the group the constant alpha and blend restart.
    inner.fill_alpha = 1;
    inner.stroke_alpha = 1;
    inner.blend = BlendMode::Normal;
    inner.soft_mask = nullptr;
  }
  run(*content, resources, inner, depth + 1);
  if (group) device_.end_group(gs);
  device_.restore();
}

void Interpreter::show_text(State& st, std::string_view bytes, const Dict& resources, int depth) {
  GState& gs = st.gs;
  if (!gs.font) {
    // No font set: use a default so text is not lost.
    Dict fake;
    fake.set("Type", Object::name("Font"));
    fake.set("Subtype", Object::name("Type1"));
    fake.set("BaseFont", Object::name("Helvetica"));
    gs.font = PdfFont::load(file_, Object(std::move(fake)), fonts_);
  }
  PdfFont& font = *gs.font;
  std::vector<GlyphCode> codes;
  font.decode(bytes, codes);
  const double fs = gs.font_size;
  const double th = gs.h_scale;
  for (const GlyphCode& code : codes) {
    GlyphEvent ev;
    ev.code = code;
    ev.font = &font;
    ev.space_code = font.is_space_code(code);
    const double w0 = font.width(code);
    if (!font.vertical()) {
      const Matrix text_space{fs * th, 0, 0, fs, 0, gs.rise};
      ev.trm = Matrix::concat(text_space, Matrix::concat(st.tm, gs.ctm));
      ev.width = w0 * fs * th;
      const double tx = (w0 * fs + gs.char_spacing + (ev.space_code ? gs.word_spacing : 0)) * th;
      ev.advance = tx;
      ev.unicode = font.unicode(code);
      device_.glyph(ev, gs);
      st.tm = Matrix::concat(Matrix::translate(tx, 0), st.tm);
    } else {
      const Point v = font.vertical_origin(code);
      const Matrix text_space{fs * th, 0, 0, fs, -v.x * fs * th, -v.y * fs + gs.rise};
      ev.trm = Matrix::concat(text_space, Matrix::concat(st.tm, gs.ctm));
      ev.width = w0 * fs * th;
      const double ty = font.vertical_advance(code) * fs + gs.char_spacing + (ev.space_code ? gs.word_spacing : 0);
      ev.advance = -ty;
      ev.unicode = font.unicode(code);
      device_.glyph(ev, gs);
      st.tm = Matrix::concat(Matrix::translate(0, ty), st.tm);
    }
    if (font.kind() == PdfFont::Kind::Type3 && device_.execute_type3() && gs.render_mode != 3 &&
        depth < limits.max_depth && ++invocations_ <= limits.max_invocations) {
      const Object proc = file_.resolve(font.type3_charproc(code));
      if (proc.is_stream()) {
        GState glyph_gs = gs;
        glyph_gs.ctm = Matrix::concat(font.type3_matrix(), ev.trm);
        device_.save();
        const Bytes content = file_.decode_or_empty(proc);
        Object res = font.type3_resources();
        if (!file_.resolve(res).is_dict()) res = st.resources_obj;
        run(content, res, glyph_gs, depth + 1);
        device_.restore();
      }
    }
  }
  (void)resources;
}

void Interpreter::run(ByteView content, const Object& resources_ref, GState& gs_in, int depth) {
  if (depth > limits.max_depth || aborted_) return;
  State st;
  st.gs = gs_in;
  st.resources_obj = file_.resolve(resources_ref);
  st.resources = &st.resources_obj.as_dict();
  const Dict& res = *st.resources;
  Lexer lexer(content);
  auto& ops = st.operands;
  int text_depth_guard = 0;
  while (true) {
    if (aborted_) break;
    const std::size_t before = lexer.position();
    Token t = lexer.peek();
    if (t.kind == Token::Kind::End) break;
    if (t.kind != Token::Kind::Keyword || t.text == "true" || t.text == "false" || t.text == "null") {
      if (t.kind == Token::Kind::Error || t.kind == Token::Kind::ArrayClose || t.kind == Token::Kind::DictClose ||
          t.kind == Token::Kind::BraceOpen || t.kind == Token::Kind::BraceClose) {
        (void)lexer.next();
        continue;
      }
      bool ok = true;
      Object operand = parse_object(lexer, 16, &ok);
      if (lexer.position() == before) (void)lexer.next();
      if (ops.size() < 64) ops.push_back(std::move(operand));
      continue;
    }
    (void)lexer.next();
    const std::string op = std::move(t.text);
    if (++operators_ > limits.max_operators) {
      aborted_ = true;
      file_.add_warning("a page has too many drawing operations; it was cut short");
      break;
    }
    if ((operators_ & 0x3FF) == 0 && device_.exhausted()) {
      aborted_ = true;
      file_.add_warning("a page needs too much drawing work; it was cut short");
      break;
    }
    if ((operators_ & 0xFFFF) == 0 && ctx_ != nullptr && ctx_->progress.cancelled()) {
      aborted_ = true;
      break;
    }
    GState& gs = st.gs;
    const auto paint_end = [&]() {
      if (st.pending_clip) {
        device_.clip(st.path, st.clip_rule, gs);
        st.pending_clip = false;
      }
      st.path.clear();
    };
    if (op == "q") {
      if (st.stack.size() < 256) {
        st.stack.push_back(gs);
        device_.save();
      }
    } else if (op == "Q") {
      if (!st.stack.empty()) {
        gs = std::move(st.stack.back());
        st.stack.pop_back();
        device_.restore();
      }
    } else if (op == "cm") {
      if (ops.size() >= 6) {
        const Matrix m{num(ops, 0), num(ops, 1), num(ops, 2), num(ops, 3), num(ops, 4), num(ops, 5)};
        if (m.finite()) gs.ctm = Matrix::concat(m, gs.ctm);
      }
    } else if (op == "w") {
      gs.stroke.width = std::abs(num(ops, 0));
    } else if (op == "J") {
      gs.stroke.cap = static_cast<raster::LineCap>(std::clamp(static_cast<int>(num(ops, 0)), 0, 2));
    } else if (op == "j") {
      gs.stroke.join = static_cast<raster::LineJoin>(std::clamp(static_cast<int>(num(ops, 0)), 0, 2));
    } else if (op == "M") {
      gs.stroke.miter_limit = std::max(1.0, num(ops, 0));
    } else if (op == "d") {
      gs.stroke.dash.clear();
      if (!ops.empty()) {
        for (const Object& v : ops[0].as_array()) {
          if (gs.stroke.dash.size() < 64) gs.stroke.dash.push_back(std::abs(file_.resolve(v).as_number()));
        }
      }
      gs.stroke.dash_phase = num(ops, 1);
    } else if (op == "gs") {
      if (!ops.empty()) {
        const Dict& egs = file_.get(file_.get(res, "ExtGState").as_dict(), ops[0].as_name()).as_dict();
        for (const auto& [key, value_ref] : egs) {
          const Object value = file_.resolve(value_ref);
          if (key == "LW") gs.stroke.width = std::abs(value.as_number(gs.stroke.width));
          else if (key == "LC") gs.stroke.cap = static_cast<raster::LineCap>(std::clamp(static_cast<int>(value.as_int()), 0, 2));
          else if (key == "LJ") gs.stroke.join = static_cast<raster::LineJoin>(std::clamp(static_cast<int>(value.as_int()), 0, 2));
          else if (key == "ML") gs.stroke.miter_limit = std::max(1.0, value.as_number(10));
          else if (key == "D" && value.is_array() && value.as_array().size() == 2) {
            gs.stroke.dash.clear();
            for (const Object& v : file_.resolve(value.as_array()[0]).as_array()) gs.stroke.dash.push_back(std::abs(file_.resolve(v).as_number()));
            gs.stroke.dash_phase = file_.resolve(value.as_array()[1]).as_number();
          } else if (key == "CA") gs.stroke_alpha = std::clamp(value.as_number(1), 0.0, 1.0);
          else if (key == "ca") gs.fill_alpha = std::clamp(value.as_number(1), 0.0, 1.0);
          else if (key == "BM") gs.blend = blend_from(value);
          else if (key == "AIS") gs.alpha_is_shape = value.as_bool();
          else if (key == "SMask") {
            if (value.is_dict()) gs.soft_mask = device_.make_soft_mask(value.as_dict(), gs);
            else gs.soft_mask = nullptr;
          } else if (key == "Font" && value.is_array() && value.as_array().size() == 2) {
            gs.font = font_for(value.as_array()[0]);
            gs.font_size = file_.resolve(value.as_array()[1]).as_number();
          }
        }
      }
    } else if (op == "m") {
      st.path.move_to(num(ops, 0), num(ops, 1));
      st.current = {num(ops, 0), num(ops, 1)};
    } else if (op == "l") {
      if (st.path.empty()) st.path.move_to(st.current.x, st.current.y);
      st.path.line_to(num(ops, 0), num(ops, 1));
      st.current = {num(ops, 0), num(ops, 1)};
    } else if (op == "c") {
      if (st.path.empty()) st.path.move_to(st.current.x, st.current.y);
      st.path.cubic_to(num(ops, 0), num(ops, 1), num(ops, 2), num(ops, 3), num(ops, 4), num(ops, 5));
      st.current = {num(ops, 4), num(ops, 5)};
    } else if (op == "v") {
      if (st.path.empty()) st.path.move_to(st.current.x, st.current.y);
      st.path.cubic_to(st.current.x, st.current.y, num(ops, 0), num(ops, 1), num(ops, 2), num(ops, 3));
      st.current = {num(ops, 2), num(ops, 3)};
    } else if (op == "y") {
      if (st.path.empty()) st.path.move_to(st.current.x, st.current.y);
      st.path.cubic_to(num(ops, 0), num(ops, 1), num(ops, 2), num(ops, 3), num(ops, 2), num(ops, 3));
      st.current = {num(ops, 2), num(ops, 3)};
    } else if (op == "h") {
      st.path.close();
    } else if (op == "re") {
      st.path.rect(num(ops, 0), num(ops, 1), num(ops, 2), num(ops, 3));
      st.current = {num(ops, 0), num(ops, 1)};
    } else if (op == "S" || op == "s") {
      if (op == "s") st.path.close();
      if (device_.wants_paths() && !st.path.empty()) device_.stroke(st.path, gs);
      paint_end();
    } else if (op == "f" || op == "F" || op == "f*") {
      if (device_.wants_paths() && !st.path.empty()) {
        device_.fill(st.path, op == "f*" ? raster::FillRule::EvenOdd : raster::FillRule::NonZero, gs);
      }
      paint_end();
    } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
      if (op[0] == 'b') st.path.close();
      if (device_.wants_paths() && !st.path.empty()) {
        device_.fill(st.path, op.size() == 2 ? raster::FillRule::EvenOdd : raster::FillRule::NonZero, gs);
        device_.stroke(st.path, gs);
      }
      paint_end();
    } else if (op == "n") {
      paint_end();
    } else if (op == "W" || op == "W*") {
      st.pending_clip = true;
      st.clip_rule = op == "W*" ? raster::FillRule::EvenOdd : raster::FillRule::NonZero;
    } else if (op == "CS" || op == "cs") {
      const bool fill = op == "cs";
      auto space = ops.empty() ? nullptr : load_color_space(file_, ops[0], &res);
      if (!space) space = device_gray();
      (fill ? gs.fill_color : gs.stroke_color) = space->initial_color();
      (fill ? gs.fill_space : gs.stroke_space) = std::move(space);
      (fill ? gs.fill_pattern : gs.stroke_pattern) = Object{};
    } else if (op == "SC" || op == "SCN" || op == "sc" || op == "scn") {
      const bool fill = op[0] == 's';
      std::vector<double>& color = fill ? gs.fill_color : gs.stroke_color;
      color.clear();
      for (const Object& o : ops) {
        if (o.is_number()) color.push_back(o.as_number());
      }
      if (!ops.empty() && ops.back().is_name()) {
        const Object pattern = file_.get(file_.get(res, "Pattern").as_dict(), ops.back().as_name());
        (fill ? gs.fill_pattern : gs.stroke_pattern) = pattern;
      }
      const auto& space = fill ? gs.fill_space : gs.stroke_space;
      if (space && space->kind() != ColorSpace::Kind::Pattern) {
        color.resize(static_cast<std::size_t>(space->components()), 0.0);
      }
    } else if (op == "G" || op == "g") {
      const bool fill = op == "g";
      (fill ? gs.fill_space : gs.stroke_space) = device_gray();
      (fill ? gs.fill_color : gs.stroke_color) = {num(ops, 0)};
    } else if (op == "RG" || op == "rg") {
      const bool fill = op == "rg";
      (fill ? gs.fill_space : gs.stroke_space) = device_rgb();
      (fill ? gs.fill_color : gs.stroke_color) = {num(ops, 0), num(ops, 1), num(ops, 2)};
    } else if (op == "K" || op == "k") {
      const bool fill = op == "k";
      (fill ? gs.fill_space : gs.stroke_space) = device_cmyk();
      (fill ? gs.fill_color : gs.stroke_color) = {num(ops, 0), num(ops, 1), num(ops, 2), num(ops, 3)};
    } else if (op == "sh") {
      if (!ops.empty()) {
        const Object shading = file_.get(file_.get(res, "Shading").as_dict(), ops[0].as_name());
        if (!shading.is_null() && device_.wants_paths()) device_.shading(shading, gs);
      }
    } else if (op == "Do") {
      if (!ops.empty()) {
        const Object xobj_ref = file_.get(res, "XObject").as_dict().get(ops[0].as_name());
        const Object xobj = file_.resolve(xobj_ref);
        const std::string subtype = file_.get(xobj.as_dict(), "Subtype").as_name();
        if (subtype == "Image") {
          if (device_.wants_images()) {
            ImageEvent ev;
            ev.dict = &xobj.as_dict();
            ev.stream = xobj;
            ev.resources = st.resources;
            device_.image(ev, gs);
          }
        } else if (subtype == "Form" || (subtype.empty() && xobj.is_stream())) {
          run_form(xobj, gs, depth + 1, st.resources_obj);
        }
      }
    } else if (op == "BI") {
      Dict dict;
      while (true) {
        Token key = lexer.next();
        if (key.kind == Token::Kind::End) break;
        if (key.kind == Token::Kind::Keyword && key.text == "ID") break;
        if (key.kind != Token::Kind::Name) continue;
        bool ok = true;
        Object value = parse_object(lexer, 8, &ok);
        dict.set(expand_inline_key(key.text), std::move(value));
        if (dict.size() > 64) break;
      }
      // One whitespace byte follows ID.
      std::size_t start = lexer.position();
      if (start < content.size() && is_pdf_space(std::to_integer<std::uint8_t>(content[start]))) ++start;
      // Find "EI" surrounded by whitespace.
      std::size_t end = start;
      const std::string_view text = as_chars(content);
      const std::int64_t declared = dict.get("Length").as_int(-1);
      bool found = false;
      if (declared >= 0 && start + static_cast<std::size_t>(declared) <= text.size()) {
        std::size_t probe = start + static_cast<std::size_t>(declared);
        while (probe < text.size() && is_pdf_space(static_cast<std::uint8_t>(text[probe]))) ++probe;
        if (text.substr(probe, 2) == "EI") {
          end = start + static_cast<std::size_t>(declared);
          lexer.seek(probe + 2);
          found = true;
        }
      }
      for (std::size_t p = start; !found && p + 1 < text.size(); ++p) {
        if (text[p] == 'E' && text[p + 1] == 'I' && (p == start || is_pdf_space(static_cast<std::uint8_t>(text[p - 1]))) &&
            (p + 2 >= text.size() || is_pdf_space(static_cast<std::uint8_t>(text[p + 2])))) {
          // Check that what follows looks like content (not binary).
          bool plausible = true;
          for (std::size_t k = p + 2; k < std::min(text.size(), p + 12); ++k) {
            const auto c = static_cast<std::uint8_t>(text[k]);
            if (c >= 0x80 || (c < 0x20 && !is_pdf_space(c))) plausible = false;
          }
          if (!plausible) continue;
          end = p > start ? p - 1 : p;
          lexer.seek(p + 2);
          found = true;
        }
      }
      if (!found) {
        lexer.seek(text.size());
        end = text.size();
      }
      if (device_.wants_images()) {
        ImageEvent ev;
        ev.dict = &dict;
        ev.is_inline = true;
        ev.inline_data.assign(content.begin() + static_cast<std::ptrdiff_t>(std::min(start, content.size())),
                              content.begin() + static_cast<std::ptrdiff_t>(std::max(std::min(end, content.size()), std::min(start, content.size()))));
        ev.resources = st.resources;
        device_.image(ev, gs);
      }
    } else if (op == "BT") {
      st.tm = Matrix{};
      st.tlm = Matrix{};
      st.in_text = true;
      ++text_depth_guard;
    } else if (op == "ET") {
      st.in_text = false;
      device_.end_text(gs);
    } else if (op == "Tc") {
      gs.char_spacing = num(ops, 0);
    } else if (op == "Tw") {
      gs.word_spacing = num(ops, 0);
    } else if (op == "Tz") {
      gs.h_scale = num(ops, 0) / 100.0;
    } else if (op == "TL") {
      gs.leading = num(ops, 0);
    } else if (op == "Tf") {
      if (ops.size() >= 2) {
        const Object font_ref = file_.get(res, "Font").as_dict().get(ops[0].as_name());
        gs.font = font_for(font_ref);
        gs.font_size = num(ops, 1);
      }
    } else if (op == "Tr") {
      gs.render_mode = std::clamp(static_cast<int>(num(ops, 0)), 0, 7);
    } else if (op == "Ts") {
      gs.rise = num(ops, 0);
    } else if (op == "Td" || op == "TD") {
      if (op == "TD") gs.leading = -num(ops, 1);
      st.tlm = Matrix::concat(Matrix::translate(num(ops, 0), num(ops, 1)), st.tlm);
      st.tm = st.tlm;
    } else if (op == "Tm") {
      if (ops.size() >= 6) {
        const Matrix m{num(ops, 0), num(ops, 1), num(ops, 2), num(ops, 3), num(ops, 4), num(ops, 5)};
        if (m.finite()) {
          st.tlm = m;
          st.tm = m;
        }
      }
    } else if (op == "T*") {
      st.tlm = Matrix::concat(Matrix::translate(0, -gs.leading), st.tlm);
      st.tm = st.tlm;
    } else if (op == "Tj") {
      if (!ops.empty()) show_text(st, ops[0].as_string(), res, depth);
    } else if (op == "'" || op == "\"") {
      if (op == "\"" && ops.size() >= 3) {
        gs.word_spacing = num(ops, 0);
        gs.char_spacing = num(ops, 1);
      }
      st.tlm = Matrix::concat(Matrix::translate(0, -gs.leading), st.tlm);
      st.tm = st.tlm;
      if (!ops.empty()) show_text(st, ops.back().as_string(), res, depth);
    } else if (op == "TJ") {
      if (!ops.empty()) {
        for (const Object& item : ops[0].as_array()) {
          if (item.is_string()) {
            show_text(st, item.as_string(), res, depth);
          } else if (item.is_number() && gs.font) {
            const double adjust = -item.as_number() / 1000.0 * gs.font_size;
            if (gs.font->vertical()) {
              st.tm = Matrix::concat(Matrix::translate(0, adjust), st.tm);
            } else {
              st.tm = Matrix::concat(Matrix::translate(adjust * gs.h_scale, 0), st.tm);
            }
          }
        }
      }
    } else if (op == "BMC") {
      device_.begin_marked(ops.empty() ? std::string() : ops[0].as_name(), Dict{});
    } else if (op == "BDC") {
      Dict props;
      if (ops.size() >= 2) {
        if (ops[1].is_dict()) {
          props = ops[1].as_dict();
        } else if (ops[1].is_name()) {
          props = file_.get(file_.get(res, "Properties").as_dict(), ops[1].as_name()).as_dict();
        }
      }
      device_.begin_marked(ops.empty() ? std::string() : ops[0].as_name(), props);
    } else if (op == "EMC") {
      device_.end_marked();
    }
    // d0, d1, BX, EX, MP, DP, ri, i and unknown operators are ignored.
    ops.clear();
  }
  // Unbalanced q: restore the device state.
  while (!st.stack.empty()) {
    st.stack.pop_back();
    device_.restore();
  }
  (void)text_depth_guard;
}

}  // namespace cvt::pdf
