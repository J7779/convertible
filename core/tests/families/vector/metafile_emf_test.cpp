// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "engine/detect.hpp"
#include "families/image/image_api.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/render.hpp"
#include "families/vector/vector_codecs.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

// ---- scene inspection

struct Item {
  enum class Kind { Shape, Text, Picture } kind = Kind::Shape;
  Rect box;  // page space
  Paint fill;
  Stroke stroke;
  FillRule rule = FillRule::NonZero;
  std::string text;
  FontSpec font;
  const Picture* picture = nullptr;
  const Shape* shape = nullptr;
  Matrix m;
  bool clipped = false;
};

void collect(const Group& g, const Matrix& parent, bool clipped, std::vector<Item>& out) {
  const Matrix m = parent * g.transform;
  clipped = clipped || static_cast<bool>(g.clip);
  for (const Node& n : g.children) {
    Item it;
    it.m = m;
    it.clipped = clipped;
    if (const auto* s = std::get_if<Shape>(&n.value)) {
      it.kind = Item::Kind::Shape;
      it.box = bounds(s->path, m);
      it.fill = s->fill;
      it.stroke = s->stroke;
      it.rule = s->fill_rule;
      it.shape = s;
    } else if (const auto* t = std::get_if<Text>(&n.value)) {
      it.kind = Item::Kind::Text;
      it.box = bounds(t->outline, m);
      it.fill = t->fill;
      it.text = t->content;
      it.font = t->font;
    } else if (const auto* p = std::get_if<Picture>(&n.value)) {
      it.kind = Item::Kind::Picture;
      it.box = transform_rect(p->bounds, m);
      it.picture = p;
    } else if (const auto* sub = std::get_if<Group>(&n.value)) {
      collect(*sub, m, clipped, out);
      continue;
    }
    out.push_back(std::move(it));
  }
}

std::vector<Item> items(const Scene& s) {
  std::vector<Item> out;
  if (!s.pages.empty()) collect(s.pages[0].root, Matrix{}, false, out);
  return out;
}

bool same(const Rect& a, const Rect& b, double tol = 1e-6) {
  return std::abs(a.x0 - b.x0) <= tol && std::abs(a.y0 - b.y0) <= tol && std::abs(a.x1 - b.x1) <= tol &&
         std::abs(a.y1 - b.y1) <= tol;
}

bool near(Color a, Color b, float tol = 0.01f) {
  return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol &&
         std::abs(a.a - b.a) <= tol;
}

const Item* find_fill(const std::vector<Item>& list, Color c, float tol = 0.01f) {
  for (const Item& it : list) {
    if (it.fill.type == Paint::Type::Solid && near(it.fill.color, c, tol)) return &it;
  }
  return nullptr;
}

const Item* find_text(const std::vector<Item>& list, std::string_view text) {
  for (const Item& it : list) {
    if (it.kind == Item::Kind::Text && it.text == text) return &it;
  }
  return nullptr;
}

Result<Scene> read(std::string_view id, ByteView bytes, std::string_view options = {},
                   std::vector<std::string>* warnings = nullptr, const Limits& limits = {}) {
  Context ctx;
  ctx.registry = &Registry::instance();
  ctx.limits = limits;
  auto opts = Options::parse(options);
  if (!opts) return std::unexpected(opts.error());
  auto scene = read_scene(id, bytes, ctx, *opts);
  if (warnings != nullptr) *warnings = ctx.warnings.list();
  return scene;
}

// ---- a minimal EMF builder (96 dpi reference device, MM_TEXT: 1 unit = 1 px)

struct Emf {
  util::ByteWriter body;
  std::uint32_t records = 0;
  double width = 100, height = 100;

  Emf& rec(std::uint32_t type, const util::ByteWriter& payload) {
    body.u32le(type).u32le(static_cast<std::uint32_t>(8 + ((payload.size() + 3) & ~std::size_t{3})));
    body.bytes(payload.view());
    body.align(4);
    ++records;
    return *this;
  }
  Emf& rec(std::uint32_t type, std::initializer_list<std::int32_t> values) {
    util::ByteWriter p;
    for (const std::int32_t v : values) p.i32le(v);
    return rec(type, p);
  }
  Bytes finish(std::uint16_t handles = 16) {
    rec(14, {0, 16, 20});
    util::ByteWriter h;
    h.u32le(1).u32le(108);
    h.i32le(0).i32le(0).i32le(static_cast<std::int32_t>(width) - 1).i32le(static_cast<std::int32_t>(height) - 1);
    h.i32le(0).i32le(0).i32le(static_cast<std::int32_t>(std::lround(width * 2540 / 96)))
        .i32le(static_cast<std::int32_t>(std::lround(height * 2540 / 96)));
    h.u32le(0x464D4520).u32le(0x10000).u32le(static_cast<std::uint32_t>(108 + body.size())).u32le(records + 1);
    h.u16le(handles).u16le(0).u32le(0).u32le(0).u32le(0);
    h.i32le(9600).i32le(9600).i32le(2540).i32le(2540).u32le(0).u32le(0).u32le(0).u32le(2540000).u32le(2540000);
    h.bytes(body.view());
    return h.take();
  }
};

void bounds16(util::ByteWriter& p) { p.i32le(0).i32le(0).i32le(0).i32le(0); }

