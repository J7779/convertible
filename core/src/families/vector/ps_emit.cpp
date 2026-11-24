// Convertible core - vector family: helpers for the PostScript and PDF writers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/vector/ps_emit.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "families/image/image_api.hpp"
#include "families/vector/geom.hpp"

namespace cvt::vector::emit {

std::string num(double value, int precision) {
  if (!std::isfinite(value)) return "0";
  precision = std::clamp(precision, 0, 9);
  const double scale = std::pow(10.0, precision);
  double r = std::round(value * scale) / scale;
  if (r == 0) r = 0;  // no "-0"
  if (std::abs(r) >= 1e15) r = std::copysign(1e15, r);
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), r, std::chars_format::fixed, precision);
  std::string s(buf, res.ptr);
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0" || s.empty()) s = "0";
  return s;
}

std::string matrix_array(const Matrix& m, int precision) {
  const int lp = std::max(precision, 6);
  return "[" + num(m.a, lp) + " " + num(m.b, lp) + " " + num(m.c, lp) + " " + num(m.d, lp) + " " +
         num(m.e, precision) + " " + num(m.f, precision) + "]";
}

std::string ascii85(ByteView data) {
  std::string out;
  out.reserve(data.size() * 5 / 4 + data.size() / 72 + 8);
  std::size_t line = 0;
  auto put = [&](char c) {
    out.push_back(c);
    if (++line >= 72) {
      out.push_back('\n');
      line = 0;
    }
  };
  std::size_t i = 0;
  while (i + 4 <= data.size()) {
    const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 24) | (static_cast<std::uint32_t>(data[i + 1]) << 16) |
                            (static_cast<std::uint32_t>(data[i + 2]) << 8) | static_cast<std::uint32_t>(data[i + 3]);
    i += 4;
    if (v == 0) {
      put('z');
      continue;
    }
    char c[5];
    std::uint32_t t = v;
    for (int k = 4; k >= 0; --k) {
      c[k] = static_cast<char>('!' + t % 85);
      t /= 85;
    }
    for (const char ch : c) put(ch);
  }
  const std::size_t rest = data.size() - i;
  if (rest > 0) {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | (k < rest ? static_cast<std::uint32_t>(data[i + k]) : 0u);
    char c[5];
    for (int k = 4; k >= 0; --k) {
      c[k] = static_cast<char>('!' + v % 85);
      v /= 85;
    }
    for (std::size_t k = 0; k < rest + 1; ++k) put(c[k]);
  }
  out += "~>";
  return out;
}

Bytes lzw_encode(ByteView data) {
  Bytes out;
  std::uint32_t buffer = 0;
  int bits_in = 0;
  int width = 9;
  auto emit_code = [&](std::uint32_t code) {
    buffer = (buffer << width) | code;
    bits_in += width;
    while (bits_in >= 8) {
      out.push_back(std::byte{static_cast<unsigned char>((buffer >> (bits_in - 8)) & 0xFF)});
      bits_in -= 8;
    }
    buffer &= (1u << bits_in) - 1u;
  };
  std::unordered_map<std::uint32_t, std::uint32_t> table;  // (prefix code << 8 | byte) -> code
  std::uint32_t next = 258;
  emit_code(256);
  if (data.empty()) {
    emit_code(257);
  } else {
    std::uint32_t prefix = static_cast<std::uint32_t>(data[0]);
    for (std::size_t i = 1; i < data.size(); ++i) {
      const std::uint32_t c = static_cast<std::uint32_t>(data[i]);
      const std::uint32_t key = (prefix << 8) | c;
      if (const auto it = table.find(key); it != table.end()) {
        prefix = it->second;
        continue;
      }
      emit_code(prefix);
      table.emplace(key, next++);
      // EarlyChange 1: widen one code early.
      if (next + 1 > (1u << width) && width < 12) ++width;
      if (next >= 4094) {
        emit_code(256);
        table.clear();
        next = 258;
        width = 9;
      }
      prefix = c;
    }
    emit_code(prefix);
    ++next;
    if (next + 1 > (1u << width) && width < 12) ++width;
    emit_code(257);
  }
  if (bits_in > 0) out.push_back(std::byte{static_cast<unsigned char>((buffer << (8 - bits_in)) & 0xFF)});
  return out;
}

