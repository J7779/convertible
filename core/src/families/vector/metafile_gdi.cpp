// Convertible core - vector family: Windows metafiles, the GDI state machine shared by WMF and EMF.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "families/vector/font.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/metafile_internal.hpp"
#include "util/text.hpp"

namespace cvt::vector::metafile {

namespace {

constexpr std::size_t max_nodes = 4'000'000;
constexpr std::size_t max_points = 32'000'000;
constexpr std::size_t max_saved_states = 4096;
constexpr std::size_t max_objects = 65536;
constexpr double pi = std::numbers::pi;

bool finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }

bool path_finite(const Path& path) {
  return std::all_of(path.points.begin(), path.points.end(), [](Point p) { return finite(p); });
}

void add_rect_points(Path& path, const Matrix& m, Rect r) {
  path.move_to(m.apply({r.x0, r.y0}));
  path.line_to(m.apply({r.x1, r.y0}));
  path.line_to(m.apply({r.x1, r.y1}));
  path.line_to(m.apply({r.x0, r.y1}));
  path.close();
}

Rect normalized(Rect r) { return {std::min(r.x0, r.x1), std::min(r.y0, r.y1), std::max(r.x0, r.x1), std::max(r.y0, r.y1)}; }

Rect node_bounds(const Node& node, const Matrix& m);

Rect group_bounds(const Group& g, const Matrix& parent) {
  Rect out;
  const Matrix m = parent * g.transform;
  for (const Node& child : g.children) out.add(node_bounds(child, m));
  return out;
}

Rect node_bounds(const Node& node, const Matrix& m) {
  if (const auto* s = std::get_if<Shape>(&node.value)) {
    Rect r = bounds(s->path, m);
    if (s->stroke.paint.visible() && !r.empty()) {
      const double half = s->stroke.width * m.mean_scale() / 2;
      r = {r.x0 - half, r.y0 - half, r.x1 + half, r.y1 + half};
    }
    return r;
  }
  if (const auto* t = std::get_if<Text>(&node.value)) return bounds(t->outline, m);
  if (const auto* p = std::get_if<Picture>(&node.value)) return transform_rect(p->bounds, m);
  if (const auto* g = std::get_if<Group>(&node.value)) return group_bounds(*g, m);
  return {};
}

bool same_stroke(const Stroke& a, const Stroke& b) {
  return a.paint.type == b.paint.type && a.paint.color == b.paint.color && a.paint.opacity == b.paint.opacity &&
         a.width == b.width && a.cap == b.cap && a.join == b.join && a.dash == b.dash && a.hairline == b.hairline &&
         a.miter_limit == b.miter_limit;
}

/// Copy a clip chain with `tail` attached below its last level.
/// Clip chains stay short: every level costs the renderer a mask, and hostile
/// files could otherwise intersect millions of times.
constexpr std::size_t max_clip_depth = 16;

std::size_t clip_depth(const ClipPath* c) {
  std::size_t depth = 0;
  for (; c != nullptr; c = c->clip.get()) ++depth;
  return depth;
}

ClipRef chain_append_unchecked(const ClipRef& head, const ClipRef& tail) {
  if (!head) return tail;
  if (!tail) return head;
  auto copy = std::make_shared<ClipPath>(*head);
  copy->clip = chain_append_unchecked(head->clip, tail);
  return copy;
}

ClipRef chain_append(const ClipRef& head, const ClipRef& tail) {
  if (clip_depth(head.get()) + clip_depth(tail.get()) > max_clip_depth) return head;  // simplified
  return chain_append_unchecked(head, tail);
}

/// The rectangle of a one-rectangle path (axis-aligned, NonZero), if it is one.
std::optional<Rect> path_rect(const Path& path) {
  if (path.points.size() < 4 || path.points.size() > 5) return std::nullopt;
  for (const Verb v : path.verbs) {
    if (v == Verb::Quad || v == Verb::Cubic) return std::nullopt;
  }
  if (std::count(path.verbs.begin(), path.verbs.end(), Verb::Move) != 1) return std::nullopt;
  Rect r;
  for (const Point p : path.points) r.add(p);
  for (const Point p : path.points) {
    const bool on_x = p.x == r.x0 || p.x == r.x1;
    const bool on_y = p.y == r.y0 || p.y == r.y1;
    if (!on_x || !on_y) return std::nullopt;
  }
  return r;
}

Path items_union_path(const ClipPath& c) {
  Path out;
  for (const auto& item : c.items) append(out, item.path);
  return out;
}

}  // namespace

// --------------------------------------------------------------------------- clips

Path huge_rect_path() {
  Path p;
  add_rect(p, -1e7, -1e7, 2e7, 2e7);
  return p;
}

ClipRef make_clip(Path path, FillRule rule, ClipRef parent) {
  auto clip = std::make_shared<ClipPath>();
  clip->items.push_back({std::move(path), rule});
  clip->clip = std::move(parent);
  return clip;
}

ClipRef empty_clip() {
  Path p;
  p.move_to({0, 0});
  p.line_to({0, 0});
  p.close();
  return make_clip(std::move(p), FillRule::NonZero);
}

ClipRef clip_intersect(const ClipRef& current, Path path, FillRule rule) {
  // Rectangle within rectangle (the common case): one rectangle.
  if (current && current->items.size() == 1 && current->items[0].rule == FillRule::NonZero) {
    const auto a = path_rect(current->items[0].path);
    const auto b = path_rect(path);
    if (a && b) {
      Rect r = a->intersect(*b);
      if (r.empty()) r = Rect{a->x0, a->y0, a->x0, a->y0};
      Path p;
      add_rect(p, r.x0, r.y0, r.width(), r.height());
      return make_clip(std::move(p), FillRule::NonZero, current->clip);
    }
  }
  if (clip_depth(current.get()) >= max_clip_depth) return current;  // simplified
  return make_clip(std::move(path), rule, current);
}

ClipRef clip_exclude(const ClipRef& current, Path path) {
  if (clip_depth(current.get()) >= max_clip_depth) return current;  // simplified
  Path p = huge_rect_path();
  append(p, path);
  return make_clip(std::move(p), FillRule::EvenOdd, current);
}

ClipRef clip_union(const ClipRef& current, Path path, FillRule rule, bool& approximated) {
  if (!current) return nullptr;  // everything OR anything = everything
  auto copy = std::make_shared<ClipPath>(*current);
  copy->items.push_back({std::move(path), rule});
  if (current->clip) approximated = true;
  return copy;
}

ClipRef combine_clip(const ClipRef& current, const ClipRef& incoming, int mode, bool& approximated) {
  switch (mode) {
    case 1:  // AND
      if (!incoming) return empty_clip();
      return chain_append(incoming, current);
    case 2:  // OR
    case 3: {  // XOR (approximated as OR)
      if (mode == 3) approximated = true;
      if (!current) return nullptr;
      if (!incoming) return current;
      auto copy = std::make_shared<ClipPath>(*current);
      for (const auto& item : incoming->items) copy->items.push_back(item);
      if (current->clip || incoming->clip) approximated = true;
      return copy;
    }
    case 4: {  // DIFF: current minus incoming
      if (!incoming) return current;
      if (incoming->clip) approximated = true;
      return clip_exclude(current, items_union_path(*incoming));
    }
    case 6: {  // complement: incoming minus current
      if (!incoming) return empty_clip();
      if (!current) return empty_clip();
      if (current->clip) approximated = true;
      return chain_append(incoming, clip_exclude(nullptr, items_union_path(*current)));
    }
    default:  // COPY
      return incoming;
  }
}