util::ByteWriter dib24(std::uint32_t w, std::uint32_t h, const std::vector<std::array<std::uint8_t, 3>>& rgb_top_down) {
  util::ByteWriter p;
  p.u32le(40).i32le(static_cast<std::int32_t>(w)).i32le(static_cast<std::int32_t>(h)).u16le(1).u16le(24).u32le(0);
  const std::uint32_t stride = ((w * 24 + 31) / 32) * 4;
  p.u32le(stride * h).i32le(0).i32le(0).u32le(0).u32le(0);
  for (std::uint32_t row = h; row-- > 0;) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const auto& c = rgb_top_down[row * w + x];
      p.u8(c[2]).u8(c[1]).u8(c[0]);
    }
    for (std::uint32_t i = w * 3; i < stride; ++i) p.u8(0);
  }
  return p;
}

/// EMR_STRETCHDIBITS drawing `dib` (BITMAPINFOHEADER + bits, 40-byte header, no table) into x,y,w,h.
Emf& stretch_dibits(Emf& e, const util::ByteWriter& dib, std::int32_t x, std::int32_t y, std::int32_t w,
                    std::int32_t h, std::uint32_t sw, std::uint32_t sh, std::uint32_t header_bytes = 40,
                    std::uint32_t rop = 0x00CC0020, std::uint32_t usage = 0) {
  util::ByteWriter p;
  bounds16(p);
  p.i32le(x).i32le(y).i32le(0).i32le(0).i32le(static_cast<std::int32_t>(sw)).i32le(static_cast<std::int32_t>(sh));
  p.u32le(80).u32le(header_bytes).u32le(80 + header_bytes).u32le(static_cast<std::uint32_t>(dib.size() - header_bytes));
  p.u32le(usage).u32le(rop).i32le(w).i32le(h);
  p.bytes(dib.view());
  return e.rec(81, p);
}

std::uint32_t rgb(int r, int g, int b) { return static_cast<std::uint32_t>(r | (g << 8) | (b << 16)); }

}  // namespace

// --------------------------------------------------------------------------- real files

TEST_CASE("vector/emf: Inkscape drawing decodes to the original geometry and colours") {
  const auto scene = read("emf", test::fixture("vector/metafile/ink_shapes.emf"));
  REQUIRE_OK(scene);
  REQUIRE(scene->pages.size() == 1);
  CHECK_APPROX(scene->pages[0].width, 400.6, 1.0);
  CHECK_APPROX(scene->pages[0].height, 302.3, 1.0);
  const auto list = items(*scene);
  const Item* red = find_fill(list, Color::rgb8(255, 0, 0));
  REQUIRE(red != nullptr);
  CHECK_APPROX(red->box.x0, 20.0, 0.5);
  CHECK_APPROX(red->box.y0, 20.0, 0.5);
  CHECK_APPROX(red->box.x1, 140.0, 0.5);
  CHECK_APPROX(red->box.y1, 100.0, 0.5);
  CHECK(near(red->stroke.paint.color, Color::rgb8(0, 0, 255)));
  CHECK_APPROX(red->stroke.width, 4.0, 0.1);
  const Item* green = find_fill(list, Color::rgb8(0, 0xc0, 0));
  REQUIRE(green != nullptr);
  CHECK_APPROX(green->box.x0, 180.0, 0.5);
  CHECK_APPROX(green->box.x1, 300.0, 0.5);
  CHECK_APPROX(green->box.y1, 100.0, 0.5);
  // The purple curve keeps its Béziers; the teal polyline is dashed.
  bool curve = false, dashed = false;
  for (const Item& it : list) {
    if (it.kind != Item::Kind::Shape) continue;
    if (near(it.stroke.paint.color, Color::rgb8(0x80, 0, 0x80))) {
      curve = std::count(it.shape->path.verbs.begin(), it.shape->path.verbs.end(), Verb::Cubic) >= 1;
      CHECK_APPROX(it.stroke.width, 6.0, 0.1);
    }
    if (near(it.stroke.paint.color, Color::rgb8(0, 0x80, 0x80))) dashed = !it.stroke.dash.empty();
  }
  CHECK(curve);
  CHECK(dashed);
  // The 4x4 picture: red, green / blue, white quadrants.
  const Item* pic = nullptr;
  for (const Item& it : list) {
    if (it.kind == Item::Kind::Picture) pic = &it;
  }
  REQUIRE(pic != nullptr);
  CHECK_APPROX(pic->box.x0, 300.0, 0.6);
  CHECK_APPROX(pic->box.y0, 240.0, 0.6);
  CHECK_APPROX(pic->box.width(), 40.0, 0.6);
  const ir::Frame& f = *pic->picture->pixels;
  REQUIRE(f.width == 4);
  REQUIRE(f.height == 4);
  const auto px = [&](std::uint32_t x, std::uint32_t y) {
    const std::size_t at = f.offset(x, y);
    return std::array<int, 3>{f.rgba[at], f.rgba[at + 1], f.rgba[at + 2]};
  };
  CHECK(px(0, 0) == std::array<int, 3>{255, 0, 0});
  CHECK(px(3, 0) == std::array<int, 3>{0, 255, 0});
  CHECK(px(0, 3) == std::array<int, 3>{0, 0, 255});
  CHECK(px(3, 3) == std::array<int, 3>{255, 255, 255});
}

