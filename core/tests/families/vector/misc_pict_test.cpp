// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/image/image_api.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/vector_codecs.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

struct Placed {
  const Node* node = nullptr;
  Matrix total;
};

void collect(const Group& group, const Matrix& parent, std::vector<Placed>& out) {
  const Matrix total = parent * group.transform;
  for (const Node& node : group.children) {
    if (const auto* g = std::get_if<Group>(&node.value)) collect(*g, total, out);
    else out.push_back({&node, total});
  }
}

Context test_context() {
  Context ctx;
  ctx.registry = &Registry::instance();
  return ctx;
}

bool near(double a, double b, double tol = 1e-3) { return std::abs(a - b) <= tol; }

bool rgb_is(const Color& c, int r, int g, int b) {
  const auto byte = [](float v) { return static_cast<int>(std::lround(v * 255.0f)); };
  return std::abs(byte(c.r) - r) <= 1 && std::abs(byte(c.g) - g) <= 1 && std::abs(byte(c.b) - b) <= 1;
}

const Picture* first_picture(const Scene& scene, Rect* placed = nullptr) {
  std::vector<Placed> items;
  collect(scene.pages.at(0).root, Matrix{}, items);
  for (const Placed& p : items) {
    if (const auto* pic = std::get_if<Picture>(&p.node->value)) {
      if (placed != nullptr) *placed = transform_rect(pic->bounds, p.total);
      return pic;
    }
  }
  return nullptr;
}

std::array<int, 3> pixel(const ir::Frame& frame, std::uint32_t x, std::uint32_t y) {
  const std::size_t at = frame.offset(x, y);
  return {frame.rgba[at], frame.rgba[at + 1], frame.rgba[at + 2]};
}

}  // namespace

TEST_CASE("vector/pict: decodes vector opcodes of a version 2 picture") {
  std::vector<std::string> warnings;
  REQUIRE_OK(test::read_as<ir::Data>("pict", test::fixture("vector/misc/vector_v2_hand.pct"), {}, &warnings));
  auto scene = read_scene("pict", test::fixture("vector/misc/vector_v2_hand.pct"), test_context());
  REQUIRE_OK(scene);
  const Page& page = scene->pages[0];
  CHECK(near(page.width, 200 * 96.0 / 72.0));
  CHECK(near(page.height, 100 * 96.0 / 72.0));
  std::vector<Placed> items;
  collect(page.root, Matrix{}, items);
  std::vector<std::pair<const Shape*, Rect>> shapes;
  std::vector<const Text*> texts;
  for (const Placed& p : items) {
    if (const auto* s = std::get_if<Shape>(&p.node->value)) shapes.push_back({s, bounds(s->path, p.total)});
    if (const auto* t = std::get_if<Text>(&p.node->value)) texts.push_back(t);
  }
  REQUIRE(shapes.size() >= 8);
  const double k = 96.0 / 72.0;
  // paintRect 10,10,30,50 in red.
  CHECK(rgb_is(shapes[0].first->fill.color, 255, 0, 0));
  CHECK(near(shapes[0].second.x0, 10 * k));
  CHECK(near(shapes[0].second.y0, 10 * k));
  CHECK(near(shapes[0].second.x1, 50 * k));
  CHECK(near(shapes[0].second.y1, 30 * k));
  // frameOval with a 4 pt pen stays inside its rectangle.
  CHECK(rgb_is(shapes[1].first->stroke.paint.color, 0, 0, 255));
  CHECK(near(shapes[1].first->stroke.width, 4));
  // Line with a 2x2 pen: from (10,50) to (100,50), 2 pt thick.
  CHECK(rgb_is(shapes[2].first->fill.color, 0, 128, 0));
  CHECK(near(shapes[2].second.x0, 10 * k));
  CHECK(near(shapes[2].second.x1, 102 * k));
  CHECK(near(shapes[2].second.y1 - shapes[2].second.y0, 2 * k));
  // ShortLine (10,60) + (20,5).
  CHECK(near(shapes[3].second.x1, 32 * k));
  CHECK(near(shapes[3].second.y1, 67 * k));
  // Polygon triangle.
  CHECK(rgb_is(shapes[4].first->fill.color, 255, 128, 0));
  CHECK(near(shapes[4].second.x0, 120 * k));
  CHECK(near(shapes[4].second.y0, 60 * k));
  // Wedge from 3 to 6 o'clock in the lower-right quadrant of 120,10..160,50.
  CHECK(rgb_is(shapes[5].first->fill.color, 128, 0, 128));
  CHECK(near(shapes[5].second.x0, 140 * k, 0.01));
  CHECK(near(shapes[5].second.y0, 30 * k, 0.01));
  CHECK(near(shapes[5].second.x1, 160 * k, 0.01));
  CHECK(near(shapes[5].second.y1, 50 * k, 0.01));
  // Rounded rectangle frame.
  CHECK(shapes[6].first->stroke.paint.visible());
  // Text runs: LongText then DHText 40 pt further.
  REQUIRE(texts.size() == 2);
  CHECK(texts[0]->content == "Hello");
  CHECK(texts[0]->font.weight == 700);
  CHECK(texts[0]->font.size == 14);
  CHECK(texts[0]->font.family == "Helvetica");
  CHECK(texts[0]->origin == Point{10, 95});
  CHECK(texts[1]->content == "Caf");
  CHECK(texts[1]->origin == Point{50, 95});
  // The clip region.
  bool clipped = false;
  for (const Node& n : page.root.children) {
    if (const auto* g = std::get_if<Group>(&n.value); g != nullptr && g->clip) clipped = true;
  }
  CHECK(clipped);
}

