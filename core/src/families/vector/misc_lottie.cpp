// Convertible core - vector family: Lottie / Bodymovin animations (read only).
//
// A static frame (option `frame`, default the first frame) becomes a scene:
// shape layers (groups, paths with tangents, rectangles, ellipses, stars and
// polygons, fills, strokes with dashes, linear/radial gradients, transforms,
// trim paths, basic round corners), layer transforms with keyframed values
// (linear, hold and cubic-bezier easing), parenting, precompositions (with
// a recursion limit), solid, image (data URIs or .lottie members) and basic
// text layers, and masks approximated by clip paths. The `.lottie` (dotLottie)
// ZIP container is read too. A second Image reader renders several frames
// into an animation (lottie -> gif / apng).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/image_api.hpp"
#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/misc_common.hpp"
#include "families/vector/render.hpp"
#include "families/vector/vector_codecs.hpp"
#include "util/base64.hpp"
#include "util/json.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::vector::misc {

namespace {

constexpr double deg = std::numbers::pi / 180.0;
constexpr int max_precomp_depth = 16;
constexpr int max_group_depth = 64;
constexpr std::size_t max_items = 2'000'000;
constexpr std::size_t max_vertices = 4'000'000;

// --------------------------------------------------------------------------- detection

bool is_zip(std::span<const std::byte> data) {
  return data.size() >= 4 && data[0] == std::byte{'P'} && data[1] == std::byte{'K'} && data[2] == std::byte{3} &&
         data[3] == std::byte{4};
}

int sniff_lottie(std::span<const std::byte> data, std::string_view) {
  const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
  if (is_zip(data)) {
    // dotLottie: a ZIP with manifest.json and animations/*.json.
    return text.find("manifest.json") != std::string_view::npos && text.find("animations/") != std::string_view::npos
               ? 92
               : 0;
  }
  std::size_t i = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
  if (i >= text.size() || text[i] != '{') return 0;
  for (const std::string_view key : {"\"v\"", "\"fr\"", "\"ip\"", "\"op\"", "\"w\"", "\"h\"", "\"layers\""}) {
    if (text.find(key) == std::string_view::npos) return 0;
  }
  return 93;
}

// --------------------------------------------------------------------------- property evaluation

double cubic_coordinate(double a, double b, double t) {
  // Bézier from 0 through control points a, b to 1.
  const double u = 1 - t;
  return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

/// Easing curve value for progress x with control points (x1,y1), (x2,y2).
double ease(double x1, double y1, double x2, double y2, double x) {
  x = std::clamp(x, 0.0, 1.0);
  x1 = std::clamp(x1, 0.0, 1.0);
  x2 = std::clamp(x2, 0.0, 1.0);
  if (!std::isfinite(y1) || !std::isfinite(y2)) return x;
  double lo = 0, hi = 1, t = x;
  for (int k = 0; k < 40; ++k) {
    const double v = cubic_coordinate(x1, x2, t);
    if (std::abs(v - x) < 1e-7) break;
    if (v < x) lo = t;
    else hi = t;
    t = (lo + hi) / 2;
  }
  return cubic_coordinate(y1, y2, t);
}

double tangent_component(const Json& tangent, const char* axis, std::size_t dim, double fallback) {
  const Json& v = tangent[axis];
  if (v.is_number()) return v.as_double(fallback);
  if (v.is_array() && !v.empty()) return v[std::min(dim, v.size() - 1)].as_double(fallback);
  return fallback;
}

std::vector<double> numbers_of(const Json& value) {
  std::vector<double> out;
  if (value.is_number()) {
    out.push_back(value.as_double());
  } else if (value.is_array()) {
    for (const Json& e : value.elements()) {
      if (!e.is_number()) break;
      out.push_back(e.as_double());
      if (out.size() >= 4096) break;
    }
  }
  for (double& d : out) {
    if (!std::isfinite(d)) d = 0.0;
  }
  return out;
}

bool is_keyframes(const Json& k) { return k.is_array() && !k.empty() && k[0].is_object() && k[0].contains("t"); }

/// Find the keyframe segment for `frame`: (index of start keyframe, progress, hold).
struct Segment {
  std::size_t index = 0;
  double progress = 0.0;
  bool past_end = false;
  bool before_start = false;
};

Segment locate(std::span<const Json> keys, double frame) {
  Segment s;
  if (keys.empty() || frame <= keys[0]["t"].as_double()) {
    s.before_start = true;
    return s;
  }
  for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
    const double t0 = keys[i]["t"].as_double(), t1 = keys[i + 1]["t"].as_double();
    if (frame < t1) {
      s.index = i;
      s.progress = t1 > t0 ? (frame - t0) / (t1 - t0) : 1.0;
      return s;
    }
  }
  s.index = keys.size() - 1;
  s.past_end = true;
  return s;
}

/// Value of a (possibly animated) numeric property at `frame`.
std::vector<double> value_at(const Json& prop, double frame) {
  if (!prop.is_object()) return numbers_of(prop);
  if (prop["s"].as_bool(false) && (prop.contains("x") || prop.contains("y"))) {  // split position
    std::vector<double> out = {0, 0};
    const auto x = value_at(prop["x"], frame), y = value_at(prop["y"], frame);
    if (!x.empty()) out[0] = x[0];
    if (!y.empty()) out[1] = y[0];
    return out;
  }
  const Json& k = prop["k"];
  if (!is_keyframes(k)) return numbers_of(k);
  const auto keys = k.elements();
  const Segment seg = locate(keys, frame);
  const Json& a = keys[seg.index];
  if (seg.before_start) return numbers_of(a.contains("s") ? a["s"] : a["e"]);
  if (seg.past_end) {
    if (a.contains("s")) return numbers_of(a["s"]);
    if (seg.index > 0) return numbers_of(keys[seg.index - 1]["e"]);
    return {};
  }
  const Json& b = keys[seg.index + 1];
  std::vector<double> start = numbers_of(a["s"]);
  std::vector<double> end = a.contains("e") ? numbers_of(a["e"]) : numbers_of(b.contains("s") ? b["s"] : a["s"]);
  if (a["h"].as_int64(0) == 1 || end.size() != start.size()) return start;
  for (std::size_t d = 0; d < start.size(); ++d) {
    const double e = ease(tangent_component(a["o"], "x", d, 0.0), tangent_component(a["o"], "y", d, 0.0),
                          tangent_component(a["i"], "x", d, 1.0), tangent_component(a["i"], "y", d, 1.0), seg.progress);
    start[d] += (end[d] - start[d]) * e;
  }
  return start;
}

double scalar_at(const Json& prop, double frame, double fallback) {
  if (prop.is_null()) return fallback;
  const auto v = value_at(prop, frame);
  return v.empty() ? fallback : v[0];
}

Point point_at(const Json& prop, double frame, Point fallback) {
  if (prop.is_null()) return fallback;
  const auto v = value_at(prop, frame);
  if (v.size() < 2) return v.size() == 1 ? Point{v[0], v[0]} : fallback;
  return {v[0], v[1]};
}

Color color_at(const Json& prop, double frame) {
  auto v = value_at(prop, frame);
  if (v.size() < 3) return Color{0, 0, 0, 1};
  if (v[0] > 1.0 || v[1] > 1.0 || v[2] > 1.0) {  // some exporters write 0..255
    for (std::size_t i = 0; i < std::min<std::size_t>(v.size(), 4); ++i) v[i] /= 255.0;
  }
  const auto f = [](double x) { return static_cast<float>(std::clamp(x, 0.0, 1.0)); };
  return Color{f(v[0]), f(v[1]), f(v[2]), v.size() >= 4 ? f(v[3]) : 1.0f};
}

// --------------------------------------------------------------------------- bezier shapes

struct Bezier {
  std::vector<Point> v, in, out;
  bool closed = false;
};

std::vector<Point> points_of(const Json& array) {
  std::vector<Point> out;
  for (const Json& p : array.elements()) {
    if (out.size() >= max_vertices) break;
    const double x = p[0].as_double(), y = p[1].as_double();
    out.push_back({std::isfinite(x) ? x : 0.0, std::isfinite(y) ? y : 0.0});
  }
  return out;
}

Bezier bezier_of(const Json& shape) {
  Bezier b;
  b.v = points_of(shape["v"]);
  b.in = points_of(shape["i"]);
  b.out = points_of(shape["o"]);
  b.closed = shape["c"].as_bool(false);
  b.in.resize(b.v.size());
  b.out.resize(b.v.size());
  return b;
}

const Json& shape_value(const Json& k) {
  // A static shape is an object; inside keyframes it is wrapped in an array.
  if (k.is_array() && !k.empty()) return k[0];
  return k;
}

Bezier bezier_at(const Json& prop, double frame) {
  const Json& k = prop["k"];
  if (!is_keyframes(k)) return bezier_of(shape_value(k));
  const auto keys = k.elements();
  const Segment seg = locate(keys, frame);
  const Json& a = keys[seg.index];
  if (seg.before_start || seg.past_end) {
    if (a.contains("s")) return bezier_of(shape_value(a["s"]));
    if (seg.index > 0) return bezier_of(shape_value(keys[seg.index - 1]["e"]));
    return {};
  }
  const Json& b = keys[seg.index + 1];
  Bezier start = bezier_of(shape_value(a["s"]));
  const Bezier end = bezier_of(shape_value(a.contains("e") ? a["e"] : b.contains("s") ? b["s"] : a["s"]));
  if (a["h"].as_int64(0) == 1 || end.v.size() != start.v.size()) return start;
  const double e = ease(tangent_component(a["o"], "x", 0, 0.0), tangent_component(a["o"], "y", 0, 0.0),
                        tangent_component(a["i"], "x", 0, 1.0), tangent_component(a["i"], "y", 0, 1.0), seg.progress);
  for (std::size_t i = 0; i < start.v.size(); ++i) {
    start.v[i] = lerp(start.v[i], end.v[i], e);
    start.in[i] = lerp(start.in[i], end.in[i], e);
    start.out[i] = lerp(start.out[i], end.out[i], e);
  }
  return start;
}

void append_bezier(Path& path, const Bezier& b, bool absolute_tangents = false) {
  if (b.v.empty()) return;
  const auto out_point = [&](std::size_t i) { return absolute_tangents ? b.out[i] : b.v[i] + b.out[i]; };
  const auto in_point = [&](std::size_t i) { return absolute_tangents ? b.in[i] : b.v[i] + b.in[i]; };
  const auto segment = [&](std::size_t from, std::size_t to) {
    const Point c1 = out_point(from), c2 = in_point(to);
    if (c1 == b.v[from] && c2 == b.v[to]) path.line_to(b.v[to]);
    else path.cubic_to(c1, c2, b.v[to]);
  };
  path.move_to(b.v[0]);
  for (std::size_t i = 1; i < b.v.size(); ++i) segment(i - 1, i);
  if (b.closed) {
    if (b.v.size() > 1) segment(b.v.size() - 1, 0);
    path.close();
  }
}

/// Lottie star / polygon, as lottie-web builds it.
Path star_path(const Json& item, double frame) {
  const bool polygon = item["sy"].as_int64(1) == 2;
  const double points = std::floor(std::clamp(scalar_at(item["pt"], frame, 5), 0.0, 1000.0));
  const Point p = point_at(item["p"], frame, {0, 0});
  const double rotation = scalar_at(item["r"], frame, 0) * deg;
  const double outer = scalar_at(item["or"], frame, 0), inner = scalar_at(item["ir"], frame, 0);
  const double outer_round = scalar_at(item["os"], frame, 0) / 100.0, inner_round = scalar_at(item["is"], frame, 0) / 100.0;
  const double dir = item["d"].as_int64(1) == 3 ? -1.0 : 1.0;
  Bezier b;
  b.closed = true;
  if (points < 1) return {};
  const int count = static_cast<int>(polygon ? points : points * 2);
  const double angle = 2 * std::numbers::pi / count;
  double current = -std::numbers::pi / 2 + rotation;
  bool long_flag = true;
  for (int i = 0; i < count; ++i) {
    const double rad = polygon || long_flag ? outer : inner;
    const double roundness = polygon || long_flag ? outer_round : inner_round;
    const double perimeter = polygon ? 2 * std::numbers::pi * rad / (count * 2.0) : 2 * std::numbers::pi * rad / (count * 2.0);
    const double x = rad * std::cos(current), y = rad * std::sin(current);
    const double norm = std::hypot(x, y);
    const double ox = norm == 0 ? 0 : y / norm, oy = norm == 0 ? 0 : -x / norm;
    const Point v = p + Point{x, y};
    b.v.push_back(v);
    b.out.push_back(v - Point{ox, oy} * (perimeter * roundness * dir));
    b.in.push_back(v + Point{ox, oy} * (perimeter * roundness * dir));
    long_flag = !long_flag;
    current += angle * dir;
  }
  Path path;
  append_bezier(path, b, true);
  return path;
}

/// lottie-web transform: translate(p) rotate(r) skew scale translate(-a).
Matrix transform_of(const Json& tr, double frame) {
  if (!tr.is_object()) return {};
  const Point a = point_at(tr["a"], frame, {0, 0});
  const Point p = point_at(tr["p"], frame, {0, 0});
  const Point s = point_at(tr["s"], frame, {100, 100});
  const double r = scalar_at(tr.contains("r") ? tr["r"] : tr["rz"], frame, 0);
  const double sk = scalar_at(tr["sk"], frame, 0);
  const double sa = scalar_at(tr["sa"], frame, 0);
  Matrix m = Matrix::translate(p.x, p.y) * Matrix::rotate(r * deg);
  if (sk != 0.0 && std::abs(sk) < 89.0) {
    m = m * Matrix::rotate(sa * deg) * Matrix::skew_x(-sk * deg) * Matrix::rotate(-sa * deg);
  }
  return m * Matrix::scale(s.x / 100.0, s.y / 100.0) * Matrix::translate(-a.x, -a.y);
}

/// Round corners: replace sharp corners of straight segments by quadratic curves.
Path round_corners(const Path& path, double radius) {
  if (!(radius > 0)) return path;
  Path out;
  std::size_t i = 0;
  std::vector<Point> poly;
  bool closed = false;
  const auto flush = [&] {
    if (poly.size() < 2) {
      if (!poly.empty()) out.move_to(poly[0]);
      poly.clear();
      return;
    }
    const std::size_t n = poly.size();
    const auto corner = [&](std::size_t k, Point& before, Point& after) {
      const Point prev = poly[(k + n - 1) % n], cur = poly[k], next = poly[(k + 1) % n];
      const double l1 = length(prev - cur), l2 = length(next - cur);
      const double r = std::min({radius, l1 / 2, l2 / 2});
      before = l1 > 0 ? cur + (prev - cur) * (r / l1) : cur;
      after = l2 > 0 ? cur + (next - cur) * (r / l2) : cur;
    };
    if (closed) {
      Point b0, a0;
      corner(0, b0, a0);
      out.move_to(a0);
      for (std::size_t k = 1; k < n; ++k) {
        Point b, a;
        corner(k, b, a);
        out.line_to(b);
        out.quad_to(poly[k], a);
      }
      out.line_to(b0);
      out.quad_to(poly[0], a0);
      out.close();
    } else {
      out.move_to(poly[0]);
      for (std::size_t k = 1; k + 1 < n; ++k) {
        Point b, a;
        corner(k, b, a);
        out.line_to(b);
        out.quad_to(poly[k], a);
      }
      out.line_to(poly[n - 1]);
    }
    poly.clear();
  };
  bool has_curves = false;
  for (const Verb v : path.verbs) has_curves = has_curves || v == Verb::Cubic || v == Verb::Quad;
  if (has_curves) return path;  // only polygons are rounded (basic support)
  for (const Verb v : path.verbs) {
    switch (v) {
      case Verb::Move:
        flush();
        closed = false;
        poly.push_back(path.points[i++]);
        break;
      case Verb::Line:
        poly.push_back(path.points[i++]);
        break;
      case Verb::Close:
        if (poly.size() > 1 && poly.back() == poly.front()) poly.pop_back();
        closed = true;
        flush();
        closed = false;
        break;
      default:
        break;
    }
  }
  flush();
  return out;
}

// --------------------------------------------------------------------------- the builder

struct Draw {
  std::size_t index = 0;  // position in the item list (earlier = on top)
  Node node;
};

class LottieBuilder {
 public:
  LottieBuilder(ReadContext& ctx, const Json& root, util::ZipReader* zip) : ctx_(ctx), root_(root), zip_(zip) {
    for (const Json& asset : root["assets"].elements()) {
      const std::string& id = asset["id"].as_string();
      if (!id.empty()) assets_.emplace(id, &asset);
    }
    frame_rate_ = root["fr"].as_double(30.0);
    if (!(frame_rate_ > 0) || !std::isfinite(frame_rate_)) frame_rate_ = 30.0;
  }

