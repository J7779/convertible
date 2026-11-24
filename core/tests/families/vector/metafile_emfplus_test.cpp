// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// EMF+ records are built by hand from [MS-EMFPLUS]: no independent tool on
// the development machines writes EMF+ beyond LibreOffice's few records
// (see lo_fills_dual.emf in the EMF tests).
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "families/image/image_api.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/vector_codecs.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

struct Item {
  enum class Kind { Shape, Text, Picture } kind = Kind::Shape;
  Rect box;
  Paint fill;
  Stroke stroke;
  FillRule rule = FillRule::NonZero;
  std::string text;
  FontSpec font;
  const Picture* picture = nullptr;
  const Shape* shape = nullptr;
  Matrix m;
  const ClipPath* clip = nullptr;
};

void collect(const Group& g, const Matrix& parent, const ClipPath* clip, std::vector<Item>& out) {
  const Matrix m = parent * g.transform;
  if (g.clip) clip = g.clip.get();
  for (const Node& n : g.children) {
    Item it;
    it.m = m;
    it.clip = clip;
    if (const auto* s = std::get_if<Shape>(&n.value)) {
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
      collect(*sub, m, clip, out);
      continue;
    }
    out.push_back(std::move(it));
  }
}

std::vector<Item> items(const Scene& s) {
  std::vector<Item> out;
  if (!s.pages.empty()) collect(s.pages[0].root, Matrix{}, nullptr, out);
  return out;
}

bool near(Color a, Color b, float tol = 0.01f) {
  return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol &&
         std::abs(a.a - b.a) <= tol;
}

bool near(const Rect& a, const Rect& b, double tol = 1e-3) {
  return std::abs(a.x0 - b.x0) <= tol && std::abs(a.y0 - b.y0) <= tol && std::abs(a.x1 - b.x1) <= tol &&
         std::abs(a.y1 - b.y1) <= tol;
}

Result<Scene> read(ByteView bytes, std::string_view options = {}, std::vector<std::string>* warnings = nullptr,
                   const Limits& limits = {}) {
  Context ctx;
  ctx.registry = &Registry::instance();
  ctx.limits = limits;
  auto opts = Options::parse(options);
  if (!opts) return std::unexpected(opts.error());
  auto scene = read_scene("emf", bytes, ctx, *opts);
  if (warnings != nullptr) *warnings = ctx.warnings.list();
  return scene;
}

struct Emf {
  util::ByteWriter body;
  std::uint32_t records = 0;
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
  Bytes finish() {
    rec(14, {0, 16, 20});
    util::ByteWriter h;
    h.u32le(1).u32le(108).i32le(0).i32le(0).i32le(199).i32le(199);
    h.i32le(0).i32le(0).i32le(5292).i32le(5292);  // 200 px
    h.u32le(0x464D4520).u32le(0x10000).u32le(static_cast<std::uint32_t>(108 + body.size())).u32le(records + 1);
    h.u16le(8).u16le(0).u32le(0).u32le(0).u32le(0);
    h.i32le(9600).i32le(9600).i32le(2540).i32le(2540).u32le(0).u32le(0).u32le(0).u32le(2540000).u32le(2540000);
    h.bytes(body.view());
    return h.take();
  }
};

/// EMF+ records collected into one EMR_COMMENT.
struct Plus {
  util::ByteWriter w;
  Plus& rec(std::uint16_t type, std::uint16_t flags, const util::ByteWriter& data = {}) {
    w.u16le(type).u16le(flags).u32le(static_cast<std::uint32_t>(12 + data.size())).u32le(static_cast<std::uint32_t>(data.size()));
    w.bytes(data.view());
    return *this;
  }
  Plus& header() {
    util::ByteWriter d;
    d.u32le(0xDBC01002u).u32le(1).u32le(96).u32le(96);
    return rec(0x4001, 1, d);
  }
  void into(Emf& e) const {
    util::ByteWriter c;
    c.u32le(static_cast<std::uint32_t>(4 + w.size())).u32le(0x2B464D45u).bytes(w.view());
    e.rec(70, c);
  }
};

util::ByteWriter floats(std::initializer_list<double> values) {
  util::ByteWriter p;
  for (const double v : values) p.f32le(static_cast<float>(v));
  return p;
}

util::ByteWriter solid_brush(std::uint32_t argb) {
  util::ByteWriter b;
  b.u32le(0xDBC01002u).u32le(0).u32le(argb);
  return b;
}