ClipRef transform_clip(const ClipRef& clip, const Matrix& m) {
  if (!clip) return nullptr;
  auto copy = std::make_shared<ClipPath>();
  for (const auto& item : clip->items) copy->items.push_back({transformed(item.path, m), item.rule});
  copy->clip = transform_clip(clip->clip, m);
  return copy;
}

// --------------------------------------------------------------------------- scene builder

Result<void> SceneBuilder::add(Node node, const ClipRef& clip) {
  if (++nodes_ > max_nodes) return fail(Status::Limit, "the metafile draws more than {} shapes", max_nodes);
  if (!clip) {
    root_.children.push_back(std::move(node));
    group_open_ = false;
    open_clip_ = nullptr;
    return {};
  }
  if (!group_open_ || open_clip_ != clip) {
    Group g;
    g.clip = clip;
    root_.children.push_back(Node{std::move(g)});
    group_open_ = true;
    open_clip_ = clip;
  }
  std::get<Group>(root_.children.back().value).children.push_back(std::move(node));
  return {};
}

Result<void> SceneBuilder::add_shape(Shape shape, const ClipRef& clip) {
  CVT_TRY(charge_points(shape.path.points.size()));
  return add(Node{std::move(shape)}, clip);
}

Result<void> SceneBuilder::charge_points(std::size_t count) {
  points_ += count;
  if (points_ > max_points) return fail(Status::Limit, "the metafile draws more than {} points", max_points);
  return {};
}

Group SceneBuilder::take_root() {
  group_open_ = false;
  open_clip_ = nullptr;
  return std::exchange(root_, Group{});
}

Rect SceneBuilder::content_bounds() const { return group_bounds(root_, Matrix{}); }

Rect drawing_bounds(const Group& group) { return group_bounds(group, Matrix{}); }

// --------------------------------------------------------------------------- player

GdiPlayer::GdiPlayer(Flavor flavor, const Context& ctx) : flavor_(flavor), ctx_(ctx) {
  dc_.pen.color = {0, 0, 0, 1};
  dc_.brush.style = 0;
  dc_.brush.color = {1, 1, 1, 1};
  recompute();
}

void GdiPlayer::note(std::string_view message) {
  if (std::find(notes_.begin(), notes_.end(), message) != notes_.end()) return;
  notes_.emplace_back(message);
  ctx_.warn(std::string(message));
}

void GdiPlayer::set_device_resolution(double px_per_mm_x, double px_per_mm_y) {
  if (std::isfinite(px_per_mm_x) && px_per_mm_x > 0) dev_px_per_mm_x_ = px_per_mm_x;
  if (std::isfinite(px_per_mm_y) && px_per_mm_y > 0) dev_px_per_mm_y_ = px_per_mm_y;
  recompute();
}

void GdiPlayer::set_wmf_frame(Rect logical, double page_width, double page_height) {
  wmf_frame_set_ = true;
  wmf_frame_ = logical;
  wmf_page_w_ = page_width;
  wmf_page_h_ = page_height;
  recompute();
}

Matrix GdiPlayer::to_device() const {
  if (flavor_ == Flavor::Wmf) return to_page_;
  return device_to_page_.inverted() * to_page_;
}

void GdiPlayer::recompute() {
  const DcState& s = dc_;
  const auto safe = [](double num, double den) {
    const double v = den == 0 ? 1.0 : num / den;
    return std::isfinite(v) && v != 0 ? v : 1.0;
  };
  Matrix mapping;
  if (flavor_ == Flavor::Wmf) {
    switch (s.map_mode) {
      case 1:  // MM_TEXT
        if (wmf_frame_set_ && wmf_frame_.width() > 0 && wmf_frame_.height() > 0 && !s.window_ext_set) {
          mapping = Matrix::scale(wmf_page_w_ / wmf_frame_.width(), wmf_page_h_ / wmf_frame_.height()) *
                    Matrix::translate(-wmf_frame_.x0, -wmf_frame_.y0);
        } else if (wmf_frame_set_ && s.window_ext_set) {
          mapping = Matrix::scale(safe(wmf_page_w_, s.window_ext.x), safe(wmf_page_h_, s.window_ext.y)) *
                    Matrix::translate(-s.window_org.x, -s.window_org.y);
        } else {
          mapping = Matrix::translate(-s.window_org.x, -s.window_org.y);
        }
        break;
      case 7:
      case 8: {
        Point org = s.window_org, ext = s.window_ext;
        if (!s.window_ext_set && wmf_frame_set_) {
          org = {wmf_frame_.x0, wmf_frame_.y0};
          ext = {wmf_frame_.width(), wmf_frame_.height()};
        }
        double sx = 1.0, sy = 1.0;
        if (wmf_frame_set_) {
          sx = safe(wmf_page_w_, ext.x);
          sy = safe(wmf_page_h_, ext.y);
        }
        if (s.map_mode == 7) {
          const double m = std::min(std::abs(sx), std::abs(sy));
          sx = std::copysign(m, sx);
          sy = std::copysign(m, sy);
        }
        mapping = Matrix::scale(sx, sy) * Matrix::translate(-org.x, -org.y);
        break;
      }
      default: {
        double mm = 0.1;
        if (s.map_mode == 3) mm = 0.01;
        if (s.map_mode == 4) mm = 0.254;
        if (s.map_mode == 5) mm = 0.0254;
        if (s.map_mode == 6) mm = 25.4 / 1440.0;
        const double k = mm * px_per_mm;
        mapping = Matrix::scale(k, -k) * Matrix::translate(-s.window_org.x, -s.window_org.y);
        break;
      }
    }
    to_page_ = mapping;
    return;
  }
  double sx = 1.0, sy = 1.0;
  switch (s.map_mode) {
    case 1: break;
    case 2: sx = 0.1 * dev_px_per_mm_x_; sy = -0.1 * dev_px_per_mm_y_; break;
    case 3: sx = 0.01 * dev_px_per_mm_x_; sy = -0.01 * dev_px_per_mm_y_; break;
    case 4: sx = 0.254 * dev_px_per_mm_x_; sy = -0.254 * dev_px_per_mm_y_; break;
    case 5: sx = 0.0254 * dev_px_per_mm_x_; sy = -0.0254 * dev_px_per_mm_y_; break;
    case 6: sx = 25.4 / 1440 * dev_px_per_mm_x_; sy = -25.4 / 1440 * dev_px_per_mm_y_; break;
    case 7:
    case 8: {
      sx = safe(s.viewport_ext.x, s.window_ext.x);
      sy = safe(s.viewport_ext.y, s.window_ext.y);
      if (s.map_mode == 7) {
        const double m = std::min(std::abs(sx), std::abs(sy));
        sx = std::copysign(m, sx);
        sy = std::copysign(m, sy);
      }
      break;
    }
    default: break;
  }
  mapping = Matrix::translate(s.viewport_org.x, s.viewport_org.y) * Matrix::scale(sx, sy) *
            Matrix::translate(-s.window_org.x, -s.window_org.y);
  to_page_ = device_to_page_ * mapping * s.world;
}

double GdiPlayer::device_pixel() const {
  if (flavor_ == Flavor::Wmf) return 1.0;
  const double s = device_to_page_.mean_scale();
  return std::isfinite(s) && s > 0 ? s : 1.0;
}

void GdiPlayer::set_map_mode(int mode) {
  if (mode < 1 || mode > 8) return;
  dc_.map_mode = mode;
  recompute();
}

void GdiPlayer::set_window_org(Point p) {
  dc_.window_org = p;
  recompute();
}