  Result<Scene> build(double frame) {
    const double w = root_["w"].as_double(0), h = root_["h"].as_double(0);
    if (!(w > 0) || !(h > 0) || !std::isfinite(w) || !std::isfinite(h) || w > 1e6 || h > 1e6) {
      return fail(Status::Decode, "the Lottie animation has no valid size");
    }
    Scene scene;
    Page page;
    page.width = w;
    page.height = h;
    CVT_TRY_ASSIGN(page.root, build_layers(root_["layers"], frame, 0));
    const std::string& name = root_["nm"].as_string();
    if (!name.empty()) scene.metadata["title"] = name;
    scene.pages.push_back(std::move(page));
    return scene;
  }

 private:
  ReadContext& ctx_;
  const Json& root_;
  util::ZipReader* zip_;
  std::map<std::string, const Json*> assets_;
  std::map<std::string, std::shared_ptr<const ir::Frame>> pictures_;
  double frame_rate_ = 30.0;
  std::size_t items_ = 0;
  bool warned_matte_ = false, warned_effects_ = false, warned_unsupported_ = false, warned_image_ = false,
       warned_merge_ = false, warned_text_ = false;

  void warn_once(bool& flag, std::string message) {
    if (flag) return;
    flag = true;
    ctx_.warn(std::move(message));
  }