std::string ps_string(std::string_view bytes) {
  std::string out = "(";
  for (const char ch : bytes) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '(' || c == ')' || c == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (c < 32 || c >= 127) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\%03o", static_cast<unsigned>(c));
      out += buf;
    } else {
      out.push_back(ch);
    }
  }
  out.push_back(')');
  return out;
}

Rect group_content_bounds(const Group& group, const Matrix& m) {
  Rect r;
  for (const Node& n : group.children) r.add(node_bounds(n, m));
  return r;
}

Rect node_bounds(const Node& node, const Matrix& m) {
  if (const auto* s = std::get_if<Shape>(&node.value)) {
    Rect r = bounds(s->path, m);
    if (s->stroke.paint.type != Paint::Type::None && !r.empty()) {
      const double pad = s->stroke.width * std::max(1.0, s->stroke.miter_limit * 0.5) * m.mean_scale() + 1;
      r = Rect{r.x0 - pad, r.y0 - pad, r.x1 + pad, r.y1 + pad};
    }
    return r;
  }
  if (const auto* t = std::get_if<Text>(&node.value)) {
    Rect r = bounds(t->outline, m);
    if (r.empty()) {
      // Text without an outline: estimate from the font size.
      const Point o = m.apply(t->origin);
      const double s = t->font.size * m.mean_scale();
      r = Rect{o.x, o.y - s, o.x + std::max(t->advance * m.mean_scale(), s), o.y + s * 0.3};
    }
    return r;
  }
  if (const auto* p = std::get_if<Picture>(&node.value)) return transform_rect(p->bounds, m);
  if (const auto* g = std::get_if<Group>(&node.value)) {
    if (!g->visible) return {};
    const Matrix inner = m * g->transform;
    Rect r = group_content_bounds(*g, inner);
    if (g->filter) {
      if (!g->filter->region.empty()) {
        r = transform_rect(g->filter->region, inner);
      } else {
        double pad = 0;
        for (const FilterPrimitive& f : g->filter->primitives) {
          pad = std::max(pad, 3 * std::max(f.std_dev_x, f.std_dev_y) + std::max(std::abs(f.dx), std::abs(f.dy)));
        }
        pad *= inner.mean_scale();
        if (!r.empty()) r = Rect{r.x0 - pad, r.y0 - pad, r.x1 + pad, r.y1 + pad};
      }
    }
    if (g->clip) {
      Rect c;
      for (const ClipPath::Item& item : g->clip->items) c.add(bounds(item.path, inner));
      if (!c.empty()) r = r.intersect(c);
    }
    return r;
  }
  return {};
}

bool paint_translucent(const Paint& paint) {
  if (paint.type == Paint::Type::None) return false;
  if (paint.opacity < 0.999) return true;
  if (paint.type == Paint::Type::Solid) return paint.color.a < 0.999f;
  if (paint.type == Paint::Type::Gradient && paint.gradient) {
    for (const GradientStop& s : paint.gradient->stops) {
      if (s.color.a < 0.999f) return true;
    }
  }
  return false;
}

bool node_translucent(const Node& node) {
  if (const auto* s = std::get_if<Shape>(&node.value)) {
    return paint_translucent(s->fill) || paint_translucent(s->stroke.paint);
  }
  if (const auto* t = std::get_if<Text>(&node.value)) return paint_translucent(t->fill) || paint_translucent(t->stroke.paint);
  if (const auto* p = std::get_if<Picture>(&node.value)) return p->pixels && p->pixels->has_transparency();
  return false;
}

bool group_needs_effects(const Group& group) {
  return group.opacity < 0.999 || group.mask != nullptr || group.filter != nullptr || group.blend != BlendMode::Normal;
}