void GdiPlayer::set_window_ext(Point p) {
  if (p.x == 0 || p.y == 0 || !finite(p)) return;
  if (dc_.map_mode != 7 && dc_.map_mode != 8 && !(flavor_ == Flavor::Wmf && dc_.map_mode == 1)) return;
  dc_.window_ext = p;
  dc_.window_ext_set = true;
  if (flavor_ == Flavor::Wmf && !wmf_frame_set_) {
    // No placeable header: the first window becomes the page. Its units are
    // unknown; treat them as pixels, scaling very large windows down.
    double w = std::abs(p.x), h = std::abs(p.y);
    const double big = std::max(w, h);
    if (big > 2048) {
      w *= 1024 / big;
      h *= 1024 / big;
    }
    wmf_frame_set_ = true;
    wmf_frame_ = Rect::xywh(dc_.window_org.x, dc_.window_org.y, std::abs(p.x), std::abs(p.y));
    wmf_page_w_ = w;
    wmf_page_h_ = h;
  }
  recompute();
}

void GdiPlayer::set_viewport_org(Point p) {
  dc_.viewport_org = p;
  recompute();
}

void GdiPlayer::set_viewport_ext(Point p) {
  if (p.x == 0 || p.y == 0 || !finite(p)) return;
  if (dc_.map_mode != 7 && dc_.map_mode != 8) return;
  dc_.viewport_ext = p;
  recompute();
}

void GdiPlayer::offset_window_org(Point d) {
  dc_.window_org = dc_.window_org + d;
  recompute();
}

void GdiPlayer::offset_viewport_org(Point d) {
  dc_.viewport_org = dc_.viewport_org + d;
  recompute();
}

void GdiPlayer::scale_window_ext(double xn, double xd, double yn, double yd) {
  if (xd == 0 || yd == 0 || xn == 0 || yn == 0) return;
  set_window_ext({dc_.window_ext.x * xn / xd, dc_.window_ext.y * yn / yd});
}

void GdiPlayer::scale_viewport_ext(double xn, double xd, double yn, double yd) {
  if (xd == 0 || yd == 0 || xn == 0 || yn == 0) return;
  set_viewport_ext({dc_.viewport_ext.x * xn / xd, dc_.viewport_ext.y * yn / yd});
}

void GdiPlayer::set_world(const Matrix& m) {
  if (!std::isfinite(m.a) || !std::isfinite(m.b) || !std::isfinite(m.c) || !std::isfinite(m.d) ||
      !std::isfinite(m.e) || !std::isfinite(m.f)) {
    return;
  }
  dc_.world = m;
  recompute();
}

void GdiPlayer::modify_world(const Matrix& m, std::uint32_t mode) {
  switch (mode) {
    case 1: set_world(Matrix{}); break;
    case 2: set_world(dc_.world * m); break;  // MWT_LEFTMULTIPLY: m applies first
    case 3: set_world(m * dc_.world); break;  // MWT_RIGHTMULTIPLY
    case 4: set_world(m); break;              // MWT_SET
    default: break;
  }
}

// ---- objects

void GdiPlayer::ensure_table(std::size_t size) {
  size = std::min(size, max_objects);
  if (objects_.size() < size) objects_.resize(size);
}

Result<void> GdiPlayer::create_object(GdiObject object) {
  // Lowest free slot; every slot below `first_free_` is in use.
  for (std::size_t i = first_free_; i < objects_.size(); ++i) {
    if (std::holds_alternative<std::monostate>(objects_[i])) {
      objects_[i] = std::move(object);
      first_free_ = i + 1;
      return {};
    }
  }
  if (objects_.size() >= max_objects) return fail(Status::Limit, "the metafile creates too many objects");
  objects_.push_back(std::move(object));
  first_free_ = objects_.size();
  return {};
}

void GdiPlayer::create_object_at(std::uint32_t index, GdiObject object) {
  if (index >= max_objects) {
    note("Objects with out-of-range indices were ignored.");
    return;
  }
  if (objects_.size() <= index) objects_.resize(std::size_t{index} + 1);
  objects_[index] = std::move(object);
  first_free_ = 0;
}

const GdiObject* GdiPlayer::object(std::uint32_t index) const {
  if (index >= objects_.size()) return nullptr;
  return &objects_[index];
}

void GdiPlayer::select_object(std::uint32_t index) {
  if ((index & 0x80000000u) != 0) {
    switch (index & 0x7FFFFFFFu) {
      case 0: dc_.brush = GdiBrush{0, {1, 1, 1, 1}, 0, nullptr, false}; break;
      case 1: dc_.brush = GdiBrush{0, Color::rgb8(0xC0, 0xC0, 0xC0), 0, nullptr, false}; break;
      case 2: dc_.brush = GdiBrush{0, Color::rgb8(0x80, 0x80, 0x80), 0, nullptr, false}; break;
      case 3: dc_.brush = GdiBrush{0, Color::rgb8(0x40, 0x40, 0x40), 0, nullptr, false}; break;
      case 4: dc_.brush = GdiBrush{0, {0, 0, 0, 1}, 0, nullptr, false}; break;
      case 5: dc_.brush = GdiBrush{1, {0, 0, 0, 1}, 0, nullptr, false}; break;
      case 6: dc_.pen = GdiPen{0, 0.0, {1, 1, 1, 1}, {}, false, false}; break;
      case 7: dc_.pen = GdiPen{0, 0.0, {0, 0, 0, 1}, {}, false, false}; break;
      case 8: dc_.pen = GdiPen{5, 0.0, {0, 0, 0, 1}, {}, false, false}; break;
      case 10: case 11: case 12: case 13: case 14: case 16: case 17: {
        GdiFont f;
        f.face = (index & 0x7FFFFFFFu) == 10 || (index & 0x7FFFFFFFu) == 11 || (index & 0x7FFFFFFFu) == 16
                     ? "Courier New"
                     : "Arial";
        dc_.font = f;
        break;
      }
      case 15: dc_.palette = nullptr; break;
      case 18: dc_.brush = GdiBrush{0, {1, 1, 1, 1}, 0, nullptr, false}; break;
      case 19: dc_.pen = GdiPen{0, 0.0, {0, 0, 0, 1}, {}, false, false}; break;
      default: break;
    }
    return;
  }
  const GdiObject* obj = object(index);
  if (obj == nullptr || std::holds_alternative<std::monostate>(*obj)) {
    note("The metafile selects objects that do not exist; they were ignored.");
    return;
  }
  if (const auto* pen = std::get_if<GdiPen>(obj)) dc_.pen = *pen;
  if (const auto* brush = std::get_if<GdiBrush>(obj)) dc_.brush = *brush;
  if (const auto* font = std::get_if<GdiFont>(obj)) dc_.font = *font;
  if (const auto* palette = std::get_if<GdiPalette>(obj)) dc_.palette = std::make_shared<const GdiPalette>(*palette);
  if (const auto* region = std::get_if<GdiRegion>(obj)) select_clip_rects(region->rects, false, 5);
}

void GdiPlayer::delete_object(std::uint32_t index) {
  if (index < objects_.size()) {
    objects_[index] = std::monostate{};
    first_free_ = std::min<std::size_t>(first_free_, index);
  }
}

void GdiPlayer::save_dc() {
  if (saved_.size() >= max_saved_states) {
    note("The metafile saves its drawing state too deeply; extra saves were ignored.");
    return;
  }
  saved_.push_back(dc_);
}

void GdiPlayer::restore_dc(int relative) {
  std::int64_t target = relative < 0 ? static_cast<std::int64_t>(saved_.size()) + relative : relative - 1;
  if (target < 0 || target >= static_cast<std::int64_t>(saved_.size())) return;
  dc_ = saved_[static_cast<std::size_t>(target)];
  saved_.resize(static_cast<std::size_t>(target));
  recompute();
}

// ---- paint

bool GdiPlayer::pen_visible() const { return (dc_.pen.style & 0x0F) != 5 && dc_.rop2 != 11; }