TEST_CASE("vector/emf: LibreOffice drawing (EMF+ header, GDI records after GetDC) decodes") {
  const auto scene = read("emf", test::fixture("vector/metafile/lo_shapes.emf"));
  REQUIRE_OK(scene);
  CHECK_APPROX(scene->pages[0].width, 372.5, 1.0);
  const auto list = items(*scene);
  const Item* red = find_fill(list, Color::rgb8(255, 0, 0));
  REQUIRE(red != nullptr);
  CHECK_APPROX(red->box.width(), 120.0, 0.5);
  CHECK_APPROX(red->box.height(), 80.0, 0.5);
  const Item* yellow = find_fill(list, Color::rgb8(255, 0xcc, 0));
  REQUIRE(yellow != nullptr);
  CHECK_APPROX(yellow->box.x0 - red->box.x0, 20.0, 0.5);  // triangle starts at x 40, rectangle at 20
  CHECK_APPROX(yellow->box.y0 - red->box.y0, 120.0, 0.5);
  const Item* hello = find_text(list, "Hello EMF");
  REQUIRE(hello != nullptr);
  CHECK(hello->font.family == "DejaVu Sans");
  CHECK_APPROX(hello->font.size, 28.0, 0.5);
  CHECK(near(hello->fill.color, Color::rgb8(0, 0, 0x80)));
  CHECK_APPROX(hello->box.x0 - red->box.x0, 0.0, 3.0);
}

TEST_CASE("vector/emf: LibreOffice text records keep strings, fonts, weights and rotation") {
  const auto scene = read("emf", test::fixture("vector/metafile/lo_text.emf"));
  REQUIRE_OK(scene);
  const auto list = items(*scene);
  const Item* lorem = find_text(list, "Lorem ipsum");
  REQUIRE(lorem != nullptr);
  CHECK_APPROX(lorem->font.size, 24.0, 0.3);
  const Item* greek = find_text(list, "Ωμέγα Жук");
  REQUIRE(greek != nullptr);
  CHECK(greek->font.family == "DejaVu Serif");
  CHECK(greek->font.weight == 700);
  CHECK(near(greek->fill.color, Color::rgb8(0xc0, 0, 0)));
  const Item* rotated = find_text(list, "rotated");
  REQUIRE(rotated != nullptr);
  CHECK_APPROX(std::atan2(rotated->m.b, rotated->m.a) * 180 / std::numbers::pi, -30.0, 0.5);
}

TEST_CASE("vector/emf: dual EMF+/GDI file draws EMF+ by default and GDI on request") {
  const Bytes file = test::fixture("vector/metafile/lo_fills_dual.emf");
  const auto plus = read("emf", file);
  REQUIRE_OK(plus);
  // The half-transparent green square exists only as an EMF+ FillPolygon.
  const auto with_plus = items(*plus);
  const Item* translucent = find_fill(with_plus, Color{0, 1, 0, 127.0f / 255.0f});
  REQUIRE(translucent != nullptr);
  CHECK_APPROX(translucent->box.width(), 80.0, 1.5);
  const auto gdi = read("emf", file, R"({"emf_plus":"off"})");
  REQUIRE_OK(gdi);
  const auto with_gdi = items(*gdi);
  CHECK(find_fill(with_gdi, Color{0, 1, 0, 127.0f / 255.0f}) == nullptr);
  // The GDI fallback draws an opaque square instead.
  const Item* opaque = find_fill(with_gdi, Color{0, 1, 0, 1});
  REQUIRE(opaque != nullptr);
  CHECK_APPROX(opaque->box.width(), 80.0, 1.5);
  CHECK(find_fill(with_plus, Color{0, 1, 0, 1}) == nullptr);
}

TEST_CASE("vector/emf: LibreOffice Draw shapes (hatch, pie, chord, dashed arc, text)") {
  const auto scene = read("emf", test::fixture("vector/metafile/lo_draw.emf"));
  REQUIRE_OK(scene);
  const auto list = items(*scene);
  const Item* pie = find_fill(list, Color::rgb8(0xff, 0x80, 0));
  REQUIRE(pie != nullptr);
  CHECK_APPROX(pie->box.height(), 3.0 * px_per_mm * 10, 1.5);  // 3 cm circle; the section spans top to bottom
  const Item* chord = find_fill(list, Color::rgb8(0, 0xa0, 0xff));
  REQUIRE(chord != nullptr);
  CHECK_APPROX(chord->box.width(), 3.0 * px_per_mm * 10, 1.5);
  CHECK_APPROX(chord->box.height(), 1.0 * px_per_mm * 10, 1.5);  // half of a 2 cm ellipse
  CHECK(find_fill(list, Color::rgb8(0xff, 0xff, 0x99)) != nullptr);  // hatch background
  std::size_t green_strokes = 0;
  for (const Item& it : list) {
    if (it.stroke.paint.visible() && near(it.stroke.paint.color, Color::rgb8(0, 0x80, 0))) ++green_strokes;
    if (it.fill.visible() && near(it.fill.color, Color::rgb8(0, 0x80, 0))) ++green_strokes;
  }
  CHECK(green_strokes >= 3);  // the dashed arc arrives as separate dashes
  CHECK(find_text(list, "Draw 123") != nullptr);
}

TEST_CASE("vector/emf: pstoedit output keeps proportions") {
  const auto scene = read("emf", test::fixture("vector/metafile/pstoedit_shapes.emf"));
  REQUIRE_OK(scene);
  const auto list = items(*scene);
  const Item* blue = find_fill(list, Color::rgb8(0, 0, 255));
  REQUIRE(blue != nullptr);
  CHECK_APPROX(blue->box.width(), blue->box.height(), 0.5);
  // 60 pt square inside a 170 pt wide drawing that fills the frame.
  CHECK_APPROX(blue->box.width() / scene->pages[0].width, 60.0 / 170.0, 0.01);
  const Item* triangle = find_fill(list, Color::rgb8(0, 0x80, 0));
  REQUIRE(triangle != nullptr);
  CHECK(triangle->box.y1 < blue->box.y0 + 1);  // PostScript y up: the triangle is above the square
}