  Result<void> count() {
    if (++items_ > max_items) return fail(Status::Limit, "the Lottie animation has too many elements");
    if ((items_ & 1023) == 0) CVT_TRY(ctx_.checkpoint(0.5, "Reading Lottie"));
    return {};
  }

  // ---- layers

  /// The layer's transform including its parents (parent transforms only, no opacity).
  Matrix layer_matrix(const Json& layer, const std::map<std::int64_t, const Json*>& by_index, double frame) {
    Matrix m = transform_of(layer["ks"], frame - layer["st"].as_double(0));
    const Json* current = &layer;
    for (int guard = 0; guard < 64; ++guard) {
      if (!current->contains("parent")) break;
      const auto it = by_index.find((*current)["parent"].as_int64(-1));
      if (it == by_index.end() || it->second == current) break;
      current = it->second;
      m = transform_of((*current)["ks"], frame - (*current)["st"].as_double(0)) * m;
    }
    return m;
  }

  Result<Group> build_layers(const Json& layers, double frame, int depth) {
    if (depth > max_precomp_depth) return fail(Status::Limit, "Lottie precompositions are nested too deeply");
    Group group;
    std::map<std::int64_t, const Json*> by_index;
    for (const Json& layer : layers.elements()) {
      if (layer.contains("ind")) by_index.emplace(layer["ind"].as_int64(), &layer);
    }
    const auto list = layers.elements();
    // The first layer is drawn on top.
    for (std::size_t n = list.size(); n-- > 0;) {
      CVT_TRY(count());
      const Json& layer = list[n];
      if (!layer.is_object() || layer["hd"].as_bool(false)) continue;
      if (layer["td"].as_int64(0) != 0) {
        warn_once(warned_matte_, "Track mattes are not supported; matte layers were left out.");
        continue;
      }
      if (layer.contains("tt")) warn_once(warned_matte_, "Track mattes are not supported; matte layers were left out.");
      if (layer.contains("ef") && !layer["ef"].empty()) warn_once(warned_effects_, "Layer effects were ignored.");
      const double ip = layer.contains("ip") ? layer["ip"].as_double(0) : -1e300;
      const double op = layer.contains("op") ? layer["op"].as_double(0) : 1e300;
      if (frame < ip || frame >= op) continue;
      const double local = frame - layer["st"].as_double(0);
      Group content;
      content.transform = layer_matrix(layer, by_index, frame);
      content.opacity = std::clamp(scalar_at(layer["ks"]["o"], local, 100.0) / 100.0, 0.0, 1.0);
      const std::string& name = layer["nm"].as_string();
      if (!name.empty()) content.id = name;
      switch (layer["ty"].as_int64(-1)) {
        case 0: {  // precomp
          const auto it = assets_.find(layer["refId"].as_string());
          if (it == assets_.end()) break;
          double child_frame = local / std::max(1e-6, std::abs(layer["sr"].as_double(1.0)));
          if (layer.contains("tm")) child_frame = scalar_at(layer["tm"], local, 0.0) * frame_rate_;
          CVT_TRY_ASSIGN(Group inner, build_layers((*it->second)["layers"], child_frame, depth + 1));
          const double pw = layer["w"].as_double(0), ph = layer["h"].as_double(0);
          if (pw > 0 && ph > 0) {
            auto clip = std::make_shared<ClipPath>();
            ClipPath::Item item;
            add_rect(item.path, 0, 0, pw, ph);
            clip->items.push_back(std::move(item));
            inner.clip = std::move(clip);
          }
          content.children.push_back(Node{std::move(inner)});
          break;
        }
        case 1: {  // solid
          Shape solid;
          add_rect(solid.path, 0, 0, layer["sw"].as_double(0), layer["sh"].as_double(0));
          solid.fill = Paint::solid(parse_android_color(layer["sc"].as_string()).value_or(Color{0, 0, 0, 1}));
          content.children.push_back(Node{std::move(solid)});
          break;
        }
        case 2: {  // image
          CVT_TRY(add_image(layer, content));
          break;
        }
        case 4: {  // shapes
          std::vector<Path> geometry;
          CVT_TRY_ASSIGN(std::vector<Draw> draws, build_items(layer["shapes"], local, 0, geometry));
          for (std::size_t k = draws.size(); k-- > 0;) content.children.push_back(std::move(draws[k].node));
          break;
        }
        case 5:  // text
          add_text(layer, local, content);
          break;
        default:  // null, camera, audio ...
          break;
      }
      apply_masks(layer, local, content);
      group.children.push_back(Node{std::move(content)});
    }
    return group;
  }