util::ByteWriter pen(float width, std::uint32_t argb, std::uint32_t flags = 0, const util::ByteWriter& extra = {},
                     std::uint32_t unit = 2) {
  util::ByteWriter p;
  p.u32le(0xDBC01002u).u32le(0).u32le(flags).u32le(unit).f32le(width).bytes(extra.view());
  p.bytes(solid_brush(argb).view());
  return p;
}

util::ByteWriter path(const std::vector<Point>& pts, const std::vector<std::uint8_t>& types) {
  util::ByteWriter p;
  p.u32le(0xDBC01002u).u32le(static_cast<std::uint32_t>(pts.size())).u16le(0).u16le(0);
  for (const Point q : pts) p.f32le(static_cast<float>(q.x)).f32le(static_cast<float>(q.y));
  for (const std::uint8_t t : types) p.u8(t);
  p.align(4);
  return p;
}

constexpr std::uint16_t obj(int type, int id) { return static_cast<std::uint16_t>((type << 8) | id); }

}  // namespace

TEST_CASE("vector/emfplus: brushes, pens, paths and primitives") {
  Emf e;
  Plus p;
  p.header();
  p.rec(0x4008, obj(1, 0), solid_brush(0xFFFF0000u));                 // brush 0: red
  p.rec(0x4008, obj(2, 1), pen(4, 0xFF0000FFu));                      // pen 1: 4 px blue
  p.rec(0x4008, obj(3, 2), path({{10, 100}, {50, 60}, {90, 140}, {130, 100}, {130, 150}},
                                {0x00, 0x03, 0x03, 0x03, 0x81}));  // bezier then a closing line
  {
    util::ByteWriter d;  // FillRects with brush object 0
    d.u32le(0).u32le(1).bytes(floats({10, 10, 40, 20}).view());
    p.rec(0x400A, 0, d);
  }
  {
    util::ByteWriter d;  // FillRects, colour brush, 16-bit rectangles
    d.u32le(0x8000FF00u).u32le(2).i16le(60).i16le(10).i16le(10).i16le(10).i16le(80).i16le(10).i16le(10).i16le(10);
    p.rec(0x400A, 0x8000 | 0x4000, d);
  }
  {
    util::ByteWriter d;  // DrawLines with pen 1, closed
    d.u32le(3).bytes(floats({10, 40, 50, 40, 30, 60}).view());
    p.rec(0x400D, 0x2000 | 1, d);
  }
  {
    util::ByteWriter d;  // FillPath 2 with a colour
    d.u32le(0xFF00FFFFu);
    p.rec(0x4014, 0x8000 | 2, d);
  }
  {
    util::ByteWriter d;  // DrawPath 2 with pen 1
    d.u32le(1);
    p.rec(0x4015, 2, d);
  }
  {
    util::ByteWriter d;  // FillPolygon with relative points: (100,10) then +10,+0 then +0,+10 (7-bit) ...
    d.u32le(0xFF808080u).u32le(4);
    d.u8(0x80 | 0x00).u8(100).u8(10);  // 15-bit 100, 7-bit 10
    d.u8(10).u8(0).u8(0).u8(10).u8(0x76).u8(0);  // +10,0 ; 0,+10 ; -10,0
    p.rec(0x400C, 0x8000 | 0x0800, d);
  }
  {
    util::ByteWriter d;  // FillEllipse
    d.u32le(0xFF000000u).bytes(floats({150, 10, 40, 20}).view());
    p.rec(0x400E, 0x8000, d);
  }
  {
    util::ByteWriter d;  // FillPie: start 0, sweep 90 (clockwise in y-down space)
    d.u32le(0xFF00FF00u).bytes(floats({0, 90, 150, 50, 40, 40}).view());
    p.rec(0x4010, 0x8000, d);
  }
  {
    util::ByteWriter d;  // DrawArc with pen 1
    d.bytes(floats({180, 90, 150, 100, 40, 40}).view());
    p.rec(0x4012, 1, d);
  }
  p.into(e);
  e.rec(43, {0, 0, 199, 199});  // a GDI rectangle hidden by EMF+
  const Bytes file = e.finish();
  const auto s = read(file);
  REQUIRE_OK(s);
  const auto list = items(*s);
  REQUIRE(list.size() == 9);
  CHECK(near(list[0].fill.color, Color::rgb8(255, 0, 0)));
  CHECK(near(list[0].box, Rect{10, 10, 50, 30}));
  CHECK(near(list[1].fill.color, Color::rgb8(0, 255, 0, 128)));
  CHECK(near(list[1].box, Rect{60, 10, 90, 20}));
  CHECK(near(list[2].stroke.paint.color, Color::rgb8(0, 0, 255)));
  CHECK_APPROX(list[2].stroke.width, 4.0, 1e-6);
  CHECK(list[2].shape->path.verbs.back() == Verb::Close);
  CHECK(list[3].rule == FillRule::EvenOdd);
  CHECK(std::count(list[3].shape->path.verbs.begin(), list[3].shape->path.verbs.end(), Verb::Cubic) == 1);
  CHECK(list[3].shape->path.verbs.back() == Verb::Close);
  CHECK(list[4].stroke.paint.visible());
  CHECK(near(list[5].box, Rect{100, 10, 110, 20}));
  CHECK(near(list[6].box, Rect{150, 10, 190, 30}));
  // Pie from 0 to 90 degrees clockwise on screen: the lower right quarter.
  CHECK(near(list[7].box, Rect{170, 70, 190, 90}));
  // Arc from 180 (left) clockwise to 270 (top): the upper left quarter.
  CHECK(near(list[8].box, Rect{150, 100, 170, 120}));

  // With EMF+ off only the GDI rectangle is drawn.
  const auto gdi = read(file, R"({"emf_plus":"off"})");
  REQUIRE_OK(gdi);
  REQUIRE(items(*gdi).size() == 1);
  CHECK(near(items(*gdi)[0].box, Rect{0, 0, 199, 199}));
}

