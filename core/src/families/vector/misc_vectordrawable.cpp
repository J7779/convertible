// Convertible core - vector family: Android VectorDrawable XML.
//
// Reader: <vector> (size in dp/px/in/mm/pt, viewport, alpha, tint),
// <group> (rotation, pivot, scale, translate), <clip-path>, <path> (path
// data, colours #RGB/#ARGB/#RRGGBB/#AARRGGBB, alphas, stroke attributes,
// fill type, trimPath applied geometrically), inline aapt:attr <gradient>
// (linear, radial; sweep approximated by solid wedges).
// Writer: everything as path data with a configurable precision, solid and
// gradient paints, strokes, groups with decomposed transforms (or baked into
// the path data), clip paths; text as outlines; pictures, masks and filters
// are dropped with a warning.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/misc_common.hpp"
#include "families/vector/vector_codecs.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::vector::misc {

namespace {

constexpr std::string_view android_ns = "http://schemas.android.com/apk/res/android";
constexpr std::size_t max_vd_segments = 4'000'000;
constexpr std::size_t max_vd_elements = 1'000'000;

// --------------------------------------------------------------------------- detection

int sniff_vectordrawable(std::span<const std::byte> data, std::string_view) {
  const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
  // Skip a BOM, the XML declaration, comments and white space before the root.
  std::size_t i = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
  for (int guard = 0; guard < 64 && i < text.size(); ++guard) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
    if (text.substr(i).starts_with("<?")) {
      const std::size_t end = text.find("?>", i);
      if (end == std::string_view::npos) return 0;
      i = end + 2;
    } else if (text.substr(i).starts_with("<!--")) {
      const std::size_t end = text.find("-->", i);
      if (end == std::string_view::npos) return 0;
      i = end + 3;
    } else {
      break;
    }
  }
  const std::string_view rest = text.substr(std::min(i, text.size()));
  if (!rest.starts_with("<vector")) return 0;
  if (rest.size() > 7 && rest[7] != ' ' && rest[7] != '\n' && rest[7] != '\r' && rest[7] != '\t' && rest[7] != '>') {
    return 0;
  }
  if (rest.find(android_ns) == std::string_view::npos) return 0;
  if (rest.find(":pathData") == std::string_view::npos) return 0;
  return 95;
}

// --------------------------------------------------------------------------- reader

struct SweepFill {
  Point center;
  std::vector<GradientStop> stops;
};

struct VdPaint {
  Paint paint;
  std::optional<SweepFill> sweep;
};

class VdReader {
 public:
  explicit VdReader(ReadContext& ctx) : ctx_(ctx) {}

  Result<Scene> read() {
    util::XmlParseOptions options;
    options.max_depth = 256;
    auto parsed = util::parse_xml(ctx_.input, options);
    if (!parsed) return fail(Status::Decode, "not a readable VectorDrawable: {}", parsed.error().message);
    const util::XmlNode root = parsed->root();
    if (root.local_name() != "vector") return fail(Status::Decode, "not a VectorDrawable: the root element is not <vector>");

    const auto viewport_w = number(root, "viewportWidth");
    const auto viewport_h = number(root, "viewportHeight");
    auto width = dimension(root, "width");
    auto height = dimension(root, "height");
    if (!width) width = viewport_w;
    if (!height) height = viewport_h;
    const double vw = viewport_w.value_or(width.value_or(0.0));
    const double vh = viewport_h.value_or(height.value_or(0.0));
    if (!width || !height || !(*width > 0) || !(*height > 0) || !(vw > 0) || !(vh > 0) || !std::isfinite(*width) ||
        !std::isfinite(*height) || !std::isfinite(vw) || !std::isfinite(vh) || *width > 1e7 || *height > 1e7) {
      return fail(Status::Decode, "VectorDrawable needs a positive width, height and viewport");
    }

    Scene scene;
    scene.source_units = "px";
    Page page;
    page.width = *width;
    page.height = *height;
    page.root.transform = Matrix::scale(*width / vw, *height / vh);
    page.root.opacity = std::clamp(number(root, "alpha").value_or(1.0), 0.0, 1.0);
    const std::string_view name = attr(root, "name");
    if (!name.empty()) page.root.id = std::string(name);
    CVT_TRY(read_children(root, page.root, 0));

    const std::string_view tint = attr(root, "tint");
    if (!tint.empty()) {
      auto color = parse_android_color(tint);
      if (!color && util::trim(tint).starts_with("?")) {
        // The usual theme attribute of icons; light theme value.
        color = Color::rgb8(0, 0, 0, 0x8A);
        ctx_.warn(std::format("The theme tint '{}' was resolved to the light theme default (54% black).", tint));
      }
      if (color) {
        const std::string mode = util::to_lower(util::trim(attr(root, "tintMode")));
        apply_tint(page.root, *color, mode.empty() ? "src_in" : mode);
      } else {
        ctx_.warn(std::format("The tint '{}' could not be resolved and was ignored.", tint));
      }
    }
    scene.pages.push_back(std::move(page));
    return scene;
  }