  void apply_masks(const Json& layer, double frame, Group& content) {
    const Json& masks = layer["masksProperties"];
    if (masks.empty() || !layer["hasMask"].as_bool(true)) return;
    // Masks live in layer space; content.transform maps layer space to the
    // parent, so the clip goes on an inner group.
    std::shared_ptr<ClipPath> chain;
    ClipPath* tail = nullptr;
    const auto push = [&](ClipPath clip) {
      auto node = std::make_shared<ClipPath>(std::move(clip));
      if (!chain) {
        chain = node;
      } else {
        tail->clip = node;
      }
      tail = node.get();
    };
    Path big;
    add_rect(big, -1e6, -1e6, 2e6, 2e6);
    ClipPath adds;
    bool any = false;
    for (const Json& mask : masks.elements()) {
      const std::string& mode = mask["mode"].as_string();
      if (mode == "n") continue;
      Path path;
      append_bezier(path, bezier_at(mask["pt"], frame));
      const bool inverted = mask["inv"].as_bool(false);
      if (mode == "a" || mode.empty()) {
        if (inverted) {
          Path p = big;
          append(p, path);
          push(ClipPath{{ClipPath::Item{std::move(p), FillRule::EvenOdd}}, nullptr});
        } else {
          adds.items.push_back({std::move(path), FillRule::NonZero});
        }
        any = true;
      } else if (mode == "s") {
        if (inverted) {
          push(ClipPath{{ClipPath::Item{std::move(path), FillRule::NonZero}}, nullptr});
        } else {
          Path p = big;
          append(p, path);
          push(ClipPath{{ClipPath::Item{std::move(p), FillRule::EvenOdd}}, nullptr});
        }
        any = true;
      } else if (mode == "i") {
        push(ClipPath{{ClipPath::Item{std::move(path), FillRule::NonZero}}, nullptr});
        any = true;
      } else {
        warn_once(warned_unsupported_, std::format("Mask mode '{}' was approximated.", mode));
      }
    }
    if (!any) return;
    if (!adds.items.empty()) {
      auto first = std::make_shared<ClipPath>(std::move(adds));
      first->clip = chain;
      chain = first;
    }
    Group inner;
    inner.clip = chain;
    inner.children = std::move(content.children);
    content.children.clear();
    content.children.push_back(Node{std::move(inner)});
  }