TEST_CASE("vector/emf: even-odd fill mode and gradient fallbacks from Inkscape") {
  const auto scene = read("emf", test::fixture("vector/metafile/ink_fills.emf"));
  REQUIRE_OK(scene);
  const auto list = items(*scene);
  const Item* frame = find_fill(list, Color::rgb8(0, 0, 0));
  REQUIRE(frame != nullptr);
  CHECK(frame->rule == FillRule::EvenOdd);
  CHECK(std::count(frame->shape->path.verbs.begin(), frame->shape->path.verbs.end(), Verb::Move) == 2);
  CHECK(find_fill(list, Color::rgb8(0, 255, 0)) != nullptr);
}

TEST_CASE("vector/emf: renders like the source drawing") {
  const auto scene = read("emf", test::fixture("vector/metafile/ink_shapes.emf"));
  REQUIRE_OK(scene);
  Context ctx;
  ctx.registry = &Registry::instance();
  RenderOptions options;
  options.width = 400;
  options.height = 300;
  options.background = Color{1, 1, 1, 1};
  auto frame = render_page(*scene, options, ctx);
  if (!frame && frame.error().code == Status::UnsupportedConversion) SKIP("the rasteriser is not available");
  REQUIRE_OK(frame);
  const auto reference = image::decode("png", test::fixture("vector/metafile/ink_shapes_ref.png"), ctx);
  REQUIRE_OK(reference);
  ir::Image ours;
  ours.frames.push_back(std::move(*frame));
  CHECK(test::psnr(ours, *reference) > 18.0);
  CHECK(test::ssim(ours, *reference) > 0.8);
}

// --------------------------------------------------------------------------- records (built by hand)

TEST_CASE("vector/emf: mapping modes, window/viewport and world transforms") {
  SUBCASE("MM_ANISOTROPIC with window and viewport") {
    Emf e;
    e.rec(17, {8});                 // SETMAPMODE anisotropic
    e.rec(9, {1000, 1000});         // window ext
    e.rec(11, {100, 50});           // viewport ext
    e.rec(10, {500, 0});            // window org
    e.rec(43, {500, 0, 1000, 1000});  // rectangle
    const auto s = read("emf", e.finish());
    REQUIRE_OK(s);
    const auto list = items(*s);
    REQUIRE(list.size() == 1);
    CHECK_APPROX(list[0].box.x0, 0.0, 0.6);
    CHECK_APPROX(list[0].box.x1, 50.0, 0.6);
    CHECK_APPROX(list[0].box.y1, 50.0, 0.6);
  }
  SUBCASE("MM_LOMETRIC is 0.1 mm per unit with y up") {
    Emf e;
    e.rec(17, {2});
    e.rec(12, {0, 100});           // viewport origin at the bottom
    e.rec(43, {0, 0, 254, 127});   // 25.4 mm x 12.7 mm above the origin
    const auto s = read("emf", e.finish());
    REQUIRE_OK(s);
    const auto list = items(*s);
    REQUIRE(list.size() == 1);
    CHECK_APPROX(list[0].box.width(), 96.0, 0.6);
    CHECK_APPROX(list[0].box.height(), 48.0, 0.6);
    CHECK_APPROX(list[0].box.y1, 100.0, 0.6);
  }
  SUBCASE("MM_ISOTROPIC keeps the aspect ratio") {
    Emf e;
    e.rec(17, {7});
    e.rec(9, {100, 100});
    e.rec(11, {100, 50});
    e.rec(43, {0, 0, 100, 100});
    const auto s = read("emf", e.finish());
    REQUIRE_OK(s);
    const auto list = items(*s);
    REQUIRE(list.size() == 1);
    CHECK_APPROX(list[0].box.width(), 50.0, 0.6);
    CHECK_APPROX(list[0].box.height(), 50.0, 0.6);
  }
  SUBCASE("world transforms, SaveDC and RestoreDC") {
    Emf e;
    util::ByteWriter xf;
    xf.f32le(0).f32le(1).f32le(-1).f32le(0).f32le(50).f32le(0);  // rotate 90 degrees, move right
    e.rec(33, {});                                                // SAVEDC
    e.rec(35, xf);                                                // SETWORLDTRANSFORM
    e.rec(43, {0, 0, 40, 10});
    e.rec(34, {-1});                                              // RESTOREDC
    e.rec(43, {0, 0, 40, 10});
    util::ByteWriter scale;
    scale.f32le(2).f32le(0).f32le(0).f32le(2).f32le(0).f32le(0).u32le(4);  // MWT_SET
    e.rec(36, scale);
    util::ByteWriter move;
    move.f32le(1).f32le(0).f32le(0).f32le(1).f32le(5).f32le(5).u32le(2);  // MWT_LEFTMULTIPLY: applied first
    e.rec(36, move);
    e.rec(43, {0, 0, 10, 10});
    const auto s = read("emf", e.finish());
    REQUIRE_OK(s);
    const auto list = items(*s);
    REQUIRE(list.size() == 3);
    CHECK_APPROX(list[0].box.x0, 40.0, 0.01);
    CHECK_APPROX(list[0].box.x1, 50.0, 0.01);
    CHECK_APPROX(list[0].box.y1, 40.0, 0.01);
    CHECK_APPROX(list[1].box.x1, 40.0, 0.01);
    CHECK_APPROX(list[1].box.y1, 10.0, 0.01);
    CHECK_APPROX(list[2].box.x0, 10.0, 0.01);
    CHECK_APPROX(list[2].box.x1, 30.0, 0.01);
  }
}