 private:
  ReadContext& ctx_;
  std::size_t elements_ = 0;
  bool warned_reference_ = false;

  static std::string_view attr(util::XmlNode node, std::string_view local) {
    util::XmlAttribute a = node.attribute(android_ns, local);
    if (!a) {
      for (const util::XmlAttribute candidate : node.attributes()) {
        if (candidate.local_name() == local && candidate.prefix() != "xmlns") {
          a = candidate;
          break;
        }
      }
    }
    return a ? util::trim(a.value()) : std::string_view{};
  }

  void reference_warning(std::string_view value) {
    if (warned_reference_) return;
    warned_reference_ = true;
    ctx_.warn(std::format("Resource reference '{}' cannot be resolved outside an app; a default was used.", value));
  }

  std::optional<double> number(util::XmlNode node, std::string_view local) {
    const std::string_view v = attr(node, local);
    if (v.empty()) return std::nullopt;
    const auto parsed = util::parse_double(v);
    if (!parsed || !std::isfinite(*parsed)) {
      reference_warning(v);
      return std::nullopt;
    }
    return *parsed;
  }

  std::optional<double> dimension(util::XmlNode node, std::string_view local) {
    std::string_view v = attr(node, local);
    if (v.empty()) return std::nullopt;
    double factor = 1.0;
    static constexpr std::pair<std::string_view, double> units[] = {
        {"dip", 1.0}, {"dp", 1.0}, {"sp", 1.0}, {"px", 1.0}, {"pt", px_per_pt}, {"in", px_per_in}, {"mm", px_per_mm}};
    for (const auto& [unit, f] : units) {
      if (v.ends_with(unit)) {
        v.remove_suffix(unit.size());
        factor = f;
        break;
      }
    }
    const auto parsed = util::parse_double(util::trim(v));
    if (!parsed || !std::isfinite(*parsed)) {
      reference_warning(attr(node, local));
      return std::nullopt;
    }
    return *parsed * factor;
  }

  std::optional<Color> color(util::XmlNode node, std::string_view local) {
    const std::string_view v = attr(node, local);
    if (v.empty()) return std::nullopt;
    auto c = parse_android_color(v);
    if (!c) {
      reference_warning(v);
      return Color{0, 0, 0, 1};
    }
    return c;
  }

  Result<void> count_element() {
    if (++elements_ > max_vd_elements) return fail(Status::Limit, "VectorDrawable has too many elements");
    if ((elements_ & 1023) == 0) CVT_TRY(ctx_.checkpoint(0.5, "Reading VectorDrawable"));
    return {};
  }

  Result<Path> path_data(util::XmlNode node) {
    const std::string_view data = attr(node, "pathData");
    if (data.empty()) return Path{};
    CVT_TRY_ASSIGN(PathParse parsed, parse_path_data(data, max_vd_segments));
    if (parsed.error) ctx_.warn("Malformed path data was read up to the first error.");
    return std::move(parsed.path);
  }

  /// The <gradient> of an `aapt:attr name="android:<which>"` child, if any.
  std::optional<VdPaint> inline_gradient(util::XmlNode node, std::string_view which) {
    for (const util::XmlNode child : node.elements()) {
      if (child.local_name() != "attr") continue;
      const std::string_view target = util::xml_local_name(util::trim(child.attribute("name").value()));
      if (target != which) continue;
      const util::XmlNode gradient = child.child_local("gradient");
      if (!gradient) continue;
      return read_gradient(gradient);
    }
    return std::nullopt;
  }