Color GdiPlayer::rop2_color(Color c) {
  switch (dc_.rop2) {
    case 13: return c;
    case 1: return {0, 0, 0, 1};
    case 16: return {1, 1, 1, 1};
    case 4: return {1 - c.r, 1 - c.g, 1 - c.b, c.a};  // R2_NOTCOPYPEN
    default:
      note("Some raster operations were approximated by plain drawing.");
      return c;
  }
}

Stroke GdiPlayer::pen_stroke() {
  Stroke s;
  if (!pen_visible()) return s;
  const GdiPen& pen = dc_.pen;
  s.paint = Paint::solid(rop2_color(pen.color));
  const double scale = to_page_.mean_scale();
  const double dev = device_pixel();
  const bool hair = pen.cosmetic || pen.width <= 0;
  double width = hair ? dev : pen.width * scale;
  if (!std::isfinite(width) || width <= 0) width = dev;
  if (width < dev) {
    width = dev;
    s.hairline = true;
  }
  if (hair) s.hairline = true;
  s.width = width;
  switch (pen.style & 0x0F00) {
    case 0x100: s.cap = LineCap::Square; break;
    case 0x200: s.cap = LineCap::Butt; break;
    default: s.cap = LineCap::Round; break;
  }
  switch (pen.style & 0xF000) {
    case 0x1000: s.join = LineJoin::Bevel; break;
    case 0x2000: s.join = LineJoin::Miter; break;
    default: s.join = LineJoin::Round; break;
  }
  s.miter_limit = std::clamp(dc_.miter_limit, 1.0, 100.0);
  const bool wide = width > dev * 1.5;
  const double u = wide ? width : dev;
  switch (pen.style & 0x0F) {
    case 1: s.dash = wide ? std::vector<double>{3 * u, u} : std::vector<double>{18 * u, 6 * u}; break;
    case 2: s.dash = wide ? std::vector<double>{u, u} : std::vector<double>{3 * u, 3 * u}; break;
    case 3: s.dash = wide ? std::vector<double>{3 * u, u, u, u} : std::vector<double>{9 * u, 6 * u, 3 * u, 6 * u}; break;
    case 4:
      s.dash = wide ? std::vector<double>{3 * u, u, u, u, u, u}
                    : std::vector<double>{9 * u, 3 * u, 3 * u, 3 * u, 3 * u, 3 * u};
      break;
    case 7: {
      double sum = 0;
      for (const double d : pen.user_dash) {
        const double v = std::isfinite(d) ? std::max(0.0, d) * u : 0.0;
        s.dash.push_back(v);
        sum += v;
      }
      if (s.dash.size() % 2 == 1) {
        const auto copy = s.dash;
        s.dash.insert(s.dash.end(), copy.begin(), copy.end());
      }
      if (!(sum > 0)) s.dash.clear();
      break;
    }
    case 8: s.dash = {dev, dev}; break;
    default: break;
  }
  if (s.dash.size() > 64) s.dash.resize(64);
  if (!s.dash.empty() && s.cap == LineCap::Round && !wide) s.cap = LineCap::Butt;
  return s;
}

Paint GdiPlayer::hatch_paint(std::uint32_t hatch, Color color, bool opaque_bk, Color bk) {
  auto content = std::make_shared<Group>();
  const auto fill = [&](Path p, Color c) {
    Shape shape;
    shape.path = std::move(p);
    shape.fill = Paint::solid(c);
    content->children.push_back(Node{std::move(shape)});
  };
  if (opaque_bk) {
    Path p;
    add_rect(p, 0, 0, 8, 8);
    fill(std::move(p), bk);
  }
  const auto horizontal = [&] {
    Path p;
    add_rect(p, 0, 3.5, 8, 1);
    fill(std::move(p), color);
  };
  const auto vertical = [&] {
    Path p;
    add_rect(p, 3.5, 0, 1, 8);
    fill(std::move(p), color);
  };
  const auto diagonal = [&](bool forward) {
    Path p;
    if (forward) {  // "\" from top-left to bottom-right
      p.move_to({0, 0});
      p.line_to({0.7, 0});
      p.line_to({8, 7.3});
      p.line_to({8, 8});
      p.line_to({7.3, 8});
      p.line_to({0, 0.7});
    } else {  // "/"
      p.move_to({8, 0});
      p.line_to({8, 0.7});
      p.line_to({0.7, 8});
      p.line_to({0, 8});
      p.line_to({0, 7.3});
      p.line_to({7.3, 0});
    }
    p.close();
    fill(std::move(p), color);
  };
  switch (hatch) {
    case 0: horizontal(); break;
    case 1: vertical(); break;
    case 2: diagonal(true); break;
    case 3: diagonal(false); break;
    case 4: horizontal(); vertical(); break;
    default: diagonal(true); diagonal(false); break;
  }
  auto pattern = std::make_shared<PatternPaint>();
  pattern->tile = Rect::xywh(0, 0, 8, 8);
  const double dev = device_pixel();
  pattern->transform = Matrix::scale(dev, dev);
  pattern->content = std::move(content);
  Paint paint;
  paint.type = Paint::Type::Pattern;
  paint.pattern = std::move(pattern);
  return paint;
}

Paint GdiPlayer::picture_pattern(const FramePtr& frame) {
  if (!frame || !frame->valid()) return Paint::solid({0.5f, 0.5f, 0.5f, 1});
  auto content = std::make_shared<Group>();
  Picture pic;
  pic.pixels = frame;
  pic.bounds = Rect::xywh(0, 0, frame->width, frame->height);
  pic.smooth = false;
  content->children.push_back(Node{std::move(pic)});
  auto pattern = std::make_shared<PatternPaint>();
  pattern->tile = Rect::xywh(0, 0, frame->width, frame->height);
  const double dev = device_pixel();
  pattern->transform = Matrix::scale(dev, dev);
  pattern->content = std::move(content);
  Paint paint;
  paint.type = Paint::Type::Pattern;
  paint.pattern = std::move(pattern);
  return paint;
}

Paint GdiPlayer::brush_paint(const GdiBrush& brush) {
  if (dc_.rop2 == 11) return Paint::none();
  switch (brush.style) {
    case 0: return Paint::solid(rop2_color(brush.color));
    case 1: return Paint::none();
    case 2: return hatch_paint(brush.hatch, rop2_color(brush.color), dc_.bk_mode == 2, dc_.bk_color);
    case 3: case 5: case 6: case 8: case 9: return picture_pattern(brush.pattern);
    default: return Paint::solid(rop2_color(brush.color));
  }
}

// ---- emission

Result<void> GdiPlayer::emit(Path page_path, const Paint& fill, FillRule rule, const Stroke& stroke,
                             const ClipRef& clip) {
  if (!drawing_enabled_) return {};
  if (!fill.visible() && !stroke.paint.visible()) return {};
  if (page_path.empty() || !path_finite(page_path)) return {};
  Shape shape;
  shape.path = std::move(page_path);
  shape.fill = fill;
  shape.fill_rule = rule;
  shape.stroke = stroke;
  return out_.add_shape(std::move(shape), clip);
}

Result<void> GdiPlayer::flush_pending() {
  if (pending_line_.empty()) return {};
  Path p = std::move(pending_line_);
  pending_line_.clear();
  return emit(std::move(p), Paint::none(), FillRule::NonZero, pending_stroke_, pending_clip_);
}

Result<void> GdiPlayer::set_drawing_enabled(bool on) {
  if (on == drawing_enabled_) return {};
  CVT_TRY(flush_pending());
  drawing_enabled_ = on;
  return {};
}

Result<Group> GdiPlayer::finish() {
  CVT_TRY(flush_pending());
  return out_.take_root();
}