TEST_CASE("vector/emf: pens, brushes, paths and arcs") {
  Emf e;
  // Geometric pen: 6 units wide, flat caps, bevel joins, green.
  {
    util::ByteWriter p;
    p.u32le(1).u32le(0).u32le(0).u32le(0).u32le(0);
    p.u32le(0x10000 | 0x200 | 0x1000).u32le(6).u32le(0).u32le(rgb(0, 128, 0)).u32le(0).u32le(0);
    e.rec(95, p);
  }
  e.rec(37, {1});
  e.rec(39, {2, 2, static_cast<std::int32_t>(rgb(255, 0, 0)), 0});  // hatched brush (HS_FDIAGONAL)
  e.rec(37, {2});
  e.rec(18, {1});  // transparent background
  e.rec(43, {10, 10, 50, 50});
  // A path bracket filled with a solid brush and the winding rule.
  e.rec(39, {3, 0, static_cast<std::int32_t>(rgb(0, 0, 255)), 0});
  e.rec(37, {3});
  e.rec(19, {2});
  e.rec(59, {});
  e.rec(27, {60, 10});
  e.rec(54, {90, 10});
  {
    util::ByteWriter p;  // POLYBEZIERTO16
    bounds16(p);
    p.u32le(3).i16le(90).i16le(30).i16le(60).i16le(30).i16le(60).i16le(10);
    e.rec(88, p);
  }
  e.rec(61, {});
  e.rec(60, {});
  e.rec(63, {0, 0, 0, 0});  // STROKEANDFILLPATH
  // Pie (counter-clockwise from the start radial to the end radial).
  e.rec(47, {0, 60, 40, 100, 40, 80, 20, 60});
  // AngleArc from the current position.
  e.rec(27, {50, 80});
  {
    util::ByteWriter p;
    p.i32le(70).i32le(80).u32le(10).f32le(0).f32le(90);
    e.rec(41, p);
  }
  const auto s = read("emf", e.finish());
  REQUIRE_OK(s);
  const auto list = items(*s);
  REQUIRE(list.size() == 4);
  // Rectangle: hatch pattern fill, the geometric pen's caps and joins.
  CHECK(list[0].fill.type == Paint::Type::Pattern);
  CHECK(near(list[0].stroke.paint.color, Color::rgb8(0, 128, 0)));
  CHECK_APPROX(list[0].stroke.width, 6.0, 1e-9);
  CHECK(list[0].stroke.cap == LineCap::Butt);
  CHECK(list[0].stroke.join == LineJoin::Bevel);
  // Path: line, curve, close; NonZero; filled blue.
  CHECK(list[1].rule == FillRule::NonZero);
  CHECK(near(list[1].fill.color, Color::rgb8(0, 0, 255)));
  CHECK(std::count(list[1].shape->path.verbs.begin(), list[1].shape->path.verbs.end(), Verb::Cubic) == 1);
  CHECK(list[1].shape->path.verbs.back() == Verb::Close);
  // Pie from angle 0 (right) counter-clockwise to 90 degrees (top): the quarter above the centre.
  CHECK_APPROX(list[2].box.x0, 20.0, 0.01);
  CHECK_APPROX(list[2].box.x1, 40.0, 0.01);
  CHECK_APPROX(list[2].box.y0, 60.0, 0.01);
  CHECK_APPROX(list[2].box.y1, 80.0, 0.01);
  // AngleArc: line from (50,80) to (80,80), then a quarter circle up to (70,70).
  CHECK_APPROX(list[3].box.x0, 50.0, 0.01);
  CHECK_APPROX(list[3].box.x1, 80.0, 0.01);
  CHECK_APPROX(list[3].box.y0, 70.0, 0.01);
}