  VdPaint read_gradient(util::XmlNode node) {
    std::vector<GradientStop> stops;
    for (const util::XmlNode item : node.children_local("item")) {
      GradientStop stop;
      stop.offset = std::clamp(number(item, "offset").value_or(0.0), 0.0, 1.0);
      stop.color = color(item, "color").value_or(Color{0, 0, 0, 0});
      stops.push_back(stop);
      if (stops.size() > 4096) break;
    }
    if (stops.empty()) {
      stops.push_back({0.0, color(node, "startColor").value_or(Color{0, 0, 0, 0})});
      if (const auto center = color(node, "centerColor")) stops.push_back({0.5, *center});
      stops.push_back({1.0, color(node, "endColor").value_or(Color{0, 0, 0, 0})});
    }
    std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    const std::string type = util::to_lower(attr(node, "type"));
    const Point center{number(node, "centerX").value_or(0.0), number(node, "centerY").value_or(0.0)};
    VdPaint out;
    if (type == "sweep") {
      out.sweep = SweepFill{center, std::move(stops)};
      out.paint = Paint::solid(gradient_color_at(out.sweep->stops, 0.5));
      return out;
    }
    auto gradient = std::make_shared<Gradient>();
    gradient->stops = std::move(stops);
    const std::string tile = util::to_lower(attr(node, "tileMode"));
    gradient->spread = tile == "repeat" || tile == "repeated" ? Spread::Repeat
                       : tile == "mirror"                     ? Spread::Reflect
                                                              : Spread::Pad;
    if (type == "radial") {
      gradient->type = Gradient::Type::Radial;
      gradient->center = gradient->focal = center;
      gradient->radius = std::max(0.0, number(node, "gradientRadius").value_or(0.0));
    } else {
      gradient->type = Gradient::Type::Linear;
      gradient->p1 = {number(node, "startX").value_or(0.0), number(node, "startY").value_or(0.0)};
      gradient->p2 = {number(node, "endX").value_or(0.0), number(node, "endY").value_or(0.0)};
    }
    out.paint.type = Paint::Type::Gradient;
    out.paint.gradient = std::move(gradient);
    return out;
  }

  std::optional<VdPaint> paint(util::XmlNode node, std::string_view which) {
    if (auto g = inline_gradient(node, which)) return g;
    const auto c = color(node, which);
    if (!c || c->a <= 0.0f) return std::nullopt;
    return VdPaint{Paint::solid(*c), std::nullopt};
  }