  Result<void> add_image(const Json& layer, Group& content) {
    const auto it = assets_.find(layer["refId"].as_string());
    if (it == assets_.end()) return {};
    const Json& asset = *it->second;
    const std::string& key = asset["id"].as_string();
    std::shared_ptr<const ir::Frame> pixels;
    if (const auto cached = pictures_.find(key); cached != pictures_.end()) {
      pixels = cached->second;
    } else {
      const std::string& p = asset["p"].as_string();
      Bytes encoded;
      if (p.starts_with("data:")) {
        const std::size_t comma = p.find(',');
        if (comma != std::string::npos && p.substr(0, comma).find(";base64") != std::string::npos) {
          auto decoded = util::base64_decode(std::string_view(p).substr(comma + 1));
          if (decoded) encoded = std::move(*decoded);
        }
      } else if (zip_ != nullptr) {
        std::string member = asset["u"].as_string() + p;
        while (member.starts_with("/")) member.erase(0, 1);
        auto bytes = zip_->read(member);
        if (bytes) encoded = std::move(*bytes);
      }
      if (encoded.empty()) {
        warn_once(warned_image_, "An image asset could not be loaded (external files are not read).");
        return {};
      }
      const auto format = image::sniff_format(encoded);
      if (!format) {
        warn_once(warned_image_, "An image asset has an unknown format and was left out.");
        return {};
      }
      auto decoded = image::decode(*format, encoded, ctx_);
      if (!decoded || decoded->frames.empty()) {
        if (decoded.error().code == Status::Limit || decoded.error().code == Status::Cancelled) return fail(decoded.error());
        warn_once(warned_image_, "An image asset could not be decoded and was left out.");
        return {};
      }
      pixels = std::make_shared<const ir::Frame>(std::move(decoded->frames.front()));
      pictures_.emplace(key, pixels);
    }
    Picture picture;
    picture.pixels = pixels;
    const double w = asset["w"].as_double(pixels->width), h = asset["h"].as_double(pixels->height);
    picture.bounds = Rect::xywh(0, 0, w, h);
    content.children.push_back(Node{std::move(picture)});
    return {};
  }