TEST_CASE("vector/emf: clipping with rectangles, regions and paths") {
  Emf e;
  e.rec(30, {10, 10, 60, 60});  // INTERSECTCLIPRECT
  e.rec(29, {20, 20, 30, 30});  // EXCLUDECLIPRECT
  e.rec(43, {0, 0, 100, 100});
  {
    util::ByteWriter p;  // EXTSELECTCLIPRGN, RGN_COPY with two rectangles
    p.u32le(32 + 32).u32le(5);
    p.u32le(32).u32le(1).u32le(2).u32le(32).i32le(0).i32le(0).i32le(100).i32le(100);
    p.i32le(0).i32le(0).i32le(10).i32le(10).i32le(50).i32le(50).i32le(60).i32le(60);
    e.rec(75, p);
  }
  e.rec(43, {0, 0, 100, 100});
  {
    util::ByteWriter p;  // a null region resets the clip
    p.u32le(0).u32le(5);
    e.rec(75, p);
  }
  e.rec(43, {0, 0, 100, 100});
  e.rec(59, {});
  e.rec(42, {0, 0, 50, 50});
  e.rec(60, {});
  e.rec(67, {1});  // SELECTCLIPPATH RGN_AND
  e.rec(43, {0, 0, 100, 100});
  const auto s = read("emf", e.finish());
  REQUIRE_OK(s);
  const Group& root = s->pages[0].root;
  REQUIRE(root.children.size() == 4);
  const auto* g0 = std::get_if<Group>(&root.children[0].value);
  REQUIRE(g0 != nullptr);
  REQUIRE(g0->clip != nullptr);
  CHECK(g0->clip->items[0].rule == FillRule::EvenOdd);  // the excluded rectangle
  REQUIRE(g0->clip->clip != nullptr);                   // ... inside the intersected one
  CHECK(same(bounds(g0->clip->clip->items[0].path), Rect{10, 10, 60, 60}));
  const auto* g1 = std::get_if<Group>(&root.children[1].value);
  REQUIRE(g1 != nullptr);
  CHECK(g1->clip->clip == nullptr);
  CHECK(std::count(g1->clip->items[0].path.verbs.begin(), g1->clip->items[0].path.verbs.end(), Verb::Move) == 2);
  CHECK(std::holds_alternative<Shape>(root.children[2].value));  // unclipped
  const auto* g3 = std::get_if<Group>(&root.children[3].value);
  REQUIRE(g3 != nullptr);
  const Rect c3 = bounds(g3->clip->items[0].path);
  CHECK_APPROX(c3.x0, 0.0, 1e-6);
  CHECK_APPROX(c3.x1, 50.0, 1e-6);
  CHECK_APPROX(c3.y1, 50.0, 1e-6);
}

TEST_CASE("vector/emf: text alignment, advances, background and charsets") {
  Emf e;
  {
    util::ByteWriter p;  // EXTCREATEFONTINDIRECTW: 20 px Arial, italic, escapement 0
    p.u32le(1).i32le(-20).i32le(0).i32le(0).i32le(0).i32le(400);
    p.u8(1).u8(0).u8(0).u8(0).u8(0).u8(0).u8(0).u8(0);
    const std::u16string face = u"Arial";
    for (std::size_t i = 0; i < 32; ++i) p.u16le(i < face.size() ? face[i] : 0);
    e.rec(82, p);
  }
  e.rec(37, {1});
  e.rec(22, {6 | 8});  // TA_CENTER | TA_BOTTOM
  e.rec(24, {static_cast<std::int32_t>(rgb(0, 0, 255))});
  e.rec(25, {static_cast<std::int32_t>(rgb(255, 255, 0))});
  {
    util::ByteWriter p;  // EXTTEXTOUTW "AB" at (50, 40), ETO_OPAQUE with a rectangle, Dx 30 + 10
    bounds16(p);
    p.u32le(1).f32le(0).f32le(0);
    p.i32le(50).i32le(40).u32le(2).u32le(76).u32le(2);
    p.i32le(0).i32le(0).i32le(100).i32le(50).u32le(80);
    p.u16le('A').u16le('B').i32le(30).i32le(10);
    e.rec(84, p);
  }
  const auto s = read("emf", e.finish());
  REQUIRE_OK(s);
  const auto list = items(*s);
  const Item* bg = find_fill(list, Color::rgb8(255, 255, 0));
  REQUIRE(bg != nullptr);
  CHECK(same(bg->box, Rect{0, 0, 100, 50}));
  const Item* text = find_text(list, "AB");
  REQUIRE(text != nullptr);
  CHECK(text->font.italic);
  CHECK_APPROX(text->font.size, 20.0, 1e-9);
  CHECK(near(text->fill.color, Color::rgb8(0, 0, 255)));
  // Centred on x = 50 over a 40 px advance; the descent sits on y = 40.
  CHECK(text->box.x0 >= 30.0 - 1.0);
  CHECK(text->box.x0 < 33.0);
  CHECK(text->box.y1 <= 40.0);
  CHECK(text->box.y1 > 30.0);
}