  Result<void> read_path(util::XmlNode node, Group& target) {
    CVT_TRY_ASSIGN(Path path, path_data(node));
    if (path.empty()) return {};

    // trimPath (applied to the geometry used for both fill and stroke, first contour only, as Android does).
    const double trim_start = number(node, "trimPathStart").value_or(0.0);
    const double trim_end = number(node, "trimPathEnd").value_or(1.0);
    const double trim_offset = number(node, "trimPathOffset").value_or(0.0);
    if (trim_start != 0.0 || trim_end != 1.0) {
      const Rect box = control_bounds(path);
      const double tolerance = std::max(1e-4, std::max(box.width(), box.height()) * 2e-4);
      const double s = std::fmod(trim_start + trim_offset, 1.0);
      const double e = std::fmod(trim_end + trim_offset, 1.0);
      path = trim_first_contour(path, s < 0 ? s + 1 : s, e < 0 ? e + 1 : e, tolerance);
      if (path.empty()) return {};
    }

    Shape shape;
    shape.id = std::string(attr(node, "name"));
    shape.fill_rule = util::to_lower(attr(node, "fillType")) == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
    const double fill_alpha = std::clamp(number(node, "fillAlpha").value_or(1.0), 0.0, 1.0);
    const double stroke_alpha = std::clamp(number(node, "strokeAlpha").value_or(1.0), 0.0, 1.0);

    Stroke stroke;
    stroke.width = std::max(0.0, number(node, "strokeWidth").value_or(0.0));
    if (stroke.width == 0.0) stroke.hairline = true;
    const std::string cap = util::to_lower(attr(node, "strokeLineCap"));
    stroke.cap = cap == "round" ? LineCap::Round : cap == "square" ? LineCap::Square : LineCap::Butt;
    const std::string join = util::to_lower(attr(node, "strokeLineJoin"));
    stroke.join = join == "round" ? LineJoin::Round : join == "bevel" ? LineJoin::Bevel : LineJoin::Miter;
    stroke.miter_limit = std::max(1.0, number(node, "strokeMiterLimit").value_or(4.0));

    auto fill = paint(node, "fillColor");
    auto stroke_paint = paint(node, "strokeColor");
    Group* place = &target;
    if (fill && fill->sweep) {
      place->children.push_back(Node{sweep_wedges(fill->sweep->center, fill->sweep->stops, path, shape.fill_rule, fill_alpha)});
      fill.reset();
    }
    if (fill) {
      shape.fill = fill->paint;
      shape.fill.opacity = fill_alpha;
    }
    if (stroke_paint && stroke_paint->sweep) {
      Stroke outline_style = stroke;
      if (outline_style.hairline) outline_style.width = 1.0;
      const Rect box = control_bounds(path);
      const double tolerance = std::max(1e-3, std::max(box.width(), box.height()) * 1e-3);
      const Path outline = stroke_outline(path, outline_style, tolerance);
      Shape filled = std::move(shape);
      shape = Shape{};
      if (filled.fill.visible()) {
        filled.path = path;
        place->children.push_back(Node{std::move(filled)});
      }
      place->children.push_back(
          Node{sweep_wedges(stroke_paint->sweep->center, stroke_paint->sweep->stops, outline, FillRule::NonZero, stroke_alpha)});
      return {};
    }
    if (stroke_paint) {
      stroke.paint = stroke_paint->paint;
      stroke.paint.opacity = stroke_alpha;
    }
    shape.stroke = std::move(stroke);
    if (!shape.fill.visible() && !shape.stroke.paint.visible()) return {};
    shape.path = std::move(path);
    place->children.push_back(Node{std::move(shape)});
    return {};
  }

  Result<void> read_children(util::XmlNode parent, Group& target, int depth) {
    if (depth > 200) return fail(Status::Limit, "VectorDrawable groups are nested too deeply");
    Group* current = &target;
    for (const util::XmlNode child : parent.elements()) {
      CVT_TRY(count_element());
      const std::string_view name = child.local_name();
      if (name == "path") {
        CVT_TRY(read_path(child, *current));
      } else if (name == "group") {
        Group group;
        group.id = std::string(attr(child, "name"));
        const double rotation = number(child, "rotation").value_or(0.0);
        const double px = number(child, "pivotX").value_or(0.0);
        const double py = number(child, "pivotY").value_or(0.0);
        const double sx = number(child, "scaleX").value_or(1.0);
        const double sy = number(child, "scaleY").value_or(1.0);
        const double tx = number(child, "translateX").value_or(0.0);
        const double ty = number(child, "translateY").value_or(0.0);
        group.transform = Matrix::translate(tx + px, ty + py) * Matrix::rotate(rotation * std::numbers::pi / 180.0) *
                          Matrix::scale(sx, sy) * Matrix::translate(-px, -py);
        CVT_TRY(read_children(child, group, depth + 1));
        current->children.push_back(Node{std::move(group)});
      } else if (name == "clip-path") {
        CVT_TRY_ASSIGN(Path clip_path, path_data(child));
        auto clip = std::make_shared<ClipPath>();
        clip->items.push_back(ClipPath::Item{std::move(clip_path),
                                             util::to_lower(attr(child, "fillType")) == "evenodd" ? FillRule::EvenOdd
                                                                                                  : FillRule::NonZero});
        Group clipped;
        clipped.clip = std::move(clip);
        current->children.push_back(Node{std::move(clipped)});
        current = &std::get<Group>(current->children.back().value);
      }
    }
    return {};
  }

  static Color tinted(Color c, Color tint, std::string_view mode) {
    if (mode == "multiply") return Color{c.r * tint.r, c.g * tint.g, c.b * tint.b, c.a * tint.a};
    if (mode == "src_atop") {
      return Color{tint.r * tint.a + c.r * (1 - tint.a), tint.g * tint.a + c.g * (1 - tint.a),
                   tint.b * tint.a + c.b * (1 - tint.a), c.a};
    }
    if (mode == "screen") {
      return Color{1 - (1 - c.r) * (1 - tint.r), 1 - (1 - c.g) * (1 - tint.g), 1 - (1 - c.b) * (1 - tint.b), c.a};
    }
    return Color{tint.r, tint.g, tint.b, c.a * tint.a};  // src_in
  }

