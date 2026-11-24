// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// Raster -> vector: embedding and tracing (IoU against the source bitmap, node counts).
#include <cmath>

#include "families/vector/geom.hpp"
#include "families/vector/trace.hpp"
#include "families/vector/render.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

Context make_ctx() {
  Context ctx;
  ctx.registry = &Registry::instance();
  return ctx;
}

// A disc, a square and a triangle on white (hard edges).
ir::Frame shapes_frame(std::uint32_t w, std::uint32_t h) {
  ir::Frame f = ir::Frame::filled(w, h, {255, 255, 255, 255});
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const double px = x + 0.5, py = y + 0.5;
      std::array<std::uint8_t, 3> c{255, 255, 255};
      if (std::hypot(px - 30, py - 30) < 22) c = {200, 30, 30};
      if (px > 60 && px < 100 && py > 10 && py < 50) c = {30, 60, 200};
      if (py > 60 && py < 95 && std::abs(px - 45) < (py - 60)) c = {30, 160, 60};
      std::copy(c.begin(), c.end(), f.rgba.begin() + static_cast<std::ptrdiff_t>(f.offset(x, y)));
    }
  }
  return f;
}

bool dark(const ir::Frame& f, std::uint32_t x, std::uint32_t y) {
  const std::size_t o = f.offset(x, y);
  return f.rgba[o + 3] >= 128 && (f.rgba[o] + f.rgba[o + 1] + f.rgba[o + 2]) < 3 * 128;
}

// Intersection over union of the "dark" pixels of two frames.
double iou(const ir::Frame& a, const ir::Frame& b) {
  std::size_t inter = 0, uni = 0;
  for (std::uint32_t y = 0; y < a.height; ++y) {
    for (std::uint32_t x = 0; x < a.width; ++x) {
      const bool p = dark(a, x, y), q = dark(b, x, y);
      inter += p && q;
      uni += p || q;
    }
  }
  return uni == 0 ? 1.0 : static_cast<double>(inter) / static_cast<double>(uni);
}

std::size_t segment_count(const Scene& s) {
  std::size_t n = 0;
  for (const Node& node : s.pages[0].root.children) {
    if (const auto* shape = std::get_if<Shape>(&node.value)) n += shape->path.verbs.size();
  }
  return n;
}

Result<ir::Frame> render(const Scene& s) {
  RenderOptions o;
  o.background = Color{1, 1, 1, 1};
  return render_page(s, o, make_ctx());
}

}  // namespace

TEST_CASE("vector/trace: black-and-white tracing matches the bitmap") {
  ir::Image image;
  image.frames.push_back(shapes_frame(110, 100));
  TraceOptions o;
  o.mode = TraceOptions::Mode::BlackWhite;
  o.threshold = 200;  // every coloured shape counts as dark
  const auto scene = image_to_scene(image, o, make_ctx());
  REQUIRE_OK(scene);
  REQUIRE(scene->pages.size() == 1);
  const auto back = render(*scene);
  REQUIRE_OK(back);
  CHECK_GT(iou(image.frames[0], *back), 0.97);
  // Three outlines described with few segments, not one per pixel step.
  CHECK(segment_count(*scene) < 60);
  // A looser tolerance never needs more nodes.
  o.tolerance = 4.0;
  const auto loose = image_to_scene(image, o, make_ctx());
  REQUIRE_OK(loose);
  CHECK(segment_count(*loose) <= segment_count(*scene));
}

TEST_CASE("vector/trace: colour tracing stacks layers without gaps") {
  ir::Image image;
  image.frames.push_back(shapes_frame(110, 100));
  TraceOptions o;
  o.mode = TraceOptions::Mode::Color;
  o.colors = 4;
  const auto scene = image_to_scene(image, o, make_ctx());
  REQUIRE_OK(scene);
  CHECK(scene->pages[0].root.children.size() == 4);
  const auto back = render(*scene);
  REQUIRE_OK(back);
  ir::Image a, b;
  a.frames.push_back(image.frames[0]);
  b.frames.push_back(*back);
  CHECK_GT(test::psnr(a, b), 22.0);
  CHECK_GT(test::ssim(a, b), 0.9);
  // The first layer is the largest colour (white) and covers the whole picture.
  const auto* bottom = std::get_if<Shape>(&scene->pages[0].root.children[0].value);
  REQUIRE(bottom != nullptr);
  CHECK(bottom->fill.color == Color::rgb8(255, 255, 255));
}

TEST_CASE("vector/trace: specks, transparency and options through the engine") {
  ir::Frame f = ir::Frame::filled(40, 40, {0, 0, 0, 0});
  for (std::uint32_t y = 10; y < 30; ++y) {
    for (std::uint32_t x = 10; x < 30; ++x) {
      const std::size_t o = f.offset(x, y);
      f.rgba[o + 3] = 255;
    }
  }
  f.rgba[f.offset(2, 2) + 3] = 255;  // a one-pixel speck
  ir::Image image;
  image.frames.push_back(f);
  TraceOptions o;
  o.mode = TraceOptions::Mode::BlackWhite;
  const auto scene = image_to_scene(image, o, make_ctx());
  REQUIRE_OK(scene);
  const auto* shape = std::get_if<Shape>(&scene->pages[0].root.children.at(0).value);
  REQUIRE(shape != nullptr);
  // One square, exact corners, the speck dropped.
  CHECK(shape->path.verbs.size() <= 6);
  const Rect r = bounds(shape->path);
  CHECK_APPROX(r.x0, 10.0, 1e-9);
  CHECK_APPROX(r.y1, 30.0, 1e-9);
  // Through the engine: png -> svg traced, and dxf (which cannot embed) traces by default.
  const auto png = test::write_from("png", image);
  REQUIRE_OK(png);
  const auto svg = test::convert(*png, "png", "svg", R"({"trace":"bw"})");
  REQUIRE(svg.ok());
  CHECK(svg.text().find("<path") != std::string::npos);
  CHECK(svg.text().find("<image") == std::string::npos);
  const auto embedded = test::convert(*png, "png", "svg");
  REQUIRE(embedded.ok());
  CHECK(embedded.text().find("<image") != std::string::npos);
  const auto dxf = test::convert(*png, "png", "dxf");
  CHECK(dxf.ok());
}