void GdiPlayer::add_to_path(const Path& logical) {
  const Path page = transformed(logical, to_page_);
  if (!page.verbs.empty() && page.verbs.front() == Verb::Move && !path_.empty() && path_.verbs.back() != Verb::Close) {
    const Point cur = current_point(path_);
    const Point start = page.points.front();
    if (std::abs(cur.x - start.x) < 1e-9 && std::abs(cur.y - start.y) < 1e-9) {
      // Continue the open figure.
      Path rest;
      rest.verbs.assign(page.verbs.begin() + 1, page.verbs.end());
      rest.points.assign(page.points.begin() + 1, page.points.end());
      append(path_, rest);
      return;
    }
  }
  append(path_, page);
}

Result<void> GdiPlayer::draw_closed(Path logical, bool fill, bool stroke) {
  if (in_path_) {
    add_to_path(logical);
    return {};
  }
  CVT_TRY(flush_pending());
  const Paint f = fill ? brush_paint(dc_.brush) : Paint::none();
  const Stroke s = stroke ? pen_stroke() : Stroke{};
  return emit(transformed(logical, to_page_), f, fill_rule(), s, dc_.clip);
}

Result<void> GdiPlayer::draw_open(Path logical) {
  if (in_path_) {
    add_to_path(logical);
    return {};
  }
  CVT_TRY(flush_pending());
  return emit(transformed(logical, to_page_), Paint::none(), FillRule::NonZero, pen_stroke(), dc_.clip);
}

// ---- drawing

Result<void> GdiPlayer::move_to(Point p) {
  dc_.position = p;
  if (in_path_) path_.move_to(to_page_.apply(p));
  return {};
}

Result<void> GdiPlayer::line_to(Point p) {
  const Point from = dc_.position;
  dc_.position = p;
  if (in_path_) {
    Path l;
    l.move_to(from);
    l.line_to(p);
    add_to_path(l);
    return {};
  }
  if (!drawing_enabled_ || !pen_visible()) return {};
  const Point a = to_page_.apply(from), b = to_page_.apply(p);
  if (!finite(a) || !finite(b)) return {};
  CVT_TRY(out_.charge_points(1));
  Stroke stroke = pen_stroke();
  if (!pending_line_.empty() && (!same_stroke(stroke, pending_stroke_) || pending_clip_ != dc_.clip)) {
    CVT_TRY(flush_pending());
  }
  if (pending_line_.empty()) {
    pending_stroke_ = std::move(stroke);
    pending_clip_ = dc_.clip;
  }
  if (!pending_line_.empty() && current_point(pending_line_) == a) {
    pending_line_.line_to(b);
  } else {
    pending_line_.move_to(a);
    pending_line_.line_to(b);
  }
  return {};
}

Result<void> GdiPlayer::polyline(std::span<const Point> points) {
  if (points.size() < 2) return {};
  Path p;
  p.move_to(points[0]);
  for (std::size_t i = 1; i < points.size(); ++i) p.line_to(points[i]);
  return draw_open(std::move(p));
}

Result<void> GdiPlayer::polygon(std::span<const Point> points) {
  if (points.size() < 2) return {};
  Path p;
  p.move_to(points[0]);
  for (std::size_t i = 1; i < points.size(); ++i) p.line_to(points[i]);
  p.close();
  return draw_closed(std::move(p), true, true);
}

Result<void> GdiPlayer::poly_polygon(const std::vector<std::vector<Point>>& polygons) {
  Path p;
  for (const auto& poly : polygons) {
    if (poly.size() < 2) continue;
    p.move_to(poly[0]);
    for (std::size_t i = 1; i < poly.size(); ++i) p.line_to(poly[i]);
    p.close();
  }
  if (p.empty()) return {};
  return draw_closed(std::move(p), true, true);
}

Result<void> GdiPlayer::poly_polyline(const std::vector<std::vector<Point>>& lines) {
  Path p;
  for (const auto& line : lines) {
    if (line.size() < 2) continue;
    p.move_to(line[0]);
    for (std::size_t i = 1; i < line.size(); ++i) p.line_to(line[i]);
  }
  if (p.empty()) return {};
  return draw_open(std::move(p));
}

Result<void> GdiPlayer::polybezier(std::span<const Point> points) {
  if (points.size() < 4) return {};
  Path p;
  p.move_to(points[0]);
  for (std::size_t i = 1; i + 2 < points.size(); i += 3) p.cubic_to(points[i], points[i + 1], points[i + 2]);
  return draw_open(std::move(p));
}

Result<void> GdiPlayer::polybezier_to(std::span<const Point> points) {
  if (points.size() < 3) return {};
  Path p;
  p.move_to(dc_.position);
  std::size_t i = 0;
  for (; i + 2 < points.size(); i += 3) p.cubic_to(points[i], points[i + 1], points[i + 2]);
  dc_.position = points[i - 1];
  return draw_open(std::move(p));
}

Result<void> GdiPlayer::polyline_to(std::span<const Point> points) {
  if (points.empty()) return {};
  Path p;
  p.move_to(dc_.position);
  for (const Point q : points) p.line_to(q);
  dc_.position = points.back();
  return draw_open(std::move(p));
}

Result<void> GdiPlayer::poly_draw(std::span<const Point> points, std::span<const std::uint8_t> types) {
  Path p;
  p.move_to(dc_.position);
  Point figure_start = dc_.position;
  const std::size_t n = std::min(points.size(), types.size());
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t t = types[i] & 0x06;
    if (t == 0x06) {
      p.move_to(points[i]);
      figure_start = points[i];
    } else if (t == 0x04 && i + 2 < n) {
      p.cubic_to(points[i], points[i + 1], points[i + 2]);
      i += 2;
    } else {
      p.line_to(points[i]);
    }
    if ((types[std::min(i, n - 1)] & 0x01) != 0) {
      p.close();
      p.move_to(figure_start);
    }
  }
  if (n > 0) dc_.position = points[n - 1];
  return draw_open(std::move(p));
}

Result<void> GdiPlayer::rectangle(Rect r) {
  Path p;
  add_rect_points(p, Matrix{}, normalized(r));
  return draw_closed(std::move(p), true, true);
}

Result<void> GdiPlayer::round_rect(Rect r, double corner_w, double corner_h) {
  const Rect n = normalized(r);
  Path p;
  add_rect(p, n.x0, n.y0, n.width(), n.height(), std::abs(corner_w) / 2, std::abs(corner_h) / 2);
  return draw_closed(std::move(p), true, true);
}

Result<void> GdiPlayer::ellipse(Rect r) {
  const Rect n = normalized(r);
  if (n.width() <= 0 || n.height() <= 0) return {};
  Path p;
  add_ellipse(p, (n.x0 + n.x1) / 2, (n.y0 + n.y1) / 2, n.width() / 2, n.height() / 2);
  return draw_closed(std::move(p), true, true);
}

Path GdiPlayer::arc_path(Rect box, Point start, Point end, ArcKind kind, bool& ok) {
  Path p;
  const Rect n = normalized(box);
  const double rx = n.width() / 2, ry = n.height() / 2;
  ok = rx > 0 && ry > 0 && std::isfinite(rx) && std::isfinite(ry);
  if (!ok) return p;
  const Point c{(n.x0 + n.x1) / 2, (n.y0 + n.y1) / 2};
  const double a0 = std::atan2((start.y - c.y) / ry, (start.x - c.x) / rx);
  double a1 = std::atan2((end.y - c.y) / ry, (end.x - c.x) / rx);
  const bool flip = to_device().determinant() < 0;
  const bool positive = dc_.arc_clockwise != flip;  // counter-clockwise on a y-down device = decreasing angle
  if (std::abs(a1 - a0) < 1e-12) a1 = positive ? a0 + 2 * pi : a0 - 2 * pi;
  switch (kind) {
    case ArcKind::Arc: add_arc(p, c, rx, ry, 0, a0, a1, positive, false); break;
    case ArcKind::ArcTo:
      p.move_to(dc_.position);
      add_arc(p, c, rx, ry, 0, a0, a1, positive, true);
      break;
    case ArcKind::Pie:
      p.move_to(c);
      add_arc(p, c, rx, ry, 0, a0, a1, positive, true);
      p.close();
      break;
    case ArcKind::Chord:
      add_arc(p, c, rx, ry, 0, a0, a1, positive, false);
      p.close();
      break;
  }
  return p;
}