  static void tint_paint(Paint& paint, Color tint, std::string_view mode) {
    if (paint.type == Paint::Type::Solid) {
      paint.color = tinted(paint.color, tint, mode);
    } else if (paint.type == Paint::Type::Gradient && paint.gradient) {
      auto copy = std::make_shared<Gradient>(*paint.gradient);
      for (GradientStop& stop : copy->stops) stop.color = tinted(stop.color, tint, mode);
      paint.gradient = std::move(copy);
    }
  }

  static void apply_tint(Group& group, Color tint, std::string_view mode) {
    for (Node& node : group.children) {
      if (auto* shape = std::get_if<Shape>(&node.value)) {
        tint_paint(shape->fill, tint, mode);
        tint_paint(shape->stroke.paint, tint, mode);
      } else if (auto* child = std::get_if<Group>(&node.value)) {
        apply_tint(*child, tint, mode);
      }
    }
  }
};

Result<Scene> read_vectordrawable(ReadContext& ctx) { return VdReader(ctx).read(); }

// --------------------------------------------------------------------------- writer

class VdWriter {
 public:
  VdWriter(WriteContext& ctx, int precision) : ctx_(ctx), precision_(precision) {}

  Result<Bytes> write(const Scene& scene) {
    if (scene.pages.empty()) return fail(Status::Encode, "the drawing has no pages");
    if (scene.pages.size() > 1) ctx_.warn("VectorDrawable holds one drawing; only the first page was written.");
    const Page& page = scene.pages.front();
    double width = page.width, height = page.height;
    if (!(width > 0) || !(height > 0) || !std::isfinite(width) || !std::isfinite(height)) {
      return fail(Status::Encode, "the drawing has no size");
    }
    // A positive axis scale on the root becomes the viewport, root opacity the vector alpha.
    const Matrix& rt = page.root.transform;
    double vw = width, vh = height;
    if (rt.b == 0 && rt.c == 0 && rt.e == 0 && rt.f == 0 && rt.a > 0 && rt.d > 0 && std::isfinite(rt.a) &&
        std::isfinite(rt.d)) {
      root_absorbed_ = true;
      vw = width / rt.a;
      vh = height / rt.d;
    }
    CVT_TRY(write_group(page.root, Matrix{}, 1.0, 2, true));
    std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<vector xmlns:android=\"";
    out += android_ns;
    out += '"';
    if (uses_aapt_) out += "\n    xmlns:aapt=\"http://schemas.android.com/aapt\"";
    out += std::format("\n    android:width=\"{}dp\"\n    android:height=\"{}dp\"", num(width), num(height));
    out += std::format("\n    android:viewportWidth=\"{}\"\n    android:viewportHeight=\"{}\"", format_number(vw, 6),
                       format_number(vh, 6));
    if (page.root.opacity < 1.0) out += std::format("\n    android:alpha=\"{}\"", format_number(std::max(page.root.opacity, 0.0), 4));
    out += ">\n";
    out += body_;
    out += "</vector>\n";
    CVT_TRY(ctx_.limits.check_output(out.size()));
    return to_bytes(out);
  }

 private:
  WriteContext& ctx_;
  int precision_;
  std::string body_;
  bool uses_aapt_ = false;
  bool root_absorbed_ = false;
  std::size_t nodes_ = 0;
  bool warned_picture_ = false, warned_effects_ = false, warned_opacity_ = false, warned_pattern_ = false,
       warned_radial_ = false, warned_dash_ = false, warned_hidden_ = false;

  std::string num(double v) const { return format_number(v, precision_); }
  static std::string pad(int depth) { return std::string(static_cast<std::size_t>(depth) * 2, ' '); }

  void warn_once(bool& flag, std::string_view message) {
    if (flag) return;
    flag = true;
    ctx_.warn(std::string(message));
  }

  static std::string attribute_name(std::string_view id) { return util::xml_escape(id, true); }