std::vector<GradientStop> normalized_stops(const Gradient& gradient) {
  std::vector<GradientStop> stops = gradient.stops;
  if (stops.empty()) stops.push_back(GradientStop{0, Color{0, 0, 0, 1}});
  double last = 0;
  for (GradientStop& s : stops) {
    s.offset = std::clamp(std::isfinite(s.offset) ? s.offset : 0.0, 0.0, 1.0);
    s.offset = std::max(s.offset, last);
    last = s.offset;
  }
  if (stops.front().offset > 0) stops.insert(stops.begin(), GradientStop{0, stops.front().color});
  if (stops.back().offset < 1) stops.push_back(GradientStop{1, stops.back().color});
  if (stops.size() == 1) stops.push_back(GradientStop{1, stops[0].color});
  return stops;
}

Group prefix_copy(const Group& root, std::span<const std::size_t> path) {
  Group out;
  out.transform = root.transform;
  out.opacity = root.opacity;
  out.blend = root.blend;
  out.clip = root.clip;
  out.mask = root.mask;
  out.filter = root.filter;
  out.id = root.id;
  out.layer = root.layer;
  out.layer_name = root.layer_name;
  out.visible = root.visible;
  if (path.empty()) return out;
  const std::size_t idx = std::min(path[0], root.children.size());
  for (std::size_t i = 0; i < idx; ++i) out.children.push_back(root.children[i]);
  if (idx < root.children.size()) {
    if (path.size() == 1) {
      out.children.push_back(root.children[idx]);
    } else if (const auto* g = std::get_if<Group>(&root.children[idx].value)) {
      out.children.push_back(Node{prefix_copy(*g, path.subspan(1))});
    }
  }
  return out;
}

Bytes picture_samples(const ir::Frame& frame, int& components) {
  bool gray = true;
  const std::size_t n = static_cast<std::size_t>(frame.width) * frame.height;
  for (std::size_t i = 0; i < n && gray; ++i) {
    const std::uint8_t* p = frame.rgba.data() + i * 4;
    gray = p[0] == p[1] && p[1] == p[2];
  }
  components = gray ? 1 : 3;
  Bytes out;
  out.reserve(n * static_cast<std::size_t>(components));
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t* p = frame.rgba.data() + i * 4;
    const int a = p[3];
    for (int k = 0; k < components; ++k) {
      const int v = (p[k] * a + 255 * (255 - a) + 127) / 255;  // over white paper
      out.push_back(std::byte{static_cast<unsigned char>(v)});
    }
  }
  return out;
}

int jpeg_components(ByteView jpeg, std::uint32_t* width, std::uint32_t* height) {
  std::size_t i = 2;
  if (jpeg.size() < 4 || jpeg[0] != std::byte{0xFF} || jpeg[1] != std::byte{0xD8}) return 0;
  while (i + 4 <= jpeg.size()) {
    if (jpeg[i] != std::byte{0xFF}) return 0;
    const int marker = static_cast<int>(jpeg[i + 1]);
    if (marker == 0xFF) {
      ++i;
      continue;
    }
    const std::size_t len = (static_cast<std::size_t>(jpeg[i + 2]) << 8) | static_cast<std::size_t>(jpeg[i + 3]);
    if ((marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) || (marker >= 0xC9 && marker <= 0xCB) ||
        (marker >= 0xCD && marker <= 0xCF)) {
      if (i + 9 >= jpeg.size()) return 0;
      if (height != nullptr) *height = (static_cast<std::uint32_t>(jpeg[i + 5]) << 8) | static_cast<std::uint32_t>(jpeg[i + 6]);
      if (width != nullptr) *width = (static_cast<std::uint32_t>(jpeg[i + 7]) << 8) | static_cast<std::uint32_t>(jpeg[i + 8]);
      return static_cast<int>(jpeg[i + 9]);
    }
    if (marker == 0xDA || marker == 0xD9) return 0;
    i += 2 + len;
  }
  return 0;
}

const ir::Frame* picture_frame(const Picture& pic, const Context& ctx, ir::Frame& storage) {
  if (pic.pixels && pic.pixels->valid()) return pic.pixels.get();
  if (pic.encoded.empty() || pic.encoded_format.empty() || !image::can_decode(pic.encoded_format, ctx)) return nullptr;
  auto img = image::decode(pic.encoded_format, pic.encoded, ctx);
  if (!img || img->frames.empty() || !img->frames[0].valid()) return nullptr;
  storage = std::move(img->frames[0]);
  return &storage;
}

}  // namespace cvt::vector::emit