Result<void> GdiPlayer::arc(Rect box, Point start, Point end, ArcKind kind) {
  bool ok = false;
  Path p = arc_path(box, start, end, kind, ok);
  if (!ok) return {};
  if (kind == ArcKind::ArcTo) dc_.position = current_point(p);
  if (kind == ArcKind::Arc || kind == ArcKind::ArcTo) return draw_open(std::move(p));
  return draw_closed(std::move(p), true, true);
}

Result<void> GdiPlayer::angle_arc(Point center, double radius, double start_deg, double sweep_deg) {
  if (!std::isfinite(radius) || !std::isfinite(start_deg) || !std::isfinite(sweep_deg)) return {};
  start_deg = std::fmod(start_deg, 360.0);
  sweep_deg = std::clamp(sweep_deg, -360.0, 360.0);
  const double a0 = -start_deg * pi / 180, a1 = -(start_deg + sweep_deg) * pi / 180;
  Path p;
  p.move_to(dc_.position);
  const double r = std::abs(radius);
  if (r > 0 && sweep_deg != 0) {
    add_arc(p, center, r, r, 0, a0, a1, sweep_deg < 0, true);
  } else {
    p.line_to({center.x + r * std::cos(a0), center.y + r * std::sin(a0)});
  }
  dc_.position = current_point(p);
  return draw_open(std::move(p));
}

Result<void> GdiPlayer::set_pixel(Point p, Color c) {
  if (in_path_ || !drawing_enabled_) return {};
  CVT_TRY(flush_pending());
  const Point q = to_page_.apply(p);
  const double d = device_pixel();
  Path path;
  add_rect(path, q.x, q.y, d, d);
  return emit(std::move(path), Paint::solid(c), FillRule::NonZero, Stroke{}, dc_.clip);
}

// ---- paths

void GdiPlayer::begin_path() {
  (void)flush_pending();
  in_path_ = true;
  path_.clear();
}

void GdiPlayer::end_path() { in_path_ = false; }

void GdiPlayer::close_figure() {
  if (in_path_ && !path_.empty() && path_.verbs.back() != Verb::Close) path_.close();
}

void GdiPlayer::abort_path() {
  in_path_ = false;
  path_.clear();
}

Result<void> GdiPlayer::fill_path() {
  in_path_ = false;
  Path p = std::move(path_);
  path_.clear();
  return emit(std::move(p), brush_paint(dc_.brush), fill_rule(), Stroke{}, dc_.clip);
}

Result<void> GdiPlayer::stroke_path() {
  in_path_ = false;
  Path p = std::move(path_);
  path_.clear();
  return emit(std::move(p), Paint::none(), FillRule::NonZero, pen_stroke(), dc_.clip);
}

Result<void> GdiPlayer::stroke_and_fill_path() {
  in_path_ = false;
  Path p = std::move(path_);
  path_.clear();
  return emit(std::move(p), brush_paint(dc_.brush), fill_rule(), pen_stroke(), dc_.clip);
}

void GdiPlayer::select_clip_path(int mode) {
  in_path_ = false;
  Path p = std::move(path_);
  path_.clear();
  if (p.empty() || !path_finite(p)) return;
  bool approximated = false;
  dc_.clip = combine_clip(dc_.clip, make_clip(std::move(p), fill_rule()), mode, approximated);
  if (approximated) note("Some clipping regions were approximated.");
}

// ---- clipping

void GdiPlayer::intersect_clip_rect(Rect logical) {
  (void)flush_pending();
  Path p;
  add_rect_points(p, to_page_, normalized(logical));
  if (!path_finite(p)) return;
  dc_.clip = clip_intersect(dc_.clip, std::move(p), FillRule::NonZero);
}

void GdiPlayer::exclude_clip_rect(Rect logical) {
  (void)flush_pending();
  Path p;
  add_rect_points(p, to_page_, normalized(logical));
  if (!path_finite(p)) return;
  dc_.clip = clip_exclude(dc_.clip, std::move(p));
}

void GdiPlayer::select_clip_rects(const std::vector<Rect>& rects, bool device_units, int mode) {
  (void)flush_pending();
  if (rects.empty()) {
    if (mode == 5) dc_.clip = nullptr;  // a null region resets the clip
    return;
  }
  const Matrix& m = device_units ? device_to_page_ : to_page_;
  Path p;
  for (const Rect& r : rects) add_rect_points(p, m, normalized(r));
  if (!path_finite(p)) return;
  bool approximated = false;
  dc_.clip = combine_clip(dc_.clip, make_clip(std::move(p), FillRule::NonZero), mode, approximated);
  if (approximated) note("Some clipping regions were approximated.");
}

void GdiPlayer::offset_clip(Point logical_delta) {
  if (!dc_.clip) return;
  const Point d = to_page_.apply_vector(logical_delta);
  dc_.clip = transform_clip(dc_.clip, Matrix::translate(d.x, d.y));
}

// ---- regions and gradients

Result<void> GdiPlayer::fill_rects(const std::vector<Rect>& rects, bool device_units, const GdiBrush& brush) {
  if (rects.empty()) return {};
  CVT_TRY(flush_pending());
  const Matrix& m = device_units ? device_to_page_ : to_page_;
  Path p;
  for (const Rect& r : rects) add_rect_points(p, m, normalized(r));
  return emit(std::move(p), brush_paint(brush), FillRule::NonZero, Stroke{}, dc_.clip);
}

Result<void> GdiPlayer::frame_rects(const std::vector<Rect>& rects, bool device_units, const GdiBrush& brush, double w,
                                    double h) {
  if (rects.empty()) return {};
  CVT_TRY(flush_pending());
  const Matrix& m = device_units ? device_to_page_ : to_page_;
  Path p;
  for (const Rect& r : rects) add_rect_points(p, m, normalized(r));
  Stroke s;
  s.paint = brush_paint(brush);
  if (s.paint.type == Paint::Type::Pattern) s.paint = Paint::solid(brush.color);
  const double scale = device_units ? device_pixel() : to_page_.mean_scale();
  s.width = std::max(device_pixel(), (std::abs(w) + std::abs(h)) / 2 * scale);
  s.join = LineJoin::Miter;
  return emit(std::move(p), Paint::none(), FillRule::NonZero, s, dc_.clip);
}

Result<void> GdiPlayer::gradient_rect(Rect logical, Color c0, Color c1, bool vertical) {
  if (in_path_ || !drawing_enabled_) return {};
  CVT_TRY(flush_pending());
  const Rect n = logical;
  Path p;
  add_rect_points(p, to_page_, normalized(n));
  auto g = std::make_shared<Gradient>();
  g->type = Gradient::Type::Linear;
  g->p1 = to_page_.apply({n.x0, n.y0});
  g->p2 = to_page_.apply(vertical ? Point{n.x0, n.y1} : Point{n.x1, n.y0});
  g->stops = {{0.0, c0}, {1.0, c1}};
  Paint paint;
  paint.type = Paint::Type::Gradient;
  paint.gradient = std::move(g);
  return emit(std::move(p), paint, FillRule::NonZero, Stroke{}, dc_.clip);
}