TEST_CASE("vector/emfplus: GetDC lets the following GDI records draw") {
  Emf e;
  {
    Plus p;
    p.header();
    p.rec(0x4004, 0);
    p.into(e);
  }
  e.rec(43, {10, 10, 20, 20});  // drawn: follows GetDC
  {
    Plus p;
    util::ByteWriter d;
    d.u32le(0xFF0000FFu).u32le(1).bytes(floats({30, 30, 10, 10}).view());
    p.rec(0x400A, 0x8000, d);
    p.into(e);
  }
  e.rec(43, {50, 50, 60, 60});  // hidden: an EMF+ record came after GetDC
  const auto s = read(e.finish());
  REQUIRE_OK(s);
  const auto list = items(*s);
  REQUIRE(list.size() == 2);
  CHECK(near(list[0].box, Rect{10, 10, 20, 20}));
  CHECK(near(list[1].box, Rect{30, 30, 40, 40}));
}

TEST_CASE("vector/emfplus: world and page transforms, save/restore and containers") {
  Emf e;
  Plus p;
  p.header();
  const auto fill = [&p](double x, double y, double w, double h) {
    util::ByteWriter d;
    d.u32le(0xFF000000u).u32le(1).bytes(floats({x, y, w, h}).view());
    p.rec(0x400A, 0x8000, d);
  };
  p.rec(0x402D, 0, floats({100, 0}));  // translate (prepend)
  util::ByteWriter save;
  save.u32le(7);
  p.rec(0x4025, 0, save);
  p.rec(0x402E, 0, floats({2, 3}));     // scale, prepended: applies first
  fill(0, 0, 10, 10);                   // -> (100,0)-(120,30)
  p.rec(0x4026, 0, save);               // restore
  p.rec(0x402F, 0x2000, floats({90}));  // rotate 90, appended: after the translation
  fill(0, 0, 10, 10);                   // translate (100,0) then rotate: (0..10 + 100) -> x' = -y, y' = x
  p.rec(0x402B, 0);                     // reset
  {
    util::ByteWriter d;  // SetWorldTransform
    d.bytes(floats({1, 0, 0, 1, 5, 5}).view());
    p.rec(0x402A, 0, d);
  }
  p.rec(0x4030, 6, floats({1}));  // page unit millimetre, scale 1
  fill(0, 0, 10, 10);             // 10 mm = 37.8 px, after the 5 mm offset
  p.rec(0x402B, 0);
  p.rec(0x4030, 2, floats({1}));
  {
    util::ByteWriter d;  // BeginContainer: src (0,0,1,1) -> dst (50,50,20,20) pixels
    d.bytes(floats({50, 50, 20, 20, 0, 0, 1, 1}).view()).u32le(9);
    p.rec(0x4027, 2, d);
  }
  fill(0, 0, 1, 1);
  {
    util::ByteWriter d;
    d.u32le(9);
    p.rec(0x4029, 0, d);  // EndContainer
  }
  fill(0, 0, 1, 1);
  p.into(e);
  const auto s = read(e.finish());
  REQUIRE_OK(s);
  const auto list = items(*s);
  REQUIRE(list.size() == 5);
  CHECK(near(list[0].box, Rect{100, 0, 120, 30}));
  CHECK(near(list[1].box, Rect{-10, 100, 0, 110}));
  const double mm = 96 / 25.4;
  CHECK(near(list[2].box, Rect{5 * mm, 5 * mm, 15 * mm, 15 * mm}, 1e-3));
  CHECK(near(list[3].box, Rect{50, 50, 70, 70}));
  CHECK(near(list[4].box, Rect{0, 0, 1, 1}));
}

