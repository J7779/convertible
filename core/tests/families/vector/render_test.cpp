// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// The vector rasteriser: exact coverage, fill rules, paint, compositing.
#include <cmath>
#include <numbers>

#include "families/vector/geom.hpp"
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

Group one_shape(Path path, Paint fill, FillRule rule = FillRule::NonZero) {
  Shape s;
  s.path = std::move(path);
  s.fill = std::move(fill);
  s.fill_rule = rule;
  Group g;
  g.children.push_back(Node{std::move(s)});
  return g;
}

double alpha(const ir::Frame& f, std::uint32_t x, std::uint32_t y) { return f.rgba[f.offset(x, y) + 3] / 255.0; }

double total_alpha(const ir::Frame& f) {
  double sum = 0;
  for (std::size_t i = 3; i < f.rgba.size(); i += 4) sum += f.rgba[i] / 255.0;
  return sum;
}

}  // namespace

TEST_CASE("vector/render: rectangle edges get exact area coverage") {
  Path p;
  add_rect(p, 2.25, 3.0, 5.5, 4.5);  // x 2.25..7.75, y 3..7.5
  const auto frame = render_group(one_shape(p, Paint::solid({0, 0, 0, 1})), {}, 12, 12, make_ctx());
  REQUIRE_OK(frame);
  CHECK_APPROX(alpha(*frame, 2, 4), 0.75, 0.01);
  CHECK_APPROX(alpha(*frame, 7, 4), 0.75, 0.01);
  CHECK_APPROX(alpha(*frame, 4, 4), 1.0, 0.01);
  CHECK_APPROX(alpha(*frame, 4, 7), 0.5, 0.01);
  CHECK_APPROX(alpha(*frame, 2, 7), 0.375, 0.01);
  CHECK_APPROX(alpha(*frame, 1, 4), 0.0, 0.001);
  CHECK_APPROX(total_alpha(*frame), 5.5 * 4.5, 0.05);
}

TEST_CASE("vector/render: circle area and edge pixels") {
  for (const double r : {3.3, 10.0, 40.0}) {
    CAPTURE(r);
    Path p;
    add_ellipse(p, 50.3, 50.6, r, r);
    const auto frame = render_group(one_shape(p, Paint::solid({0, 0, 0, 1})), {}, 101, 101, make_ctx());
    REQUIRE_OK(frame);
    CHECK_APPROX(total_alpha(*frame), std::numbers::pi * r * r, std::numbers::pi * r * r * 0.004 + 0.5);
  }
  // A pixel almost entirely inside the circle edge.
  Path p;
  add_ellipse(p, 0, 0, 40, 40);
  const auto frame = render_group(one_shape(p, Paint::solid({0, 0, 0, 1})), Matrix::translate(50, 50), 101, 101,
                                  make_ctx());
  REQUIRE_OK(frame);
  // Pixel (88, 50) spans x 38..39 from the centre: fully inside.
  CHECK_APPROX(alpha(*frame, 88, 50), 1.0, 0.02);
  CHECK_APPROX(alpha(*frame, 89, 50), 0.98, 0.03);
}

TEST_CASE("vector/render: non-zero and even-odd fill rules") {
  Path p;
  add_rect(p, 0, 0, 20, 20);
  add_rect(p, 5, 5, 10, 10);  // same direction
  const auto nz = render_group(one_shape(p, Paint::solid({0, 0, 0, 1})), {}, 20, 20, make_ctx());
  const auto eo = render_group(one_shape(p, Paint::solid({0, 0, 0, 1}), FillRule::EvenOdd), {}, 20, 20, make_ctx());
  REQUIRE_OK(nz);
  REQUIRE_OK(eo);
  CHECK(alpha(*nz, 10, 10) == 1.0);
  CHECK(alpha(*eo, 10, 10) == 0.0);
  CHECK(alpha(*eo, 2, 2) == 1.0);
}