  void add_text(const Json& layer, double frame, Group& content) {
    const Json& keys = layer["t"]["d"]["k"];
    if (keys.empty()) return;
    // Text documents are hold-keyframed: take the last one at or before the frame.
    const Json* doc = &keys[0]["s"];
    for (const Json& k : keys.elements()) {
      if (k["t"].as_double(0) <= frame) doc = &k["s"];
    }
    const std::string& text = (*doc)["t"].as_string();
    if (text.empty()) return;
    warn_once(warned_text_, "Text layers were set with the bundled fallback font.");
    FontSpec font;
    font.family = (*doc)["f"].as_string();
    font.size = std::clamp((*doc)["s"].as_double(16), 0.1, 10000.0);
    const std::string lower = util::to_lower(font.family);
    if (lower.find("bold") != std::string::npos) font.weight = 700;
    if (lower.find("italic") != std::string::npos || lower.find("oblique") != std::string::npos) font.italic = true;
    const Color color = color_at(Json::object({{"k", (*doc)["fc"]}}), 0);
    const double line_height = (*doc)["lh"].as_double(font.size * 1.2);
    const std::int64_t justify = (*doc)["j"].as_int64(0);
    double y = 0;
    std::size_t start = 0;
    for (int line = 0; line < 1000 && start <= text.size(); ++line) {
      std::size_t end = text.find_first_of("\r\n\x03", start);
      if (end == std::string::npos) end = text.size();
      const std::string_view piece = std::string_view(text).substr(start, end - start);
      Text t = make_text(piece, font, {0, y}, Paint::solid(color));
      const double shift = justify == 1 ? -t.advance : justify == 2 ? -t.advance / 2 : 0.0;
      if (shift != 0.0) {
        t.origin.x += shift;
        t.outline = transformed(t.outline, Matrix::translate(shift, 0));
      }
      content.children.push_back(Node{std::move(t)});
      y += line_height;
      start = end + 1;
    }
  }

  // ---- shape items