  Result<void> write_group(const Group& group, const Matrix& bake, double opacity, int depth, bool is_root) {
    if (!group.visible) {
      warn_once(warned_hidden_, "Hidden layers were left out.");
      return {};
    }
    if (++nodes_ % 512 == 0) CVT_TRY(ctx_.checkpoint(0.5, "Writing VectorDrawable"));
    if (depth > 400) return fail(Status::Encode, "the drawing is nested too deeply for VectorDrawable");
    if (group.mask || group.filter || group.blend != BlendMode::Normal) {
      warn_once(warned_effects_, "Masks, filters and blend modes cannot be expressed in VectorDrawable and were dropped.");
    }
    if (!is_root) {
      if (group.opacity < 1.0 && group.children.size() > 1) {
        warn_once(warned_opacity_, "Group opacity was applied to each shape separately (VectorDrawable has no group alpha).");
      }
      opacity *= std::clamp(group.opacity, 0.0, 1.0);
    }
    const Matrix transform = is_root && root_absorbed_ ? Matrix{} : group.transform;
    Matrix child_bake = bake * transform;
    std::optional<Decomposed> parts;
    if (bake.is_identity() && !transform.is_identity()) parts = decompose(transform);
    const bool transform_group = parts.has_value();
    if (transform_group) child_bake = Matrix{};

    const bool needs_group = transform_group || group.clip != nullptr || (!group.id.empty() && !is_root);
    int inner = depth;
    int opened = 0;
    if (needs_group) {
      std::string open = pad(depth) + "<group";
      if (!group.id.empty()) open += std::format(" android:name=\"{}\"", attribute_name(group.id));
      if (parts) {
        if (parts->rotation != 0.0) open += std::format(" android:rotation=\"{}\"", format_number(parts->rotation, 6));
        if (std::abs(parts->sx - 1.0) > 1e-12) open += std::format(" android:scaleX=\"{}\"", format_number(parts->sx, 8));
        if (std::abs(parts->sy - 1.0) > 1e-12) open += std::format(" android:scaleY=\"{}\"", format_number(parts->sy, 8));
        if (parts->tx != 0.0) open += std::format(" android:translateX=\"{}\"", num(parts->tx));
        if (parts->ty != 0.0) open += std::format(" android:translateY=\"{}\"", num(parts->ty));
      }
      body_ += open + ">\n";
      ++opened;
      inner = depth + 1;
      // Clip paths: each nested clip becomes another group so the clips intersect.
      const ClipPath* clip = group.clip.get();
      for (int guard = 0; clip != nullptr && guard < 64; ++guard) {
        Path combined;
        for (const ClipPath::Item& item : clip->items) append(combined, item.path, child_bake);
        body_ += std::format("{}<clip-path android:pathData=\"{}\"/>\n", pad(inner), minify_path_data(combined, precision_));
        clip = clip->clip.get();
        if (clip != nullptr) {
          body_ += pad(inner) + "<group>\n";
          ++opened;
          ++inner;
        }
      }
    }
    for (const Node& node : group.children) {
      if (const auto* shape = std::get_if<Shape>(&node.value)) {
        write_shape(*shape, child_bake, opacity, inner);
      } else if (const auto* text = std::get_if<Text>(&node.value)) {
        Shape outline;
        outline.path = text->outline;
        outline.fill = text->fill;
        outline.stroke = text->stroke;
        outline.id = text->id;
        write_shape(outline, child_bake, opacity, inner);
      } else if (std::holds_alternative<Picture>(node.value)) {
        warn_once(warned_picture_, "VectorDrawable cannot embed pictures; raster pictures were dropped.");
      } else if (const auto* child = std::get_if<Group>(&node.value)) {
        CVT_TRY(write_group(*child, child_bake, opacity, inner, false));
      }
    }
    for (int k = opened; k > 0; --k) body_ += pad(depth + k - 1) + "</group>\n";
    return {};
  }

  /// Solid colour for a paint VectorDrawable cannot hold.
  Color fallback_color(const Paint& paint) {
    if (paint.type == Paint::Type::Pattern) {
      warn_once(warned_pattern_, "Pattern fills were replaced by a solid colour.");
      if (paint.pattern && paint.pattern->content) {
        for (const Node& node : paint.pattern->content->children) {
          if (const auto* s = std::get_if<Shape>(&node.value); s != nullptr && s->fill.type == Paint::Type::Solid) {
            return s->fill.color;
          }
        }
      }
      return Color{0.5f, 0.5f, 0.5f, 1.0f};
    }
    return paint.color;
  }