TEST_CASE("vector/emfplus: clipping with rectangles, paths and regions") {
  Emf e;
  Plus p;
  p.header();
  const auto fill = [&p] {
    util::ByteWriter d;
    d.u32le(0xFF000000u).u32le(1).bytes(floats({0, 0, 200, 200}).view());
    p.rec(0x400A, 0x8000, d);
  };
  p.rec(0x4032, 0x0000, floats({10, 10, 100, 100}));  // replace
  p.rec(0x4032, 0x0100, floats({50, 50, 100, 100}));  // intersect
  fill();
  // Region object: rect minus rect (exclude).
  {
    util::ByteWriter r;
    r.u32le(0xDBC01002u).u32le(2);
    r.u32le(4);  // exclude
    r.u32le(0x10000000u).bytes(floats({0, 0, 100, 100}).view());
    r.u32le(0x10000000u).bytes(floats({25, 25, 50, 50}).view());
    p.rec(0x4008, obj(4, 3), r);
  }
  p.rec(0x4034, 0x0000 | 3, {});  // SetClipRegion replace
  fill();
  p.rec(0x4031, 0);  // ResetClip
  fill();
  p.into(e);
  const auto s = read(e.finish());
  REQUIRE_OK(s);
  const auto list = items(*s);
  REQUIRE(list.size() == 3);
  REQUIRE(list[0].clip != nullptr);
  CHECK(near(bounds(list[0].clip->items[0].path), Rect{50, 50, 150, 150}));
  REQUIRE(list[0].clip->clip != nullptr);
  CHECK(near(bounds(list[0].clip->clip->items[0].path), Rect{10, 10, 110, 110}));
  REQUIRE(list[1].clip != nullptr);
  CHECK(list[1].clip->items[0].rule == FillRule::EvenOdd);
  REQUIRE(list[1].clip->clip != nullptr);
  CHECK(near(bounds(list[1].clip->clip->items[0].path), Rect{0, 0, 100, 100}));
  CHECK(list[2].clip == nullptr);
}

TEST_CASE("vector/emfplus: strings, driver strings and fonts") {
  Emf e;
  Plus p;
  p.header();
  {
    util::ByteWriter f;  // font 0: 20 px bold, "Arial"
    f.u32le(0xDBC01002u).f32le(20).u32le(2).i32le(1).u32le(0).u32le(5);
    for (const char16_t c : std::u16string(u"Arial")) f.u16le(c);
    p.rec(0x4008, obj(6, 0), f);
  }
  {
    util::ByteWriter f;  // string format 1: centred both ways
    f.u32le(0xDBC01002u).u32le(0).u32le(0).u32le(1).u32le(1).zeros(40);
    p.rec(0x4008, obj(7, 1), f);
  }
  {
    util::ByteWriter d;  // DrawString "Hi" in (0,0,200,100)
    d.u32le(0xFF0000FFu).u32le(1).u32le(2).bytes(floats({0, 0, 200, 100}).view()).u16le('H').u16le('i');
    p.rec(0x401C, 0x8000 | 0, d);
  }
  {
    util::ByteWriter d;  // DrawDriverString "ok" with explicit positions
    d.u32le(0xFFFF0000u).u32le(1).u32le(0).u32le(2).u16le('o').u16le('k');
    d.bytes(floats({20, 150, 40, 150}).view());
    p.rec(0x4036, 0x8000 | 0, d);
  }
  {
    util::ByteWriter d;  // glyph indices cannot be shown
    d.u32le(0xFFFF0000u).u32le(0).u32le(0).u32le(1).u16le(42).bytes(floats({20, 180}).view());
    p.rec(0x4036, 0x8000 | 0, d);
  }
  p.into(e);
  std::vector<std::string> warnings;
  const auto s = read(e.finish(), {}, &warnings);
  REQUIRE_OK(s);
  const auto list = items(*s);
  REQUIRE(list.size() == 2);
  CHECK(list[0].text == "Hi");
  CHECK(list[0].font.family == "Arial");
  CHECK(list[0].font.weight == 700);
  CHECK_APPROX(list[0].font.size, 20.0, 1e-6);
  CHECK(near(list[0].fill.color, Color::rgb8(0, 0, 255)));
  CHECK_APPROX((list[0].box.x0 + list[0].box.x1) / 2, 100.0, 2.0);
  CHECK_APPROX((list[0].box.y0 + list[0].box.y1) / 2, 50.0, 4.0);
  CHECK(list[1].text == "ok");
  CHECK_APPROX(list[1].box.x0, 20.0, 2.0);
  CHECK(list[1].box.y1 <= 150.5);
  CHECK(!warnings.empty());
}