  Paint fill_paint(const Json& item, double frame, bool gradient) {
    const double opacity = std::clamp(scalar_at(item["o"], frame, 100.0) / 100.0, 0.0, 1.0);
    if (!gradient) return Paint::solid(color_at(item["c"], frame), opacity);
    Paint paint;
    paint.type = Paint::Type::Gradient;
    paint.opacity = opacity;
    auto g = std::make_shared<Gradient>();
    const std::int64_t count = std::clamp<std::int64_t>(item["g"]["p"].as_int64(0), 0, 1024);
    const std::vector<double> values = value_at(item["g"]["k"], frame);
    for (std::int64_t i = 0; i < count; ++i) {
      const std::size_t at = static_cast<std::size_t>(i) * 4;
      if (at + 3 >= values.size()) break;
      const auto f = [](double x) { return static_cast<float>(std::clamp(x, 0.0, 1.0)); };
      g->stops.push_back({std::clamp(values[at], 0.0, 1.0), Color{f(values[at + 1]), f(values[at + 2]), f(values[at + 3]), 1.0f}});
    }
    // Optional opacity stops after the colour stops: (offset, alpha) pairs.
    const std::size_t alpha_start = static_cast<std::size_t>(count) * 4;
    if (values.size() >= alpha_start + 2) {
      std::vector<GradientStop> alphas;
      for (std::size_t at = alpha_start; at + 1 < values.size(); at += 2) {
        alphas.push_back({values[at], Color{0, 0, 0, static_cast<float>(std::clamp(values[at + 1], 0.0, 1.0))}});
      }
      for (GradientStop& stop : g->stops) stop.color.a = gradient_color_at(alphas, stop.offset).a;
    }
    std::stable_sort(g->stops.begin(), g->stops.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    if (g->stops.empty()) return Paint::solid(Color{0, 0, 0, 1}, opacity);
    const Point s = point_at(item["s"], frame, {0, 0}), e = point_at(item["e"], frame, {0, 0});
    if (item["t"].as_int64(1) == 2) {
      g->type = Gradient::Type::Radial;
      g->center = s;
      g->radius = length(e - s);
      const double highlight = std::clamp(scalar_at(item["h"], frame, 0.0) / 100.0, -0.99, 0.99);
      const double angle = scalar_at(item["a"], frame, 0.0) * deg + std::atan2(e.y - s.y, e.x - s.x);
      g->focal = s + Point{std::cos(angle), std::sin(angle)} * (highlight * g->radius);
    } else {
      g->p1 = s;
      g->p2 = e;
    }
    paint.gradient = std::move(g);
    return paint;
  }

  Stroke stroke_of(const Json& item, double frame, bool gradient) {
    Stroke stroke;
    stroke.paint = fill_paint(item, frame, gradient);
    stroke.width = std::max(0.0, scalar_at(item["w"], frame, 1.0));
    const std::int64_t cap = item["lc"].as_int64(2), join = item["lj"].as_int64(2);
    stroke.cap = cap == 1 ? LineCap::Butt : cap == 3 ? LineCap::Square : LineCap::Round;
    stroke.join = join == 1 ? LineJoin::Miter : join == 3 ? LineJoin::Bevel : LineJoin::Round;
    stroke.miter_limit = std::max(1.0, item["ml"].as_double(4.0));
    std::vector<double> pattern;
    double offset = 0;
    for (const Json& d : item["d"].elements()) {
      const double v = std::max(0.0, scalar_at(d["v"], frame, 0.0));
      const std::string& n = d["n"].as_string();
      if (n == "o") offset = v;
      else pattern.push_back(v);
      if (pattern.size() > 64) break;
    }
    if (pattern.size() % 2 == 1) {
      const std::vector<double> copy = pattern;
      pattern.insert(pattern.end(), copy.begin(), copy.end());
    }
    double sum = 0;
    for (double v : pattern) sum += v;
    if (sum > 0) {
      stroke.dash = std::move(pattern);
      stroke.dash_offset = offset;
    }
    return stroke;
  }

  Result<std::vector<Draw>> build_items(const Json& items, double frame, int depth, std::vector<Path>& geometry) {
    if (depth > max_group_depth) return fail(Status::Limit, "Lottie shape groups are nested too deeply");
    std::vector<Draw> draws;
    const auto list = items.elements();
    const auto merged = [&] {
      Path all;
      for (const Path& p : geometry) append(all, p);
      return all;
    };
    for (std::size_t i = 0; i < list.size(); ++i) {
      CVT_TRY(count());
      const Json& item = list[i];
      if (item["hd"].as_bool(false)) continue;
      const std::string& ty = item["ty"].as_string();
      if (ty == "gr") {
        const Json* tr = nullptr;
        for (const Json& child : item["it"].elements()) {
          if (child["ty"].as_string() == "tr") tr = &child;
        }
        Group group;
        if (tr != nullptr) {
          group.transform = transform_of(*tr, frame);
          group.opacity = std::clamp(scalar_at((*tr)["o"], frame, 100.0) / 100.0, 0.0, 1.0);
        }
        const std::string& name = item["nm"].as_string();
        if (!name.empty()) group.id = name;
        std::vector<Path> inner_geometry;
        CVT_TRY_ASSIGN(std::vector<Draw> inner, build_items(item["it"], frame, depth + 1, inner_geometry));
        for (std::size_t k = inner.size(); k-- > 0;) group.children.push_back(std::move(inner[k].node));
        for (const Path& p : inner_geometry) geometry.push_back(transformed(p, group.transform));
        draws.push_back({i, Node{std::move(group)}});
      } else if (ty == "sh") {
        Path path;
        append_bezier(path, bezier_at(item["ks"], frame));
        geometry.push_back(std::move(path));
      } else if (ty == "rc") {
        const Point p = point_at(item["p"], frame, {0, 0}), s = point_at(item["s"], frame, {0, 0});
        const double r = std::max(0.0, scalar_at(item["r"], frame, 0.0));
        Path path;
        add_rect(path, p.x - s.x / 2, p.y - s.y / 2, s.x, s.y, r, r);
        geometry.push_back(std::move(path));
      } else if (ty == "el") {
        const Point p = point_at(item["p"], frame, {0, 0}), s = point_at(item["s"], frame, {0, 0});
        Path path;
        add_ellipse(path, p.x, p.y, std::abs(s.x) / 2, std::abs(s.y) / 2);
        geometry.push_back(std::move(path));
      } else if (ty == "sr") {
        geometry.push_back(star_path(item, frame));
      } else if (ty == "fl" || ty == "gf") {
        Shape shape;
        shape.path = merged();
        shape.fill = fill_paint(item, frame, ty == "gf");
        shape.fill_rule = item["r"].as_int64(1) == 2 ? FillRule::EvenOdd : FillRule::NonZero;
        if (!shape.path.empty()) draws.push_back({i, Node{std::move(shape)}});
      } else if (ty == "st" || ty == "gs") {
        Shape shape;
        shape.path = merged();
        shape.stroke = stroke_of(item, frame, ty == "gs");
        if (!shape.path.empty()) draws.push_back({i, Node{std::move(shape)}});
      } else if (ty == "tm") {
        double s = scalar_at(item["s"], frame, 0.0) / 100.0, e = scalar_at(item["e"], frame, 100.0) / 100.0;
        const double o = scalar_at(item["o"], frame, 0.0) / 360.0;
        if (s > e) std::swap(s, e);
        s = std::clamp(s, 0.0, 1.0);
        e = std::clamp(e, 0.0, 1.0);
        if (e - s < 1.0 - 1e-9) {
          const double start = (s + o) - std::floor(s + o);
          const double end = (e + o) - std::floor(e + o);
          for (Path& p : geometry) {
            if (e - s < 1e-9) {
              p.clear();
              continue;
            }
            const Rect box = control_bounds(p);
            const double tolerance = std::max(1e-3, std::max(box.width(), box.height()) * 5e-4);
            p = trim_path(p, start, end, tolerance, false);
          }
        }
      } else if (ty == "rd") {
        const double r = scalar_at(item["r"], frame, 0.0);
        for (Path& p : geometry) p = round_corners(p, r);
      } else if (ty == "mm") {
        warn_once(warned_merge_, "Merge paths were approximated by combining the shapes.");
      } else if (ty == "tr") {
        // handled by the enclosing group
      } else if (!ty.empty()) {
        warn_once(warned_unsupported_, std::format("Lottie shape item '{}' is not supported and was ignored.", ty));
      }
    }
    return draws;
  }
};

// --------------------------------------------------------------------------- entry points

struct Loaded {
  Json root;
  std::optional<util::ZipReader> zip;
};

Result<Loaded> load(ReadContext& ctx) {
  Loaded out;
  ByteView json_bytes = ctx.input;
  Bytes member;
  if (is_zip(ctx.input)) {
    auto zip = util::ZipReader::open(ctx.input, ctx.limits);
    if (!zip) return fail(Status::Decode, "not a readable dotLottie archive: {}", zip.error().message);
    std::string name;
    if (zip->contains("manifest.json")) {
      auto manifest_bytes = zip->read("manifest.json");
      if (manifest_bytes) {
        auto manifest = util::parse_json(to_string(ByteView(*manifest_bytes)), 64);
        if (manifest) {
          const std::string& id = (*manifest)["animations"][0]["id"].as_string();
          if (!id.empty() && id.find('/') == std::string::npos) name = "animations/" + id + ".json";
        }
      }
    }
    if (name.empty() || !zip->contains(name)) {
      name.clear();
      for (const util::ZipEntry& entry : zip->entries()) {
        if (entry.name.starts_with("animations/") && entry.name.ends_with(".json")) {
          name = entry.name;
          break;
        }
      }
    }
    if (name.empty()) return fail(Status::Decode, "the dotLottie archive contains no animation");
    CVT_TRY_ASSIGN(member, zip->read(name));
    out.zip.emplace(std::move(*zip));
    json_bytes = member;
  }
  auto parsed = util::parse_json(to_string(json_bytes), 256);
  if (!parsed) return fail(Status::Decode, "not a readable Lottie file: {}", parsed.error().message);
  if (!parsed->is_object() || !parsed->contains("layers") || !(*parsed)["layers"].is_array()) {
    return fail(Status::Decode, "not a Lottie animation (no layers)");
  }
  out.root = std::move(*parsed);
  return out;
}

double resolve_frame(const Json& root, double requested) {
  const double ip = root["ip"].as_double(0), op = root["op"].as_double(ip + 1);
  if (requested < 0) return ip;
  return std::clamp(ip + requested, ip, std::max(ip, op - 1e-6));
}

Result<Scene> read_lottie(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Loaded loaded, load(ctx));
  const double frame = resolve_frame(loaded.root, ctx.options.get_float("frame", -1.0));
  LottieBuilder builder(ctx, loaded.root, loaded.zip ? &*loaded.zip : nullptr);
  return builder.build(frame);
}

/// Image reader: several frames rendered into an animation.
Result<AnyIR> read_lottie_animation(ReadContext& ctx) {
  CVT_TRY_ASSIGN(Loaded loaded, load(ctx));
  const Json& root = loaded.root;
  const double source_fps = std::clamp(root["fr"].as_double(30.0), 1.0, 240.0);
  const double ip = root["ip"].as_double(0), op = root["op"].as_double(ip + 1);
  double fps = ctx.options.get_float("animation_fps", 0.0);
  if (!(fps > 0)) fps = std::min(source_fps, 30.0);
  const double duration = std::max(0.0, (op - ip) / source_fps);
  auto count = static_cast<std::int64_t>(ctx.options.get_int("animation_frames", 0, 0, 1000));
  if (count == 0) count = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(duration * fps + 1e-9)), 1, 300);
  RenderOptions options;
  options.width = static_cast<std::uint32_t>(ctx.options.get_int("width", 0, 0, 65535));
  options.height = static_cast<std::uint32_t>(ctx.options.get_int("height", 0, 0, 65535));
  options.dpi = ctx.options.get_float("dpi", 96.0, 1.0, 9600.0);
  const std::string background = util::to_lower(util::trim(ctx.options.get_string("background", "transparent")));
  if (!background.empty() && background != "transparent" && background != "none") {
    const auto c = parse_android_color("#" + background);
    if (!c || background.size() != 6) return fail(Status::InvalidArgument, "option 'background' must be six hexadecimal digits");
    options.background = *c;
  }
  const double w = root["w"].as_double(0), h = root["h"].as_double(0);
  const double px = options.width != 0 ? options.width : w * options.dpi / 96.0;
  const double py = options.height != 0 ? options.height : h * options.dpi / 96.0;
  if (px > 0 && py > 0 && px < 1e6 && py < 1e6) {
    CVT_TRY(ctx.limits.check_pixels(static_cast<std::uint64_t>(std::ceil(px)), static_cast<std::uint64_t>(std::ceil(py)),
                                    static_cast<std::uint64_t>(count)));
  }
  LottieBuilder builder(ctx, root, loaded.zip ? &*loaded.zip : nullptr);
  ir::Image image;
  const double step = count > 1 ? (op - ip) / static_cast<double>(count) : 0.0;
  const auto delay = static_cast<std::uint32_t>(std::lround(count > 1 ? duration * 1000.0 / static_cast<double>(count) : 0.0));
  for (std::int64_t k = 0; k < count; ++k) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(k) / static_cast<double>(count), "Rendering Lottie frames"));
    CVT_TRY_ASSIGN(const Scene scene, builder.build(ip + step * static_cast<double>(k)));
    CVT_TRY_ASSIGN(ir::Frame frame, render_page(scene, options, ctx));
    frame.delay_ms = delay;
    image.frames.push_back(std::move(frame));
  }
  if (count > 1) ctx.warn(std::format("The animation was sampled into {} frames.", count));
  image.loop_count = 0;
  return AnyIR(std::move(image));
}

}  // namespace