  /// Attributes (or an aapt:attr child) for a paint; returns the child XML, if any.
  std::string paint_xml(const Paint& paint, const Matrix& bake, std::string_view which, std::string& attributes,
                        int depth) {
    if (paint.type == Paint::Type::Gradient && paint.gradient && !paint.gradient->stops.empty()) {
      const Gradient& g = *paint.gradient;
      const Matrix m = bake * g.transform;
      std::string x = std::format("{}<aapt:attr name=\"android:{}\">\n{}<gradient", pad(depth), which, pad(depth + 1));
      if (g.type == Gradient::Type::Linear) {
        const Point d = g.p2 - g.p1;
        const double len2 = dot(d, d);
        Point a = m.apply(g.p1), b = m.apply(g.p2);
        if (len2 > 0 && m.invertible()) {
          // Exact under any affine map: keep the gradient's iso-lines.
          const Matrix inv = m.inverted();
          const Point grad{(inv.a * d.x + inv.b * d.y) / len2, (inv.c * d.x + inv.d * d.y) / len2};
          const double g2 = dot(grad, grad);
          if (g2 > 0) b = a + grad * (1.0 / g2);
        }
        x += std::format(" android:type=\"linear\" android:startX=\"{}\" android:startY=\"{}\" android:endX=\"{}\" android:endY=\"{}\"",
                         num(a.x), num(a.y), num(b.x), num(b.y));
      } else {
        const Point c = m.apply(g.center);
        double scale = m.mean_scale();
        if (const auto parts = decompose(m); !parts || std::abs(std::abs(parts->sx) - std::abs(parts->sy)) > 1e-6 * std::abs(parts->sx)) {
          warn_once(warned_radial_, "Elliptical radial gradients were approximated by circular ones.");
        } else {
          scale = std::abs(parts->sx);
        }
        if (!(g.focal == g.center) || g.focal_radius > 0) {
          warn_once(warned_radial_, "Radial gradient focal points are not supported by VectorDrawable and were dropped.");
        }
        x += std::format(" android:type=\"radial\" android:centerX=\"{}\" android:centerY=\"{}\" android:gradientRadius=\"{}\"",
                         num(c.x), num(c.y), num(std::max(g.radius * scale, 1e-6)));
      }
      if (g.spread == Spread::Repeat) x += " android:tileMode=\"repeat\"";
      else if (g.spread == Spread::Reflect) x += " android:tileMode=\"mirror\"";
      x += ">\n";
      for (const GradientStop& stop : g.stops) {
        x += std::format("{}<item android:offset=\"{}\" android:color=\"{}\"/>\n", pad(depth + 2),
                         format_number(std::clamp(stop.offset, 0.0, 1.0), 4), android_color_string(stop.color));
      }
      x += std::format("{}</gradient>\n{}</aapt:attr>\n", pad(depth + 1), pad(depth));
      uses_aapt_ = true;
      return x;
    }
    const Color c = paint.type == Paint::Type::Solid ? paint.color : fallback_color(paint);
    attributes += std::format(" android:{}=\"{}\"", which, android_color_string(c));
    return {};
  }

  void write_shape(const Shape& shape, const Matrix& bake, double opacity, int depth) {
    if (++nodes_ % 512 == 0) (void)ctx_.checkpoint(0.5, "Writing VectorDrawable");
    const bool fill = shape.fill.visible();
    const bool stroke = shape.stroke.paint.visible() && (shape.stroke.width > 0 || shape.stroke.hairline);
    if ((!fill && !stroke) || shape.path.empty()) return;
    const Path path = bake.is_identity() ? shape.path : transformed(shape.path, bake);
    const bool dashed = stroke && !shape.stroke.dash.empty();
    if (dashed) {
      warn_once(warned_dash_, "Dashed strokes were converted to separate dash segments (VectorDrawable has no dashes).");
      if (fill) emit_path(shape, path, bake, opacity, depth, true, false);
      const Rect box = control_bounds(path);
      const double tolerance = std::max(1e-3, std::max(box.width(), box.height()) * 2e-4);
      const double scale = bake.mean_scale();
      std::vector<double> pattern = shape.stroke.dash;
      for (double& d : pattern) d = std::max(0.0, d * scale);
      double sum = 0;
      for (double d : pattern) sum += d;
      Path dashes;
      if (sum > 0 && pattern.size() % 2 == 0) {
        for (const Polyline& pl : dash(flatten(path, tolerance), pattern, shape.stroke.dash_offset * scale)) {
          if (pl.points.empty()) continue;
          dashes.move_to(pl.points.front());
          for (std::size_t k = 1; k < pl.points.size(); ++k) dashes.line_to(pl.points[k]);
          if (pl.closed) dashes.close();
        }
      } else {
        dashes = path;
      }
      emit_path(shape, dashes, bake, opacity, depth, false, true);
      return;
    }
    emit_path(shape, path, bake, opacity, depth, fill, stroke);
  }