TEST_CASE("vector/emf: bitmaps (24-bit, palette, RLE8, alpha blend, raster operations)") {
  SUBCASE("StretchDIBits with a 24-bit bottom-up DIB and a source rectangle") {
    Emf e;
    const auto dib = dib24(2, 2, {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}});
    stretch_dibits(e, dib, 10, 10, 40, 20, 2, 2);
    util::ByteWriter crop;
    bounds16(crop);
    crop.i32le(60).i32le(10).i32le(1).i32le(0).i32le(1).i32le(1);  // xSrc 1, ySrc 0 (from the bottom): bottom-right
    crop.u32le(80).u32le(40).u32le(120).u32le(static_cast<std::uint32_t>(dib.size() - 40)).u32le(0).u32le(0x00CC0020);
    crop.i32le(10).i32le(10).bytes(dib.view());
    e.rec(81, crop);
    const auto s = read("emf", e.finish());
    REQUIRE_OK(s);
    const auto list = items(*s);
    REQUIRE(list.size() == 2);
    CHECK(same(list[0].box, Rect{10, 10, 50, 30}));
    const ir::Frame& f = *list[0].picture->pixels;
    CHECK(f.rgba[f.offset(0, 0)] == 255);
    CHECK(f.rgba[f.offset(1, 0) + 1] == 255);
    CHECK(f.rgba[f.offset(0, 1) + 2] == 255);
    const ir::Frame& c = *list[1].picture->pixels;
    REQUIRE(c.width == 1);
    CHECK(c.rgba[0] == 255);
    CHECK(c.rgba[1] == 255);
    CHECK(c.rgba[2] == 255);
  }
  SUBCASE("8-bit RLE with a colour table") {
    Emf e;
    util::ByteWriter dib;
    dib.u32le(40).i32le(4).i32le(2).u16le(1).u16le(8).u32le(1).u32le(0).i32le(0).i32le(0).u32le(2).u32le(0);
    dib.u8(0).u8(0).u8(255).u8(0);   // index 0 red
    dib.u8(255).u8(0).u8(0).u8(0);   // index 1 blue
    dib.u8(4).u8(0).u8(0).u8(0);     // bottom row: 4 x red
    dib.u8(2).u8(1).u8(2).u8(0).u8(0).u8(1);  // top row: 2 blue, 2 red, end of bitmap
    stretch_dibits(e, dib, 0, 0, 4, 2, 4, 2, 48);
    const auto s = read("emf", e.finish());
    REQUIRE_OK(s);
    const auto list = items(*s);
    REQUIRE(list.size() == 1);
    const ir::Frame& f = *list[0].picture->pixels;
    CHECK(f.rgba[f.offset(0, 0) + 2] == 255);  // top-left blue
    CHECK(f.rgba[f.offset(3, 0)] == 255);      // top-right red
    CHECK(f.rgba[f.offset(0, 1)] == 255);      // bottom row red
  }
  SUBCASE("AlphaBlend un-premultiplies and applies the constant alpha") {
    Emf e;
    util::ByteWriter dib;
    dib.u32le(40).i32le(1).i32le(1).u16le(1).u16le(32).u32le(0).u32le(4).i32le(0).i32le(0).u32le(0).u32le(0);
    dib.u8(0).u8(0).u8(128).u8(128);  // premultiplied red at alpha 128
    util::ByteWriter p;
    bounds16(p);
    p.i32le(0).i32le(0).i32le(10).i32le(10);
    p.u8(0).u8(0).u8(128).u8(1);  // constant alpha 128, AC_SRC_ALPHA
    p.i32le(0).i32le(0).f32le(1).f32le(0).f32le(0).f32le(1).f32le(0).f32le(0).u32le(0).u32le(0);
    p.u32le(108).u32le(40).u32le(148).u32le(4).i32le(1).i32le(1);
    p.bytes(dib.view());
    e.rec(114, p);
    const auto s = read("emf", e.finish());
    REQUIRE_OK(s);
    const auto list = items(*s);
    REQUIRE(list.size() == 1);
    const ir::Frame& f = *list[0].picture->pixels;
    CHECK(f.rgba[0] == 255);
    CHECK_APPROX(static_cast<double>(f.rgba[3]), 64.0, 1.0);
  }
  SUBCASE("raster operations without a source use the brush") {
    Emf e;
    e.rec(39, {1, 0, static_cast<std::int32_t>(rgb(0, 255, 255)), 0});
    e.rec(37, {1});
    util::ByteWriter p;  // BITBLT PATCOPY without a bitmap
    bounds16(p);
    p.i32le(5).i32le(5).i32le(10).i32le(10).u32le(0x00F00021).i32le(0).i32le(0);
    p.f32le(1).f32le(0).f32le(0).f32le(1).f32le(0).f32le(0).u32le(0).u32le(0).u32le(0).u32le(0).u32le(0).u32le(0);
    e.rec(76, p);
    util::ByteWriter q;  // BLACKNESS
    bounds16(q);
    q.i32le(20).i32le(5).i32le(10).i32le(10).u32le(0x00000042).i32le(0).i32le(0);
    q.f32le(1).f32le(0).f32le(0).f32le(1).f32le(0).f32le(0).u32le(0).u32le(0).u32le(0).u32le(0).u32le(0).u32le(0);
    e.rec(76, q);
    const auto s = read("emf", e.finish());
    REQUIRE_OK(s);
    const auto list = items(*s);
    REQUIRE(list.size() == 2);
    CHECK(near(list[0].fill.color, Color::rgb8(0, 255, 255)));
    CHECK(same(list[0].box, Rect{5, 5, 15, 15}));
    CHECK(near(list[1].fill.color, Color::rgb8(0, 0, 0)));
  }
}

TEST_CASE("vector/emf: gradient fills") {
  Emf e;
  util::ByteWriter p;  // GRADIENTFILL: one horizontal rectangle and one triangle
  bounds16(p);
  p.u32le(2).u32le(1).u32le(0);  // 2 vertices, 1 rectangle, GRADIENT_FILL_RECT_H
  p.i32le(0).i32le(0).u16le(0xff00).u16le(0).u16le(0).u16le(0);
  p.i32le(100).i32le(20).u16le(0).u16le(0).u16le(0xff00).u16le(0);
  p.u32le(0).u32le(1);
  e.rec(118, p);
  util::ByteWriter t;
  bounds16(t);
  t.u32le(3).u32le(1).u32le(2);
  t.i32le(0).i32le(30).u16le(0xff00).u16le(0).u16le(0).u16le(0);
  t.i32le(100).i32le(30).u16le(0).u16le(0xff00).u16le(0).u16le(0);
  t.i32le(50).i32le(90).u16le(0).u16le(0).u16le(0xff00).u16le(0);
  t.u32le(0).u32le(1).u32le(2);
  e.rec(118, t);
  std::vector<std::string> warnings;
  const auto s = read("emf", e.finish(), {}, &warnings);
  REQUIRE_OK(s);
  const auto list = items(*s);
  REQUIRE(list.size() == 17);
  REQUIRE(list[0].fill.type == Paint::Type::Gradient);
  const Gradient& g = *list[0].fill.gradient;
  CHECK(near(g.stops.front().color, Color::rgb8(255, 0, 0)));
  CHECK(near(g.stops.back().color, Color::rgb8(0, 0, 255)));
  CHECK_APPROX(g.p2.x - g.p1.x, 100.0, 1e-9);
  CHECK(!warnings.empty());  // the triangle is approximated
}