TEST_CASE("vector/emfplus: images, gradient and texture brushes, continued objects") {
  Emf e;
  Plus p;
  p.header();
  util::ByteWriter img;  // image 0: 2x1 32-bit ARGB bitmap
  img.u32le(0xDBC01002u).u32le(1).i32le(2).i32le(1).i32le(8).u32le(0x0026200Au).u32le(0);
  img.u8(0).u8(0).u8(255).u8(255).u8(255).u8(0).u8(0).u8(128);  // red opaque, blue half transparent
  p.rec(0x4008, obj(5, 0), img);
  {
    util::ByteWriter d;  // DrawImage: whole image into (10,10,40,20)
    d.u32le(0).u32le(2).bytes(floats({0, 0, 2, 1, 10, 10, 40, 20}).view());
    p.rec(0x401A, 0, d);
  }
  {
    util::ByteWriter d;  // DrawImagePoints: a parallelogram
    d.u32le(0).u32le(2).bytes(floats({0, 0, 2, 1}).view()).u32le(3).bytes(floats({100, 10, 140, 20, 100, 40}).view());
    p.rec(0x401B, 0, d);
  }
  {
    util::ByteWriter b;  // brush 1: linear gradient red -> blue over x 0..100, preset colours
    b.u32le(0xDBC01002u).u32le(4).u32le(0x04).i32le(4).bytes(floats({0, 0, 100, 10}).view());
    b.u32le(0xFFFF0000u).u32le(0xFF0000FFu).u32le(0).u32le(0);
    b.u32le(3).bytes(floats({0, 0.5, 1}).view()).u32le(0xFFFF0000u).u32le(0xFF00FF00u).u32le(0xFF0000FFu);
    p.rec(0x4008, obj(1, 1), b);
  }
  {
    util::ByteWriter d;
    d.u32le(1).u32le(1).bytes(floats({0, 50, 100, 10}).view());
    p.rec(0x400A, 0, d);
  }
  {
    util::ByteWriter b;  // brush 2: hatch style 0 (horizontal), white on black
    b.u32le(0xDBC01002u).u32le(1).u32le(0).u32le(0xFFFFFFFFu).u32le(0xFF000000u);
    p.rec(0x4008, obj(1, 2), b);
    util::ByteWriter d;
    d.u32le(2).u32le(1).bytes(floats({0, 70, 10, 10}).view());
    p.rec(0x400A, 0, d);
  }
  {
    // pen 3 split over two records: the first carries the total size.
    const util::ByteWriter whole = pen(3, 0xFF00FF00u);
    const ByteView all = whole.view();
    util::ByteWriter first;
    first.u32le(static_cast<std::uint32_t>(all.size())).bytes(all.first(10));
    p.rec(0x4008, static_cast<std::uint16_t>(0x8000 | obj(2, 3)), first);
    util::ByteWriter rest;
    rest.u32le(static_cast<std::uint32_t>(all.size())).bytes(all.subspan(10));
    p.rec(0x4008, static_cast<std::uint16_t>(0x8000 | obj(2, 3)), rest);
    util::ByteWriter d;
    d.u32le(2).bytes(floats({0, 90, 100, 90}).view());
    p.rec(0x400D, 3, d);
  }
  p.into(e);
  const auto s = read(e.finish());
  REQUIRE_OK(s);
  const auto list = items(*s);
  REQUIRE(list.size() == 5);
  REQUIRE(list[0].kind == Item::Kind::Picture);
  CHECK(near(list[0].box, Rect{10, 10, 50, 30}));
  const ir::Frame& f = *list[0].picture->pixels;
  CHECK(f.rgba[0] == 255);
  CHECK(f.rgba[3] == 255);
  CHECK(f.rgba[6] == 255);
  CHECK(f.rgba[7] == 128);
  REQUIRE(list[1].kind == Item::Kind::Picture);
  CHECK_APPROX(list[1].m.c / 1.0, 0.0, 1e-9);  // picture space x stays horizontal...
  CHECK(near(list[1].box, Rect{100, 10, 140, 50}));  // ... and the parallelogram is sheared
  REQUIRE(list[2].fill.type == Paint::Type::Gradient);
  const Gradient& g = *list[2].fill.gradient;
  REQUIRE(g.stops.size() == 3);
  CHECK(near(g.stops[1].color, Color::rgb8(0, 255, 0)));
  CHECK(g.spread == Spread::Pad);
  CHECK(list[3].fill.type == Paint::Type::Pattern);
  CHECK(near(list[4].stroke.paint.color, Color::rgb8(0, 255, 0)));
  CHECK_APPROX(list[4].stroke.width, 3.0, 1e-6);
}