Result<void> GdiPlayer::gradient_triangle(std::span<const Point> tri, std::span<const Color> colors) {
  if (in_path_ || !drawing_enabled_ || tri.size() < 3 || colors.size() < 3) return {};
  CVT_TRY(flush_pending());
  note("Triangle gradients were approximated by small flat triangles.");
  constexpr int n = 4;
  const auto at = [&](double u, double v) -> std::pair<Point, Color> {
    const double w = 1 - u - v;
    const Point p{tri[0].x * w + tri[1].x * u + tri[2].x * v, tri[0].y * w + tri[1].y * u + tri[2].y * v};
    const auto mix = [&](float a, float b, float c) { return static_cast<float>(a * w + b * u + c * v); };
    return {p, Color{mix(colors[0].r, colors[1].r, colors[2].r), mix(colors[0].g, colors[1].g, colors[2].g),
                     mix(colors[0].b, colors[1].b, colors[2].b), mix(colors[0].a, colors[1].a, colors[2].a)}};
  };
  const auto emit_tri = [&](std::pair<Point, Color> a, std::pair<Point, Color> b,
                            std::pair<Point, Color> c) -> Result<void> {
    Path p;
    p.move_to(to_page_.apply(a.first));
    p.line_to(to_page_.apply(b.first));
    p.line_to(to_page_.apply(c.first));
    p.close();
    const Color avg{(a.second.r + b.second.r + c.second.r) / 3, (a.second.g + b.second.g + c.second.g) / 3,
                    (a.second.b + b.second.b + c.second.b) / 3, 1.0f};
    Stroke seam;  // a hairline in the same colour hides anti-aliasing seams
    seam.paint = Paint::solid(avg);
    seam.width = device_pixel() * 0.5;
    return emit(std::move(p), Paint::solid(avg), FillRule::NonZero, seam, dc_.clip);
  };
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n - i; ++j) {
      const double u0 = static_cast<double>(i) / n, v0 = static_cast<double>(j) / n, d = 1.0 / n;
      CVT_TRY(emit_tri(at(u0, v0), at(u0 + d, v0), at(u0, v0 + d)));
      if (i + j < n - 1) CVT_TRY(emit_tri(at(u0 + d, v0), at(u0 + d, v0 + d), at(u0, v0 + d)));
    }
  }
  return {};
}

// ---- text

Result<void> GdiPlayer::text_out(const TextRun& run) {
  if (!drawing_enabled_ && !in_path_) return {};
  CVT_TRY(flush_pending());
  const Matrix& m = to_page_;
  const GdiFont& f = dc_.font;
  const bool update_cp = (dc_.text_align & 1) != 0;
  const Point ref = update_cp ? dc_.position : run.reference;

  // Opaque background rectangle.
  if (!in_path_ && run.rect && (run.options & 0x0002) != 0) {
    Path p;
    add_rect_points(p, m, normalized(*run.rect));
    CVT_TRY(emit(std::move(p), Paint::solid(dc_.bk_color), FillRule::NonZero, Stroke{}, dc_.clip));
  }
  if (run.chars.empty()) return {};
  if ((run.options & 0x0010) != 0) {
    note("Text stored as glyph indices cannot be shown; it was left out.");
    return {};
  }

  FontSpec spec;
  spec.family = f.face.empty() ? std::string("sans-serif") : f.face;
  spec.weight = std::clamp(f.weight == 0 ? 400 : f.weight, 100, 1000);
  spec.italic = f.italic;
  const double sx = std::hypot(m.a, m.b) * run.scale_x, sy = std::hypot(m.c, m.d);
  double size = 0;
  if (f.height < 0) {
    size = -f.height * sy;
  } else if (f.height > 0) {
    FontSpec unit = spec;
    unit.size = 100;
    const TextLayout probe = layout_text("", unit);
    const double cell = (probe.ascent + probe.descent) / 100;
    size = f.height * sy / (cell > 0.5 ? cell : 1.2);
  } else {
    size = 16 * device_pixel();
  }
  if (!std::isfinite(size) || size <= 0 || size > 1e6) {
    note("Text with an unusable font size was left out.");
    return {};
  }
  spec.size = size;

  // Characters as UTF-8 (Symbol fonts mapped to real symbols).
  const bool symbol = util::iequals(f.face, "Symbol");
  std::u32string chars = run.chars;
  for (char32_t& c : chars) {
    if (symbol && ((c >= 0xF020 && c <= 0xF0FF) || (c >= 0x20 && c <= 0xFF))) {
      c = symbol_to_unicode(static_cast<std::uint8_t>(c & 0xFF));
    }
    if (!util::is_scalar_value(c)) c = U'�';
  }
  const std::string utf8 = util::utf32_to_utf8(chars);
  CVT_TRY(out_.charge_points(chars.size() * 16));

  Path outline;
  double advance = 0;
  double ascent = 0, descent = 0;
  if (!run.dx.empty()) {
    double pen_x = 0, pen_y = 0;
    for (std::size_t i = 0; i < chars.size(); ++i) {
      const std::string one = util::utf32_to_utf8(std::u32string_view(&chars[i], 1));
      const TextLayout glyph = layout_text(one, spec);
      ascent = glyph.ascent;
      descent = glyph.descent;
      append(outline, glyph.outline, Matrix::translate(pen_x, pen_y));
      const double dx = i < run.dx.size() ? run.dx[i] : glyph.advance / std::max(sx, 1e-12);
      pen_x += dx * sx;
      if (i < run.dy.size()) pen_y -= run.dy[i] * sy;
    }
    advance = pen_x;
  } else {
    const TextLayout layout = layout_text(utf8, spec, dc_.char_extra * sx);
    outline = layout.outline;
    advance = layout.advance;
    ascent = layout.ascent;
    descent = layout.descent;
  }
  if (ascent == 0 && descent == 0) {
    const TextLayout probe = layout_text("", spec);
    ascent = probe.ascent;
    descent = probe.descent;
  }

  double off_x = 0, off_y = 0;
  switch (dc_.text_align & 6) {
    case 2: off_x = -advance; break;
    case 6: off_x = -advance / 2; break;
    default: break;
  }
  switch (dc_.text_align & 24) {
    case 0: off_y = ascent; break;
    case 8: off_y = -descent; break;
    default: break;
  }
  const Point origin = m.apply(ref);
  if (!finite(origin)) return {};
  const double axis = std::atan2(m.b, m.a);  // direction of the logical x axis on the page
  const double angle = (m.determinant() < 0 ? 0.0 : axis) - f.escapement / 10.0 * pi / 180.0;
  const Matrix place = Matrix::translate(origin.x, origin.y) * Matrix::rotate(std::abs(angle) < 1e-9 ? 0.0 : angle);
  const bool rotated = std::abs(angle) >= 1e-9;

  if (update_cp) {
    const Point step = m.inverted().apply_vector(Matrix::rotate(rotated ? angle : 0.0).apply_vector({advance, 0}));
    dc_.position = dc_.position + step;
  }

  if (in_path_) {
    Path page = transformed(outline, place * Matrix::translate(off_x, off_y));
    append(path_, page);
    return {};
  }

  ClipRef clip = dc_.clip;
  if (run.rect && (run.options & 0x0004) != 0) {
    Path p;
    add_rect_points(p, m, normalized(*run.rect));
    clip = clip_intersect(clip, std::move(p), FillRule::NonZero);
  }

  // Opaque text background (OPAQUE background mode without ETO_OPAQUE).
  std::vector<Node> nodes;
  if (dc_.bk_mode == 2 && (run.options & 0x0002) == 0) {
    Shape bg;
    add_rect(bg.path, off_x, off_y - ascent, advance, ascent + descent);
    bg.fill = Paint::solid(dc_.bk_color);
    nodes.push_back(Node{std::move(bg)});
  }
  Text text;
  text.content = utf8;
  text.font = spec;
  text.origin = {off_x, off_y};
  text.advance = advance;
  text.outline = transformed(outline, Matrix::translate(off_x, off_y));
  text.fill = Paint::solid(dc_.text_color);
  nodes.push_back(Node{std::move(text)});
  const auto deco = [&](double y, double thickness) {
    Shape s;
    add_rect(s.path, off_x, off_y + y, advance, thickness);
    s.fill = Paint::solid(dc_.text_color);
    nodes.push_back(Node{std::move(s)});
  };
  if (f.underline) deco(size * 0.1, std::max(size * 0.06, device_pixel()));
  if (f.strikeout) deco(-ascent * 0.32, std::max(size * 0.06, device_pixel()));

  if (rotated) {
    Group g;
    g.transform = place;
    g.children = std::move(nodes);
    return out_.add(Node{std::move(g)}, clip);
  }
  const Matrix shift = Matrix::translate(origin.x, origin.y);
  for (Node& node : nodes) {
    if (auto* t = std::get_if<Text>(&node.value)) {
      t->outline = transformed(t->outline, shift);
      t->origin = shift.apply(t->origin);
    } else if (auto* s = std::get_if<Shape>(&node.value)) {
      s->path = transformed(s->path, shift);
    }
    CVT_TRY(out_.add(std::move(node), clip));
  }
  return {};
}