  void emit_path(const Shape& shape, const Path& path, const Matrix& bake, double opacity, int depth, bool fill,
                 bool stroke) {
    if (path.empty()) return;
    std::string attributes;
    std::string children;
    if (!shape.id.empty()) attributes += std::format(" android:name=\"{}\"", attribute_name(shape.id));
    attributes += std::format(" android:pathData=\"{}\"", minify_path_data(path, precision_));
    if (fill) {
      children += paint_xml(shape.fill, bake, "fillColor", attributes, depth + 1);
      const double alpha = std::clamp(shape.fill.opacity * opacity, 0.0, 1.0);
      if (alpha < 1.0) attributes += std::format(" android:fillAlpha=\"{}\"", format_number(alpha, 4));
      if (shape.fill_rule == FillRule::EvenOdd) attributes += " android:fillType=\"evenOdd\"";
    }
    if (stroke) {
      const Stroke& s = shape.stroke;
      children += paint_xml(s.paint, bake, "strokeColor", attributes, depth + 1);
      const double alpha = std::clamp(s.paint.opacity * opacity, 0.0, 1.0);
      if (alpha < 1.0) attributes += std::format(" android:strokeAlpha=\"{}\"", format_number(alpha, 4));
      const double width = s.hairline ? 1.0 : s.width * bake.mean_scale();
      attributes += std::format(" android:strokeWidth=\"{}\"", num(width));
      if (s.cap == LineCap::Round) attributes += " android:strokeLineCap=\"round\"";
      else if (s.cap == LineCap::Square) attributes += " android:strokeLineCap=\"square\"";
      if (s.join == LineJoin::Round) attributes += " android:strokeLineJoin=\"round\"";
      else if (s.join == LineJoin::Bevel) attributes += " android:strokeLineJoin=\"bevel\"";
      if (s.join != LineJoin::Round && s.join != LineJoin::Bevel && std::abs(s.miter_limit - 4.0) > 1e-9) {
        attributes += std::format(" android:strokeMiterLimit=\"{}\"", format_number(s.miter_limit, 4));
      }
    }
    if (children.empty()) {
      body_ += std::format("{}<path{}/>\n", pad(depth), attributes);
    } else {
      body_ += std::format("{}<path{}>\n{}{}</path>\n", pad(depth), attributes, children, pad(depth));
    }
  }
};

Result<Bytes> write_vectordrawable(const Scene& scene, WriteContext& ctx) {
  const int precision = static_cast<int>(ctx.options.get_int("precision", 3, 0, 8));
  return VdWriter(ctx, precision).write(scene);
}

}  // namespace

void register_vectordrawable(Registry& registry) {
  VectorFormat format;
  format.info.id = "vectordrawable";
  format.info.name = "Android VectorDrawable";
  format.info.description = "Android vector graphics resource (XML with path data), used for app icons.";
  format.info.category = Category::Vector;
  format.info.extensions = {"xml"};
  format.info.mime = "application/xml";
  format.info.popularity = 35;
  format.info.sniff = sniff_vectordrawable;
  format.read = read_vectordrawable;
  format.write = write_vectordrawable;
  format.write_options = {
      OptionSpec::integer("precision", "Precision", 3, 0, 8,
                          "Decimal places of coordinates in the path data; fewer gives smaller files."),
  };
  format.lossy_write = true;
  format.can_embed_images = false;
  register_vector_format(registry, std::move(format));
}

}  // namespace cvt::vector::misc
