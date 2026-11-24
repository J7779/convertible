// Convertible core - vector family: PostScript functions and smooth shading.
//
// Axial (type 2) and radial (type 3) shadings become scene gradients with
// stops sampled from their functions; the other shading types are
// approximated by a solid colour.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "families/vector/geom.hpp"
#include "families/vector/ps_internal.hpp"

namespace cvt::vector::ps {

namespace {

std::vector<double> numbers_of(Interpreter& in, const Object* arr) {
  std::vector<double> out;
  if (arr == nullptr || arr->type != Type::Array) return out;
  for (std::uint32_t i = 0; i < arr->size && i < 4096; ++i) {
    const Object e = in.array_get(*arr, i);
    if (e.is_number()) out.push_back(e.number());
  }
  return out;
}

}  // namespace

std::vector<double> Interpreter::eval_function(const Object& fn, double t, int depth) {
  if (depth > 16) error("limitcheck", "functions nested too deeply");
  if (fn.type == Type::Array && fn.exec) {
    const std::size_t before = ostack_.size();
    push_real(t);
    call(fn);
    std::vector<double> out;
    while (ostack_.size() > before) {
      const Object o = ostack_.back();
      ostack_.pop_back();
      out.insert(out.begin(), o.is_number() ? o.number() : 0.0);
    }
    return out;
  }
  if (fn.type == Type::Array) {
    // An array of single-output functions.
    std::vector<double> out;
    for (std::uint32_t i = 0; i < fn.size && i < 32; ++i) {
      const std::vector<double> v = eval_function(array_get(fn, i), t, depth + 1);
      out.push_back(v.empty() ? 0.0 : v[0]);
    }
    return out;
  }
  if (fn.type != Type::Dict) error("typecheck", "bad function");
  const Object* ft = dict_find(fn, "FunctionType");
  const int type = ft != nullptr && ft->is_number() ? static_cast<int>(ft->number()) : -1;
  const std::vector<double> domain = numbers_of(*this, dict_find(fn, "Domain"));
  if (domain.size() >= 2) t = std::clamp(t, std::min(domain[0], domain[1]), std::max(domain[0], domain[1]));
  std::vector<double> out;
  switch (type) {
    case 2: {
      std::vector<double> c0 = numbers_of(*this, dict_find(fn, "C0"));
      std::vector<double> c1 = numbers_of(*this, dict_find(fn, "C1"));
      if (c0.empty()) c0 = {0.0};
      if (c1.empty()) c1 = {1.0};
      const Object* n_obj = dict_find(fn, "N");
      const double n = n_obj != nullptr && n_obj->is_number() ? n_obj->number() : 1.0;
      const double tn = n == 1.0 ? t : std::pow(std::max(t, 0.0), n);
      for (std::size_t i = 0; i < std::min(c0.size(), c1.size()); ++i) out.push_back(c0[i] + tn * (c1[i] - c0[i]));
      break;
    }
    case 3: {
      const Object* fns = dict_find(fn, "Functions");
      if (fns == nullptr || fns->type != Type::Array || fns->size == 0) error("rangecheck", "stitching function");
      const std::vector<double> bounds = numbers_of(*this, dict_find(fn, "Bounds"));
      const std::vector<double> encode = numbers_of(*this, dict_find(fn, "Encode"));
      const double d0 = domain.size() >= 2 ? domain[0] : 0.0, d1 = domain.size() >= 2 ? domain[1] : 1.0;
      std::size_t k = 0;
      while (k < bounds.size() && t >= bounds[k]) ++k;
      k = std::min<std::size_t>(k, fns->size - 1);
      const double lo = k == 0 ? d0 : bounds[k - 1];
      const double hi = k < bounds.size() ? bounds[k] : d1;
      double e0 = 0, e1 = 1;
      if (encode.size() >= 2 * (k + 1)) {
        e0 = encode[2 * k];
        e1 = encode[2 * k + 1];
      }
      const double u = hi > lo ? e0 + (t - lo) * (e1 - e0) / (hi - lo) : e0;
      out = eval_function(array_get(*fns, static_cast<std::uint32_t>(k)), u, depth + 1);
      break;
    }
    case 0: {
      const std::vector<double> size = numbers_of(*this, dict_find(fn, "Size"));
      const std::vector<double> range = numbers_of(*this, dict_find(fn, "Range"));
      const Object* bps_obj = dict_find(fn, "BitsPerSample");
      const int bps = bps_obj != nullptr && bps_obj->is_number() ? static_cast<int>(bps_obj->number()) : 8;
      if (size.empty() || range.size() < 2 || size[0] < 1 || size[0] > 1e6) error("rangecheck", "sampled function");
      const std::size_t m = static_cast<std::size_t>(size[0]);
      const std::size_t nout = range.size() / 2;
      std::vector<double> encode = numbers_of(*this, dict_find(fn, "Encode"));
      std::vector<double> decode = numbers_of(*this, dict_find(fn, "Decode"));
      if (encode.size() < 2) encode = {0.0, static_cast<double>(m - 1)};
      if (decode.size() < 2 * nout) decode = range;
      const Object* src = dict_find(fn, "DataSource");
      std::string data;
      if (src != nullptr && src->type == Type::String) {
        data = std::string(string_view(*src));
      } else if (src != nullptr && src->type == Type::File) {
        // Read once and keep it as a string in the dictionary.
        const std::size_t need_bytes = (m * nout * static_cast<std::size_t>(std::max(bps, 1)) + 7) / 8;
        data = file_read(*src, need_bytes);
        dict_put(fn, "DataSource", make_string(data));
      }
      const double d0 = domain.size() >= 2 ? domain[0] : 0.0, d1 = domain.size() >= 2 ? domain[1] : 1.0;
      double e = d1 > d0 ? encode[0] + (t - d0) * (encode[1] - encode[0]) / (d1 - d0) : encode[0];
      e = std::clamp(e, 0.0, static_cast<double>(m - 1));
      const std::size_t i0 = static_cast<std::size_t>(std::floor(e));
      const std::size_t i1 = std::min(i0 + 1, m - 1);
      const double frac = e - static_cast<double>(i0);
      const double maxv = std::ldexp(1.0, std::clamp(bps, 1, 32)) - 1;
      auto sample = [&](std::size_t idx, std::size_t comp) {
        const std::size_t bit = (idx * nout + comp) * static_cast<std::size_t>(bps);
        std::uint64_t v = 0;
        for (int b = 0; b < bps; ++b) {
          const std::size_t pos = bit + static_cast<std::size_t>(b);
          const std::size_t byte = pos / 8;
          const int bitv = byte < data.size() ? (static_cast<unsigned char>(data[byte]) >> (7 - pos % 8)) & 1 : 0;
          v = (v << 1) | static_cast<std::uint64_t>(bitv);
        }
        return static_cast<double>(v);
      };
      for (std::size_t c = 0; c < nout; ++c) {
        const double s = sample(i0, c) * (1 - frac) + sample(i1, c) * frac;
        out.push_back(decode[2 * c] + s * (decode[2 * c + 1] - decode[2 * c]) / maxv);
      }
      break;
    }
    default: error("rangecheck", std::format("FunctionType {} is not supported", type));
  }
  const std::vector<double> range = numbers_of(*this, dict_find(fn, "Range"));
  for (std::size_t i = 0; i < out.size() && 2 * i + 1 < range.size(); ++i) {
    out[i] = std::clamp(out[i], std::min(range[2 * i], range[2 * i + 1]), std::max(range[2 * i], range[2 * i + 1]));
  }
  return out;
}

std::optional<Paint> Interpreter::shading_paint(const Object& shading, const Matrix& space) {
  if (shading.type != Type::Dict) return std::nullopt;
  const Object* st = dict_find(shading, "ShadingType");
  const int type = st != nullptr && st->is_number() ? static_cast<int>(st->number()) : 0;
  const Object* cs_obj = dict_find(shading, "ColorSpace");
  ColorSpace cs;
  if (cs_obj != nullptr) cs = parse_color_space(*cs_obj);
  else {
    cs.family = Family::RGB;
    cs.components = 3;
  }
  const Object* fn = dict_find(shading, "Function");
  const std::vector<double> coords = numbers_of(*this, dict_find(shading, "Coords"));
  std::vector<double> domain = numbers_of(*this, dict_find(shading, "Domain"));
  if (domain.size() < 2) domain = {0.0, 1.0};

  if ((type == 2 && coords.size() >= 4) || (type == 3 && coords.size() >= 6)) {
    if (fn == nullptr) return std::nullopt;
    auto g = std::make_shared<Gradient>();
    int samples = 33;
    if (fn->type == Type::Dict) {
      const Object* ft = dict_find(*fn, "FunctionType");
      const Object* n = dict_find(*fn, "N");
      if (ft != nullptr && ft->is_number() && ft->number() == 2 && (n == nullptr || (n->is_number() && n->number() == 1)) &&
          (cs.family == Family::Gray || cs.family == Family::RGB)) {
        samples = 2;
      }
    }
    std::vector<double> offsets;
    for (int i = 0; i < samples; ++i) offsets.push_back(static_cast<double>(i) / (samples - 1));
    // Stitching functions of linear pieces: stops exactly at the piece ends
    // (two stops at a bound where the colour jumps).
    bool exact = false;
    if (fn->type == Type::Dict && (cs.family == Family::Gray || cs.family == Family::RGB || cs.family == Family::CMYK)) {
      const Object* ft = dict_find(*fn, "FunctionType");
      const Object* fns = dict_find(*fn, "Functions");
      if (ft != nullptr && ft->is_number() && ft->number() == 3 && fns != nullptr && fns->type == Type::Array &&
          fns->size > 0 && fns->size <= 256) {
        exact = true;
        const std::vector<double> bounds = numbers_of(*this, dict_find(*fn, "Bounds"));
        const std::vector<double> encode = numbers_of(*this, dict_find(*fn, "Encode"));
        if (bounds.size() + 1 != fns->size) exact = false;
        for (std::uint32_t i = 0; exact && i < fns->size; ++i) {
          const Object sub = array_get(*fns, i);
          const Object* st2 = sub.type == Type::Dict ? dict_find(sub, "FunctionType") : nullptr;
          const Object* n = sub.type == Type::Dict ? dict_find(sub, "N") : nullptr;
          exact = st2 != nullptr && st2->is_number() && st2->number() == 2 &&
                  (n == nullptr || (n->is_number() && n->number() == 1));
        }
        if (exact) {
          const double span = domain[1] - domain[0];
          for (std::uint32_t i = 0; i < fns->size; ++i) {
            const double lo = i == 0 ? domain[0] : bounds[i - 1];
            const double hi = i + 1 < fns->size ? bounds[i] : domain[1];
            const double e0 = encode.size() >= 2 * (i + 1) ? encode[2 * i] : 0.0;
            const double e1 = encode.size() >= 2 * (i + 1) ? encode[2 * i + 1] : 1.0;
            const Object sub = array_get(*fns, i);
            for (const auto& [t, e] : {std::pair{lo, e0}, std::pair{hi, e1}}) {
              const double off = span != 0 ? std::clamp((t - domain[0]) / span, 0.0, 1.0) : 0.0;
              const GradientStop stop{off, resolve_color(cs, eval_function(sub, e))};
              if (!g->stops.empty() && g->stops.back() == stop) continue;
              g->stops.push_back(stop);
            }
          }
        }
      }
    }
    if (!exact) {
      for (const double off : offsets) {
        const double t = domain[0] + off * (domain[1] - domain[0]);
        const std::vector<double> comps = eval_function(*fn, t);
        g->stops.push_back(GradientStop{std::clamp(off, 0.0, 1.0), resolve_color(cs, comps)});
      }
    }
    g->transform = space;
    if (type == 2) {
      g->type = Gradient::Type::Linear;
      g->p1 = {coords[0], coords[1]};
      g->p2 = {coords[2], coords[3]};
    } else {
      g->type = Gradient::Type::Radial;
      double x0 = coords[0], y0 = coords[1], r0 = std::abs(coords[2]);
      double x1 = coords[3], y1 = coords[4], r1 = std::abs(coords[5]);
      if (r0 > r1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        std::swap(r0, r1);
        std::reverse(g->stops.begin(), g->stops.end());
        for (GradientStop& s : g->stops) s.offset = 1.0 - s.offset;
      }
      g->focal = {x0, y0};
      g->focal_radius = r0;
      g->center = {x1, y1};
      g->radius = r1;
    }
    const std::vector<double> extend = [&] {
      std::vector<double> e;
      if (const Object* ex = dict_find(shading, "Extend"); ex != nullptr && ex->type == Type::Array) {
        for (std::uint32_t i = 0; i < ex->size; ++i) {
          const Object b = array_get(*ex, i);
          e.push_back(b.type == Type::Boolean && b.ival != 0 ? 1.0 : 0.0);
        }
      }
      return e;
    }();
    if (extend.size() < 2 || extend[0] == 0 || extend[1] == 0) {
      warn_once("shadings that do not extend past their ends are drawn extended");
    }
    Paint p;
    p.type = Paint::Type::Gradient;
    p.gradient = std::move(g);
    return p;
  }
  // Other shading types: a representative solid colour.
  warn_once(std::format("shading type {} is approximated by a solid colour", type));
  std::vector<double> comps;
  if (fn != nullptr) {
    try {
      comps = eval_function(*fn, (domain[0] + domain[1]) / 2);
    } catch (const PsError&) {
      comps.clear();
    }
  }
  if (comps.empty()) {
    comps.assign(static_cast<std::size_t>(std::max(1, cs.components)), 0.5);
  }
  return Paint::solid(resolve_color(cs, comps));
}

void Interpreter::do_shfill(const Object& shading, const Matrix& space) {
  std::optional<Paint> paint = shading_paint(shading, space);
  if (!paint) return;
  // Paint the whole clip region (bounded by the page, or the shading's BBox).
  Path area;
  const Rect r = page_rect_device();
  const double pad = std::max(r.width(), r.height());
  add_rect(area, r.x0 - pad, r.y0 - pad, r.width() + 2 * pad, r.height() + 2 * pad);
  if (const Object* bb = dict_find(shading, "BBox"); bb != nullptr && bb->type == Type::Array && bb->size == 4) {
    const std::vector<double> b = numbers_of(*this, bb);
    if (b.size() == 4) {
      Path box;
      add_rect(box, std::min(b[0], b[2]), std::min(b[1], b[3]), std::abs(b[2] - b[0]), std::abs(b[3] - b[1]));
      area = transformed(box, space);
    }
  }
  emit_fill_paint(area, FillRule::NonZero, *paint);
}

}  // namespace cvt::vector::ps