// ---- bitmaps

RopEffect GdiPlayer::classify_rop(std::uint32_t rop) {
  switch ((rop >> 16) & 0xFF) {
    case 0xCC: case 0x33: case 0x44: case 0x11: case 0xC0: return RopEffect::Source;
    case 0xEE: case 0xBB: return RopEffect::SourcePaint;
    case 0x88: return RopEffect::SourceAnd;
    case 0x66: return RopEffect::SourceInvert;
    case 0xF0: case 0xFB: case 0x5A: return RopEffect::Pattern;
    case 0x00: return RopEffect::Black;
    case 0xFF: return RopEffect::White;
    case 0xAA: case 0x55: return RopEffect::Nothing;
    default: {
      const std::uint32_t code = (rop >> 16) & 0xFF;
      // Does the result depend on the source (S) or the pattern (P)?
      const bool uses_source = ((code >> 2) & 0x33) != (code & 0x33);
      const bool uses_pattern = ((code >> 4) & 0x0F) != (code & 0x0F);
      if (uses_source) return RopEffect::Source;
      if (uses_pattern) return RopEffect::Pattern;
      return RopEffect::Nothing;
    }
  }
}

Result<void> GdiPlayer::pat_blt(Rect dest, std::uint32_t rop) {
  if (in_path_ || !drawing_enabled_) return {};
  CVT_TRY(flush_pending());
  const RopEffect effect = classify_rop(rop);
  Paint paint;
  switch (effect) {
    case RopEffect::Black: paint = Paint::solid({0, 0, 0, 1}); break;
    case RopEffect::White: paint = Paint::solid({1, 1, 1, 1}); break;
    case RopEffect::Pattern:
      if (((rop >> 16) & 0xFF) != 0xF0) note("Some raster operations were approximated by plain drawing.");
      paint = brush_paint(dc_.brush);
      break;
    case RopEffect::Nothing:
      if (((rop >> 16) & 0xFF) == 0x55) note("Inverting raster operations were left out.");
      return {};
    default:
      note("Some raster operations were approximated by plain drawing.");
      paint = brush_paint(dc_.brush);
      break;
  }
  Path p;
  add_rect_points(p, to_page_, normalized(Rect{dest.x0, dest.y0, dest.x0 + dest.x1, dest.y0 + dest.y1}));
  return emit(std::move(p), paint, FillRule::NonZero, Stroke{}, dc_.clip);
}

Result<void> GdiPlayer::draw_bitmap(const FramePtr& frame, Rect d, std::uint32_t rop) {
  return draw_bitmap(frame, {d.x0, d.y0}, {d.x0 + d.x1, d.y0}, {d.x0, d.y0 + d.y1}, rop);
}

Result<void> GdiPlayer::draw_bitmap(const FramePtr& frame_in, Point p0, Point p1, Point p2, std::uint32_t rop) {
  if (in_path_ || !drawing_enabled_ || !frame_in || !frame_in->valid()) return {};
  CVT_TRY(flush_pending());
  const RopEffect effect = classify_rop(rop);
  const Point q0 = to_page_.apply(p0), q1 = to_page_.apply(p1), q2 = to_page_.apply(p2);
  if (!finite(q0) || !finite(q1) || !finite(q2)) return {};
  const auto parallelogram = [&] {
    Path p;
    p.move_to(q0);
    p.line_to(q1);
    p.line_to(q1 + (q2 - q0));
    p.line_to(q2);
    p.close();
    return p;
  };
  FramePtr frame = frame_in;
  switch (effect) {
    case RopEffect::Nothing:
      if (((rop >> 16) & 0xFF) == 0x55) note("Inverting raster operations were left out.");
      return {};
    case RopEffect::Black:
      return emit(parallelogram(), Paint::solid({0, 0, 0, 1}), FillRule::NonZero, Stroke{}, dc_.clip);
    case RopEffect::White:
      return emit(parallelogram(), Paint::solid({1, 1, 1, 1}), FillRule::NonZero, Stroke{}, dc_.clip);
    case RopEffect::Pattern:
      return emit(parallelogram(), brush_paint(dc_.brush), FillRule::NonZero, Stroke{}, dc_.clip);
    case RopEffect::SourceAnd:
    case RopEffect::SourcePaint:
    case RopEffect::SourceInvert: {
      // AND keeps the background under white, OR and XOR keep it under black:
      // show the picture with those pixels transparent.
      note("Masking raster operations were approximated with transparency.");
      auto copy = std::make_shared<ir::Frame>(*frame);
      const bool white_clear = effect == RopEffect::SourceAnd;
      for (std::size_t i = 0; i < copy->rgba.size(); i += 4) {
        const std::uint8_t r = copy->rgba[i], g = copy->rgba[i + 1], b = copy->rgba[i + 2];
        const bool clear = white_clear ? (r >= 250 && g >= 250 && b >= 250) : (r <= 5 && g <= 5 && b <= 5);
        if (clear) copy->rgba[i + 3] = 0;
      }
      frame = std::move(copy);
      break;
    }
    case RopEffect::Source:
      if (((rop >> 16) & 0xFF) != 0xCC) note("Some raster operations were approximated by plain drawing.");
      break;
  }
  const double w = frame->width, h = frame->height;
  Picture pic;
  pic.pixels = frame;
  if (std::abs(q1.y - q0.y) < 1e-9 && std::abs(q2.x - q0.x) < 1e-9 && q1.x > q0.x && q2.y > q0.y) {
    pic.bounds = Rect{q0.x, q0.y, q1.x, q2.y};
    return out_.add(Node{std::move(pic)}, dc_.clip);
  }
  const Point u = q1 - q0, v = q2 - q0;
  if (std::abs(cross(u, v)) < 1e-12) return {};
  Group g;
  g.transform = Matrix{u.x / w, u.y / w, v.x / h, v.y / h, q0.x, q0.y};
  pic.bounds = Rect::xywh(0, 0, w, h);
  g.children.push_back(Node{std::move(pic)});
  return out_.add(Node{std::move(g)}, dc_.clip);
}

}  // namespace cvt::vector::metafile