TEST_CASE("vector/emf: damaged and hostile files fail cleanly") {
  const Bytes good = test::fixture("vector/metafile/ink_shapes.emf");
  CHECK_STATUS(read("emf", ByteView(good).first(60)), Status::Decode);
  {
    Bytes bad = good;
    const std::size_t first = static_cast<std::size_t>(good[4]) | (static_cast<std::size_t>(good[5]) << 8);
    for (std::size_t i = 4; i < 8; ++i) bad[first + i] = std::byte{0};  // first record: size 0
    CHECK_STATUS(read("emf", bad), Status::Decode);
  }
  SUBCASE("a bitmap far beyond the pixel limit") {
    Emf e;
    util::ByteWriter dib;
    dib.u32le(40).i32le(100000).i32le(100000).u16le(1).u16le(24).u32le(0).u32le(0).i32le(0).i32le(0).u32le(0).u32le(0);
    dib.zeros(64);
    stretch_dibits(e, dib, 0, 0, 10, 10, 100000, 100000);
    CHECK_STATUS(read("emf", e.finish()), Status::Limit);
  }
  SUBCASE("a truncated bitmap is skipped with a warning") {
    Emf e;
    util::ByteWriter dib;
    dib.u32le(40).i32le(100).i32le(100).u16le(1).u16le(24).u32le(0).u32le(0).i32le(0).i32le(0).u32le(0).u32le(0);
    dib.zeros(64);
    stretch_dibits(e, dib, 0, 0, 10, 10, 100, 100);
    std::vector<std::string> warnings;
    const auto s = read("emf", e.finish(), {}, &warnings);
    REQUIRE_OK(s);
    CHECK(items(*s).empty());
    CHECK(!warnings.empty());
  }
  SUBCASE("deep SaveDC, absurd counts and object indices") {
    Emf e;
    for (int i = 0; i < 6000; ++i) e.rec(33, {});
    e.rec(37, {0x7FFFFFF0});
    e.rec(40, {0x7FFFFFF0});
    e.rec(38, {0x7FFFFFF0, 0, 1, 0, 0});
    util::ByteWriter p;  // POLYGON claiming 2^31 points
    bounds16(p);
    p.u32le(0x80000000u).i32le(0).i32le(0).i32le(10).i32le(0).i32le(10).i32le(10);
    e.rec(3, p);
    util::ByteWriter pp;  // POLYPOLYGON with absurd counts
    bounds16(pp);
    pp.u32le(0x40000000u).u32le(0xFFFFFFFFu).u32le(3);
    e.rec(8, pp);
    for (int i = 0; i < 7000; ++i) e.rec(34, {-1});
    const auto s = read("emf", e.finish());
    REQUIRE_OK(s);
    CHECK(items(*s).size() == 1);
  }
  SUBCASE("thousands of clip operations keep the clip chain short") {
    Emf e;
    for (int i = 0; i < 3000; ++i) {
      e.rec(29, {i % 90, i % 90, i % 90 + 2, i % 90 + 2});  // EXCLUDECLIPRECT
      e.rec(30, {0, 0, 100 - i % 7, 100});                 // INTERSECTCLIPRECT
    }
    e.rec(43, {0, 0, 100, 100});
    const auto s = read("emf", e.finish());
    REQUIRE_OK(s);
    const auto* g = std::get_if<Group>(&s->pages[0].root.children.at(0).value);
    REQUIRE(g != nullptr);
    std::size_t depth = 0;
    for (const ClipPath* c = g->clip.get(); c != nullptr; c = c->clip.get()) ++depth;
    CHECK(depth <= 17);
  }
  SUBCASE("a scale that overflows") {
    Emf e;
    util::ByteWriter xf;
    xf.f32le(3e38f).f32le(0).f32le(0).f32le(3e38f).f32le(0).f32le(0);
    e.rec(35, xf);
    e.rec(43, {0, 0, 2000000000, 2000000000});
    e.rec(42, {0, 0, 2000000000, 2000000000});
    CHECK_OK(read("emf", e.finish()));
  }

}

TEST_CASE("vector/emf: fuzzing real files never crashes") {
  for (const char* name : {"ink_shapes.emf", "lo_text.emf", "lo_fills_dual.emf", "pstoedit_shapes.emf",
                           "ink_fills.emf"}) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Data>("emf", test::fixture(std::string("vector/metafile/") + name), 192) > 0);
  }
}

TEST_CASE("vector/emf: detection by content and extension") {
  const Bytes emf = test::fixture("vector/metafile/ink_shapes.emf");
  auto d = detect_format(Registry::instance(), emf, "");
  REQUIRE(d.has_value());
  CHECK(d->format->id == "emf");
  d = detect_format(Registry::instance(), emf, "picture.emf");
  REQUIRE(d.has_value());
  CHECK(d->format->id == "emf");
  CHECK(d->confidence >= 90);
  // " EMF" at offset 40 is required.
  Bytes other = emf;
  other[41] = std::byte{'X'};
  d = detect_format(Registry::instance(), other, "");
  CHECK((!d.has_value() || d->format->id != "emf"));
  CHECK(test::convert(emf, "", "emf").ok());  // same-format route
}