TEST_CASE("vector/emfplus: hostile records fail cleanly") {
  SUBCASE("a continued object claiming a gigabyte") {
    Emf e;
    Plus p;
    p.header();
    util::ByteWriter d;
    d.u32le(0x40000000u).zeros(16);
    p.rec(0x4008, static_cast<std::uint16_t>(0x8000 | obj(2, 1)), d);
    p.into(e);
    Limits limits;
    limits.max_decompressed_bytes = 1 << 20;
    CHECK_STATUS(read(e.finish(), {}, nullptr, limits), Status::Limit);
  }
  SUBCASE("deeply nested regions") {
    Emf e;
    Plus p;
    p.header();
    util::ByteWriter r;
    r.u32le(0xDBC01002u).u32le(400);
    for (int i = 0; i < 200; ++i) r.u32le(1);  // AND nodes nested on the left
    r.u32le(0x10000003u);
    p.rec(0x4008, obj(4, 1), r);
    p.rec(0x4034, 1, {});
    p.into(e);
    const auto s = read(e.finish());
    CHECK((s.has_value() || s.error().code == Status::Limit));
  }
  SUBCASE("sizes and counts that do not fit") {
    Emf e;
    Plus p;
    p.header();
    util::ByteWriter d;
    d.u32le(0x7FFFFFFF).bytes(floats({0, 0, 1, 1}).view());
    p.rec(0x400D, 1, d);                         // DrawLines with an absurd count
    p.rec(0x400A, 0x8000, floats({1}));          // FillRects without data
    util::ByteWriter img;                        // a 100000 x 100000 bitmap without pixels
    img.u32le(0xDBC01002u).u32le(1).i32le(100000).i32le(100000).i32le(400000).u32le(0x0026200Au).u32le(0);
    p.rec(0x4008, obj(5, 2), img);
    p.into(e);
    util::ByteWriter bad;  // a comment whose EMF+ record overruns it
    bad.u32le(24).u32le(0x2B464D45u).u16le(0x400A).u16le(0).u32le(1000).u32le(988);
    e.rec(70, bad);
    CHECK_STATUS(read(e.finish()), Status::Limit);  // the bitmap
  }
  SUBCASE("fuzzing a file with many record types") {
    Emf e;
    Plus p;
    p.header();
    p.rec(0x4008, obj(1, 0), solid_brush(0xFFFF0000u));
    p.rec(0x4008, obj(2, 1), pen(2, 0xFF0000FFu));
    p.rec(0x4008, obj(3, 2), path({{0, 0}, {10, 0}, {10, 10}}, {0, 1, 0x81}));
    util::ByteWriter d;
    d.u32le(0).u32le(1).bytes(floats({1, 1, 5, 5}).view());
    p.rec(0x400A, 0, d);
    util::ByteWriter dp;
    dp.u32le(1);
    p.rec(0x4015, 2, dp);
    p.rec(0x4032, 0x0100, floats({0, 0, 50, 50}));
    p.into(e);
    CHECK(test::fuzz_reader<ir::Data>("emf", e.finish(), 256) > 0);
  }
}