void register_lottie(Registry& registry) {
  VectorFormat format;
  format.info.id = "lottie";
  format.info.name = "Lottie animation";
  format.info.description = "Bodymovin/Lottie vector animation (JSON or the zipped .lottie container).";
  format.info.category = Category::Vector;
  format.info.extensions = {"json", "lottie"};
  format.info.mime = "application/json";
  format.info.popularity = 40;
  format.info.sniff = sniff_lottie;
  format.read = read_lottie;
  format.read_options = {
      OptionSpec::number("frame", "Frame", -1.0, -1.0, 1e9,
                         "Animation frame to convert, counted from the first frame; -1 is the first frame.")
          .advanced(),
  };
  format.lossy_read = true;
  format.read_cost = 12;
  format.image_reader = false;  // the animation reader below renders frames itself
  register_vector_format(registry, std::move(format));

  // Animated Image reader, cheaper than the single-frame rasteriser so that
  // lottie -> gif / apng / webp produce an animation.
  Codec codec;
  codec.format_id = "lottie";
  codec.kind = Kind::Image;
  codec.lossy = true;
  codec.read_cost = 25;
  codec.read = read_lottie_animation;
  codec.read_options = render_read_options();
  std::erase_if(codec.read_options, [](const OptionSpec& spec) { return spec.key == "page"; });
  codec.read_options.push_back(OptionSpec::integer("animation_frames", "Frames", 0, 0, 1000,
                                                   "Number of frames to render; 0 samples the whole animation "
                                                   "(at most 300 frames)."));
  codec.read_options.push_back(
      OptionSpec::number("animation_fps", "Frame rate", 0.0, 0.0, 240.0,
                         "Frames per second of the rendered animation; 0 uses the file's rate (at most 30)."));
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::vector::misc