TEST_CASE("vector/pict: decodes a version 1 picture with patterns") {
  auto scene = read_scene("pict", test::fixture("vector/misc/vector_v1_hand.pct"), test_context());
  REQUIRE_OK(scene);
  CHECK(near(scene->pages[0].width, 60 * 96.0 / 72.0));
  std::vector<Placed> items;
  collect(scene->pages[0].root, Matrix{}, items);
  REQUIRE(items.size() == 3);
  const auto* grey = std::get_if<Shape>(&items[0].node->value);
  REQUIRE(grey != nullptr);
  CHECK(rgb_is(grey->fill.color, 128, 128, 128));  // 50% pattern of black on white
  const auto* oval = std::get_if<Shape>(&items[1].node->value);
  REQUIRE(oval != nullptr);
  CHECK(rgb_is(oval->fill.color, 0, 0, 0));
  const Rect r = bounds(oval->path, items[1].total);
  CHECK(near(r.x0, 30 * 96.0 / 72.0, 0.01));
}

TEST_CASE("vector/pict: decodes ImageMagick pixmaps") {
  {
    auto scene = read_scene("pict", test::fixture("vector/misc/gradient_truecolor.pct"), test_context());
    REQUIRE_OK(scene);
    Rect placed;
    const Picture* picture = first_picture(*scene, &placed);
    REQUIRE(picture != nullptr);
    REQUIRE(picture->pixels->width == 16);
    REQUIRE(picture->pixels->height == 8);
    CHECK(pixel(*picture->pixels, 0, 0) == std::array<int, 3>{255, 0, 0});
    CHECK(pixel(*picture->pixels, 5, 3) == std::array<int, 3>{146, 0, 109});
    CHECK(pixel(*picture->pixels, 15, 7) == std::array<int, 3>{0, 0, 255});
    CHECK(near(placed.x1, 16 * 96.0 / 72.0));
  }
  {
    auto scene = read_scene("pict", test::fixture("vector/misc/palette.pct"), test_context());
    REQUIRE_OK(scene);
    const Picture* picture = first_picture(*scene);
    REQUIRE(picture != nullptr);
    REQUIRE(picture->pixels->width == 12);
    CHECK(pixel(*picture->pixels, 2, 4) == std::array<int, 3>{255, 0, 0});
    CHECK(pixel(*picture->pixels, 8, 1) == std::array<int, 3>{0, 0, 255});
    CHECK(pixel(*picture->pixels, 8, 4) == std::array<int, 3>{255, 255, 255});
  }
  if (image::can_decode("jpg", test_context())) {
    auto scene = read_scene("pict", test::fixture("vector/misc/jpeg.pct"), test_context());
    REQUIRE_OK(scene);
    Rect placed;
    const Picture* picture = first_picture(*scene, &placed);
    REQUIRE(picture != nullptr);
    CHECK(picture->encoded_format == "jpg");
    // The scene envelope keeps the original JPEG bytes; decode them as a renderer would.
    auto decoded = image::decode("jpg", picture->encoded, test_context());
    REQUIRE_OK(decoded);
    const ir::Frame& jpeg = decoded->frames.at(0);
    REQUIRE(jpeg.width == 16);
    const auto top = pixel(jpeg, 0, 0), bottom = pixel(jpeg, 8, 15);
    CHECK(std::abs(top[0] - 255) < 8);
    CHECK(std::abs(top[1] - 255) < 8);
    CHECK(std::abs(bottom[1] - 128) < 8);
    CHECK(near(placed.x1, 16 * 96.0 / 72.0));
  }
}

TEST_CASE("vector/pict: detection and conversion") {
  const Registry& registry = Registry::instance();
  for (const char* name : {"vector_v2_hand.pct", "gradient_truecolor.pct", "palette.pct", "jpeg.pct"}) {
    const auto d = detect_format(registry, test::fixture(std::string("vector/misc/") + name), "");
    REQUIRE(d.has_value());
    CHECK(d->format->id == "pict");
  }
  const auto v1 = detect_format(registry, test::fixture("vector/misc/vector_v1_hand.pct"), "old.pct");
  REQUIRE(v1.has_value());
  CHECK(v1->format->id == "pict");
  CHECK(test::find_writer("pict", Kind::Data) == nullptr);  // read only
  const auto tikz = test::convert(test::fixture("vector/misc/vector_v2_hand.pct"), "", "tikz");
  INFO(tikz.message);
  CHECK(tikz.ok());
  const auto vd = test::convert(test::fixture("vector/misc/vector_v2_hand.pct"), "", "vectordrawable");
  CHECK(vd.ok());
}

TEST_CASE("vector/pict: hostile input fails cleanly") {
  for (const char* name : {"vector_v2_hand.pct", "vector_v1_hand.pct", "gradient_truecolor.pct", "palette.pct", "jpeg.pct"}) {
    CHECK(test::fuzz_reader<ir::Data>("pict", test::fixture(std::string("vector/misc/") + name)) > 0);
  }
  Context ctx = test_context();
  CHECK_STATUS(read_scene("pict", as_bytes(std::string("not a picture at all")), ctx), Status::Decode);
  // A pixmap that claims to be 30000 x 30000 pixels.
  Bytes bomb = test::fixture("vector/misc/palette.pct");
  // bounds of the PixMap: after opcode 0x0098 and rowBytes at 0x234..0x237.
  REQUIRE(bomb.size() > 0x240);
  bomb[0x23C] = std::byte{0x75};
  bomb[0x23D] = std::byte{0x30};
  bomb[0x23E] = std::byte{0x75};
  bomb[0x23F] = std::byte{0x30};
  Limits limits;
  limits.max_pixels = 1'000'000;
  const auto result = test::read_as<ir::Data>("pict", bomb, {}, nullptr, limits);
  CHECK_FALSE(result.has_value());
}
