// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "engine/detect.hpp"
#include "families/vector/geom.hpp"
#include "families/vector/vector_codecs.hpp"
#include "support/fixtures.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"
#include "util/deflate.hpp"

using namespace cvt;
using namespace cvt::vector;

namespace {

struct Item {
  enum class Kind { Shape, Text, Picture } kind = Kind::Shape;
  Rect box;
  Paint fill;
  Stroke stroke;
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
      it.box = bounds(s->path, m);
      it.fill = s->fill;
      it.stroke = s->stroke;
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

bool near(Color a, Color b, float tol = 0.01f) {
  return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol &&
         std::abs(a.a - b.a) <= tol;
}

const Item* find_fill(const std::vector<Item>& list, Color c) {
  for (const Item& it : list) {
    if (it.fill.type == Paint::Type::Solid && near(it.fill.color, c)) return &it;
  }
  return nullptr;
}

const Item* find_text(const std::vector<Item>& list, std::string_view text) {
  for (const Item& it : list) {
    if (it.kind == Item::Kind::Text && it.text == text) return &it;
  }
  return nullptr;
}

Result<Scene> read(std::string_view id, ByteView bytes, std::vector<std::string>* warnings = nullptr,
                   const Limits& limits = {}) {
  Context ctx;
  ctx.registry = &Registry::instance();
  ctx.limits = limits;
  auto scene = read_scene(id, bytes, ctx);
  if (warnings != nullptr) *warnings = ctx.warnings.list();
  return scene;
}

// ---- a WMF builder: parameters are 16-bit words in record order.

struct Wmf {
  util::ByteWriter body;
  std::uint16_t objects = 8;
  std::uint32_t max_record = 3;
  bool placeable = true;
  std::int16_t bbox_w = 1000, bbox_h = 1000;
  std::uint16_t inch = 1000;  // 1000 units per inch: 96 px per 1000 units

  Wmf& rec(std::uint16_t function, const util::ByteWriter& params) {
    const std::uint32_t words = static_cast<std::uint32_t>(3 + (params.size() + 1) / 2);
    body.u32le(words).u16le(function).bytes(params.view());
    if (params.size() % 2 == 1) body.u8(0);
    max_record = std::max(max_record, words);
    return *this;
  }
  Wmf& rec(std::uint16_t function, std::initializer_list<int> words) {
    util::ByteWriter p;
    for (const int w : words) p.i16le(static_cast<std::int16_t>(w));
    return rec(function, p);
  }
  Bytes finish() {
    rec(0x0000, {});
    util::ByteWriter out;
    if (placeable) {
      out.u32le(0x9AC6CDD7u).u16le(0).i16le(0).i16le(0).i16le(bbox_w).i16le(bbox_h).u16le(inch).u32le(0).u16le(0);
    }
    out.u16le(1).u16le(9).u16le(0x0300).u32le(static_cast<std::uint32_t>(9 + body.size() / 2)).u16le(objects);
    out.u32le(max_record).u16le(0);
    out.bytes(body.view());
    return out.take();
  }
};

util::ByteWriter colorref(int r, int g, int b) {
  util::ByteWriter p;
  p.u8(static_cast<std::uint8_t>(r)).u8(static_cast<std::uint8_t>(g)).u8(static_cast<std::uint8_t>(b)).u8(0);
  return p;
}

void brush(Wmf& w, int style, int r, int g, int b, int hatch = 0) {
  util::ByteWriter p;
  p.u16le(static_cast<std::uint16_t>(style)).bytes(colorref(r, g, b).view()).u16le(static_cast<std::uint16_t>(hatch));
  w.rec(0x02FC, p);
}

void pen(Wmf& w, int style, int width, int r, int g, int b) {
  util::ByteWriter p;
  p.u16le(static_cast<std::uint16_t>(style)).i16le(static_cast<std::int16_t>(width)).i16le(0).bytes(colorref(r, g, b).view());
  w.rec(0x02FA, p);
}

void font(Wmf& w, int height, int escapement, int weight, int charset, std::string_view face) {
  util::ByteWriter p;
  p.i16le(static_cast<std::int16_t>(height)).i16le(0).i16le(static_cast<std::int16_t>(escapement)).i16le(0);
  p.i16le(static_cast<std::int16_t>(weight)).u8(0).u8(0).u8(0).u8(static_cast<std::uint8_t>(charset)).u8(0).u8(0).u8(0).u8(0);
  for (std::size_t i = 0; i < 32; ++i) p.u8(i < face.size() ? static_cast<std::uint8_t>(face[i]) : 0);
  w.rec(0x02FB, p);
}

void text_out(Wmf& w, std::string_view bytes, int x, int y) {
  util::ByteWriter p;
  p.u16le(static_cast<std::uint16_t>(bytes.size())).text(bytes);
  if (bytes.size() % 2 == 1) p.u8(0);
  p.i16le(static_cast<std::int16_t>(y)).i16le(static_cast<std::int16_t>(x));
  w.rec(0x0521, p);
}

}  // namespace

// --------------------------------------------------------------------------- real files

TEST_CASE("vector/wmf: Inkscape drawing decodes to the original geometry and colours") {
  const auto scene = read("wmf", test::fixture("vector/metafile/ink_shapes.wmf"));
  REQUIRE_OK(scene);
  CHECK_APPROX(scene->pages[0].width, 399.92, 0.01);  // placeable 4999 x 3749 at 1200 units per inch
  CHECK_APPROX(scene->pages[0].height, 299.92, 0.01);
  CHECK(scene->source_units == "in");
  const auto list = items(*scene);
  const Item* red = find_fill(list, Color::rgb8(255, 0, 0));
  REQUIRE(red != nullptr);
  CHECK_APPROX(red->box.x0, 20.0, 0.2);
  CHECK_APPROX(red->box.y0, 20.0, 0.2);
  CHECK_APPROX(red->box.x1, 140.0, 0.2);
  CHECK_APPROX(red->box.y1, 100.0, 0.2);
  CHECK(near(red->stroke.paint.color, Color::rgb8(0, 0, 255)));
  CHECK_APPROX(red->stroke.width, 4.0, 0.1);
  const Item* green = find_fill(list, Color::rgb8(0, 0xc0, 0));
  REQUIRE(green != nullptr);
  CHECK_APPROX(green->box.width(), 120.0, 0.3);
  CHECK_APPROX(green->box.height(), 80.0, 0.3);
  std::size_t navy = 0, pictures = 0;
  for (const Item& it : list) {
    if (near(it.fill.color, Color::rgb8(0, 0, 0x80))) ++navy;  // "Hello EMF" as glyph outlines
    if (it.kind == Item::Kind::Picture) {
      ++pictures;
      CHECK_APPROX(it.box.x0, 300.0, 0.3);
      CHECK_APPROX(it.box.width(), 40.0, 0.3);
      CHECK(it.picture->pixels->width == 4);
    }
  }
  CHECK(navy == 8);
  CHECK(pictures == 1);
}

TEST_CASE("vector/wmf: LibreOffice drawing with text records, pictures and escapes") {
  const auto scene = read("wmf", test::fixture("vector/metafile/lo_shapes.wmf"));
  REQUIRE_OK(scene);
  const auto list = items(*scene);
  const Item* red = find_fill(list, Color::rgb8(255, 0, 0));
  REQUIRE(red != nullptr);
  CHECK_APPROX(red->box.width(), 120.0, 0.6);
  const Item* hello = find_text(list, "Hello EMF");
  REQUIRE(hello != nullptr);
  CHECK(hello->font.family == "DejaVu Sans");
  CHECK_APPROX(hello->font.size, 28.0, 0.6);
  CHECK(near(hello->fill.color, Color::rgb8(0, 0, 0x80)));
  bool picture = false;
  for (const Item& it : list) picture = picture || it.kind == Item::Kind::Picture;
  CHECK(picture);
}

TEST_CASE("vector/wmf: LibreOffice text with escapement") {
  const auto scene = read("wmf", test::fixture("vector/metafile/lo_text.wmf"));
  REQUIRE_OK(scene);
  const auto list = items(*scene);
  REQUIRE(find_text(list, "Lorem ipsum") != nullptr);
  const Item* rotated = find_text(list, "rotated");
  REQUIRE(rotated != nullptr);
  CHECK_APPROX(std::atan2(rotated->m.b, rotated->m.a) * 180 / std::numbers::pi, -30.0, 0.5);
}

TEST_CASE("vector/wmf: a WMF without placeable header takes its page from the window") {
  const Bytes placeable = test::fixture("vector/metafile/ink_shapes.wmf");
  const auto scene = read("wmf", ByteView(placeable).subspan(22));
  REQUIRE_OK(scene);
  // Window 4999 x 3749 units of unknown size: scaled down to 1024 px wide.
  CHECK_APPROX(scene->pages[0].width, 1024.0, 0.01);
  CHECK_APPROX(scene->pages[0].height, 1024.0 * 3749 / 4999, 0.01);
  CHECK(find_fill(items(*scene), Color::rgb8(255, 0, 0)) != nullptr);
}

TEST_CASE("vector/wmf: compressed WMZ reads like the WMF") {
  const auto plain = read("wmf", test::fixture("vector/metafile/ink_shapes.wmf"));
  const auto packed = read("wmz", test::fixture("vector/metafile/ink_shapes.wmz"));
  REQUIRE_OK(plain);
  REQUIRE_OK(packed);
  CHECK(items(*plain).size() == items(*packed).size());
  CHECK(packed->pages[0].width == plain->pages[0].width);
  Limits tight;
  tight.max_decompressed_bytes = 100;
  CHECK_STATUS(read("wmz", test::fixture("vector/metafile/ink_shapes.wmz"), nullptr, tight), Status::Limit);
}

// --------------------------------------------------------------------------- records (built by hand)

TEST_CASE("vector/wmf: mapping modes") {
  SUBCASE("placeable header and window define the page") {
    Wmf w;  // 1000 x 1000 units at 1000 per inch = 96 x 96 px
    w.rec(0x0103, {8});           // MM_ANISOTROPIC
    w.rec(0x020B, {0, 0});        // window org
    w.rec(0x020C, {500, 500});    // window ext (y, x): 500 units span the page
    w.rec(0x041B, {250, 250, 0, 0});  // rectangle bottom, right, top, left
    const auto s = read("wmf", w.finish());
    REQUIRE_OK(s);
    CHECK_APPROX(s->pages[0].width, 96.0, 1e-9);
    const auto list = items(*s);
    REQUIRE(list.size() == 1);
    CHECK_APPROX(list[0].box.x1, 48.0, 1e-9);
    CHECK_APPROX(list[0].box.y1, 48.0, 1e-9);
  }
  SUBCASE("a flipped window (negative extent) mirrors y") {
    Wmf w;
    w.rec(0x0103, {8});
    w.rec(0x020B, {1000, 0});      // origin at the bottom
    w.rec(0x020C, {-1000, 1000});  // y grows upwards
    w.rec(0x041B, {100, 100, 0, 0});
    const auto s = read("wmf", w.finish());
    REQUIRE_OK(s);
    const auto list = items(*s);
    REQUIRE(list.size() == 1);
    CHECK_APPROX(list[0].box.y0, 86.4, 1e-6);
    CHECK_APPROX(list[0].box.y1, 96.0, 1e-6);
  }
  SUBCASE("MM_TEXT with a placeable header scales the bounding box") {
    Wmf w;
    w.rec(0x041B, {500, 1000, 0, 0});
    const auto s = read("wmf", w.finish());
    REQUIRE_OK(s);
    CHECK_APPROX(items(*s)[0].box.x1, 96.0, 1e-9);
    CHECK_APPROX(items(*s)[0].box.y1, 48.0, 1e-9);
  }
  SUBCASE("MM_LOMETRIC without placeable header: 0.1 mm units, y up, page from the drawing") {
    Wmf w;
    w.placeable = false;
    w.rec(0x0103, {2});
    w.rec(0x041B, {-254, 254, 0, 0});  // 25.4 mm square below the origin
    const auto s = read("wmf", w.finish());
    REQUIRE_OK(s);
    // 96 px plus the default one-pixel pen.
    CHECK_APPROX(s->pages[0].width, 97.0, 0.01);
    CHECK_APPROX(s->pages[0].height, 97.0, 0.01);
  }
  SUBCASE("ScaleWindowExt, OffsetWindowOrg, SaveDC/RestoreDC") {
    Wmf w;
    w.rec(0x0103, {8});
    w.rec(0x020C, {1000, 1000});
    w.rec(0x001E, {});                 // SAVEDC
    w.rec(0x0410, {1, 2, 1, 2});       // scale window ext by 2/1 (yDenom, yNum, xDenom, xNum)
    w.rec(0x020F, {0, 100});           // offset window org by x 100
    w.rec(0x041B, {1000, 1100, 0, 100});
    w.rec(0x0127, {-1});               // RESTOREDC
    w.rec(0x041B, {500, 500, 0, 0});
    const auto s = read("wmf", w.finish());
    REQUIRE_OK(s);
    const auto list = items(*s);
    REQUIRE(list.size() == 2);
    CHECK_APPROX(list[0].box.x1, 48.0, 1e-9);
    CHECK_APPROX(list[1].box.x1, 48.0, 1e-9);
  }
}

TEST_CASE("vector/wmf: objects, pens, brushes and shapes") {
  Wmf w;
  w.rec(0x0102, {1});              // transparent background
  pen(w, 0x0200 | 0x1000, 20, 0, 0, 255);  // flat caps, bevel joins, 20 units = 1.92 px
  brush(w, 2, 255, 0, 0, 4);       // hatched (cross)
  w.rec(0x012D, {0});
  w.rec(0x012D, {1});
  w.rec(0x0418, {200, 300, 0, 0});  // ellipse
  w.rec(0x01F0, {1});               // delete the brush; a new one takes slot 1
  brush(w, 0, 0, 255, 0);
  w.rec(0x012D, {1});
  w.rec(0x061C, {50, 50, 400, 400, 300, 300});  // round rect: corner h, w, bottom, right, top, left
  w.rec(0x081A, {800, 900, 900, 1000, 1000, 1000, 800, 800});  // pie: end (y,x), start (y,x), box
  w.rec(0x0830, {900, 600, 900, 800, 1000, 800, 800, 600});    // chord
  w.rec(0x0817, {0, 0, 0, 0, 100, 100, 0, 0});                  // arc with equal radials: full ellipse
  {
    util::ByteWriter p;  // POLYPOLYGON with two squares, alternate fill
    p.u16le(2).u16le(4).u16le(4);
    for (const int v : {0, 500, 200, 500, 200, 700, 0, 700, 50, 550, 150, 550, 150, 650, 50, 650}) p.i16le(static_cast<std::int16_t>(v));
    w.rec(0x0538, p);
  }
  w.rec(0x0214, {900, 0});  // MoveTo (y, x)
  w.rec(0x0213, {900, 100});
  w.rec(0x0213, {950, 100});  // consecutive LineTo calls make one polyline
  const auto s = read("wmf", w.finish());
  REQUIRE_OK(s);
  const auto list = items(*s);
  REQUIRE(list.size() == 7);
  CHECK(list[0].fill.type == Paint::Type::Pattern);
  CHECK(list[0].stroke.cap == LineCap::Butt);
  CHECK(list[0].stroke.join == LineJoin::Bevel);
  CHECK_APPROX(list[0].stroke.width, 1.92, 1e-9);
  CHECK(near(list[1].fill.color, Color::rgb8(0, 255, 0)));
  CHECK_APPROX(list[1].box.width(), 9.6, 1e-6);
  CHECK(std::count(list[1].shape->path.verbs.begin(), list[1].shape->path.verbs.end(), Verb::Cubic) >= 4);
  // Pie from (1000, 900) to (900, 1000) counter-clockwise: the quarter to the upper right... of
  // a circle centred on (900, 900): from angle 0 to -90 degrees (y down).
  CHECK_APPROX(list[2].box.x0, 86.4, 1e-6);
  CHECK_APPROX(list[2].box.y1, 86.4, 1e-6);
  CHECK_APPROX(list[2].box.x1, 96.0, 1e-6);
  CHECK_APPROX(list[2].box.y0, 76.8, 1e-6);
  // Chord: the upper half of the circle centred on (700, 900).
  CHECK_APPROX(list[3].box.height(), 9.6, 1e-6);
  CHECK_APPROX(list[3].box.width(), 19.2, 1e-6);
  CHECK_APPROX(list[4].box.width(), 9.6, 1e-6);  // full ellipse
  CHECK(list[5].shape->fill_rule == FillRule::EvenOdd);
  CHECK(std::count(list[5].shape->path.verbs.begin(), list[5].shape->path.verbs.end(), Verb::Move) == 2);
  CHECK(list[6].shape->path.verbs.size() == 3);
  CHECK(!list[6].fill.visible());
}

TEST_CASE("vector/wmf: clipping and regions") {
  Wmf w;
  w.rec(0x0416, {500, 500, 100, 100});  // IntersectClipRect bottom, right, top, left
  w.rec(0x0415, {300, 300, 200, 200});  // ExcludeClipRect
  w.rec(0x041B, {1000, 1000, 0, 0});
  {
    util::ByteWriter p;  // CREATEREGION with one scan of one span
    p.u16le(0).i16le(6).u32le(0).i16le(0).i16le(1).i16le(0);
    p.i16le(0).i16le(0).i16le(100).i16le(100);
    p.u16le(2).i16le(0).i16le(100).i16le(0).i16le(50).u16le(2);
    w.rec(0x06FF, p);
  }
  w.rec(0x012C, {0});  // SELECTCLIPREGION
  w.rec(0x041B, {1000, 1000, 0, 0});
  w.rec(0x0220, {10, 20});  // OFFSETCLIPRGN y, x
  w.rec(0x041B, {1000, 1000, 0, 0});
  const auto s = read("wmf", w.finish());
  REQUIRE_OK(s);
  const Group& root = s->pages[0].root;
  REQUIRE(root.children.size() == 3);
  const auto* g0 = std::get_if<Group>(&root.children[0].value);
  REQUIRE(g0 != nullptr);
  CHECK(g0->clip->items[0].rule == FillRule::EvenOdd);
  REQUIRE(g0->clip->clip != nullptr);
  const auto* g1 = std::get_if<Group>(&root.children[1].value);
  REQUIRE(g1 != nullptr);
  const Rect r1 = bounds(g1->clip->items[0].path);
  CHECK_APPROX(r1.x1, 4.8, 1e-9);
  CHECK_APPROX(r1.y1, 9.6, 1e-9);
  const auto* g2 = std::get_if<Group>(&root.children[2].value);
  REQUIRE(g2 != nullptr);
  const Rect r2 = bounds(g2->clip->items[0].path);
  CHECK_APPROX(r2.x0, 1.92, 1e-9);
  CHECK_APPROX(r2.y0, 0.96, 1e-9);
}

TEST_CASE("vector/wmf: text in code pages, symbol fonts and alignment") {
  Wmf w;
  w.rec(0x0102, {1});
  font(w, -100, 0, 700, 204, "Arial");  // RUSSIAN_CHARSET
  w.rec(0x012D, {0});
  {
    util::ByteWriter c;
    c.u8(0x80).u8(0x40).u8(0).u8(0);
    w.rec(0x0209, c);  // SETTEXTCOLOR
  }
  text_out(w, "\xC6\xF3\xEA", 100, 200);  // "Жук" in Windows-1251
  font(w, -100, 900, 400, 2, "Symbol");    // SYMBOL_CHARSET, rotated 90 degrees
  w.rec(0x012D, {1});
  w.rec(0x012E, {24});  // TA_BASELINE
  text_out(w, "abW", 500, 500);
  font(w, -100, 0, 400, 238, "Arial");  // EASTEUROPE_CHARSET
  w.rec(0x012D, {2});
  w.rec(0x012E, {2 | 24});  // TA_RIGHT
  {
    util::ByteWriter p;  // EXTTEXTOUT with ETO_OPAQUE rectangle and Dx
    p.i16le(900).i16le(900).i16le(2).u16le(2);
    p.i16le(0).i16le(800).i16le(900).i16le(950);
    p.text("\x8A\xB9");  // "Šą"
    p.i16le(60).i16le(60);
    w.rec(0x0A32, p);
  }
  const auto s = read("wmf", w.finish());
  REQUIRE_OK(s);
  const auto list = items(*s);
  const Item* russian = find_text(list, "Жук");
  REQUIRE(russian != nullptr);
  CHECK(russian->font.weight == 700);
  CHECK_APPROX(russian->font.size, 9.6, 1e-9);
  CHECK(near(russian->fill.color, Color::rgb8(0x80, 0x40, 0)));
  CHECK(russian->box.y0 >= 19.2 - 0.01);  // TA_TOP: the text hangs below y = 200 units
  const Item* greek = find_text(list, "αβΩ");
  REQUIRE(greek != nullptr);
  CHECK_APPROX(std::atan2(greek->m.b, greek->m.a) * 180 / std::numbers::pi, -90.0, 1e-6);
  const Item* czech = find_text(list, "Šą");
  REQUIRE(czech != nullptr);
  CHECK(czech->box.x1 <= 86.4 + 0.01);  // right-aligned at x = 900 units minus the 120-unit advance
  CHECK(czech->box.x0 >= 86.4 - 11.52 - 0.5);
  const Item* bg = find_fill(list, Color::rgb8(255, 255, 255));
  REQUIRE(bg != nullptr);  // the default background colour fills the ETO_OPAQUE rectangle
}

TEST_CASE("vector/wmf: bitmaps and raster operations") {
  Wmf w;
  // A 2x1 24-bit DIB: black, white.
  util::ByteWriter dib;
  dib.u32le(40).i32le(2).i32le(1).u16le(1).u16le(24).u32le(0).u32le(8).i32le(0).i32le(0).u32le(0).u32le(0);
  dib.u8(0).u8(0).u8(0).u8(255).u8(255).u8(255).u8(0).u8(0);
  {
    util::ByteWriter p;  // STRETCHDIB SRCCOPY
    p.u32le(0x00CC0020).u16le(0).i16le(1).i16le(2).i16le(0).i16le(0).i16le(100).i16le(200).i16le(0).i16le(0);
    p.bytes(dib.view());
    w.rec(0x0F43, p);
  }
  {
    util::ByteWriter p;  // DIBSTRETCHBLT SRCAND: white becomes transparent
    p.u32le(0x008800C6).i16le(1).i16le(2).i16le(0).i16le(0).i16le(100).i16le(200).i16le(200).i16le(0);
    p.bytes(dib.view());
    w.rec(0x0B41, p);
  }
  {
    util::ByteWriter p;  // DIBBITBLT SRCPAINT: black becomes transparent
    p.u32le(0x00EE0086).i16le(0).i16le(0).i16le(1).i16le(2).i16le(400).i16le(0);
    p.bytes(dib.view());
    w.rec(0x0940, p);
  }
  brush(w, 0, 0, 0, 255);
  w.rec(0x012D, {0});
  {
    util::ByteWriter p;  // PATBLT PATCOPY
    p.u32le(0x00F00021).i16le(10).i16le(10).i16le(600).i16le(0);
    w.rec(0x061D, p);
  }
  {
    util::ByteWriter p;  // DIBCREATEPATTERNBRUSH
    p.u16le(5).u16le(0).bytes(dib.view());
    w.rec(0x0142, p);
  }
  w.rec(0x012D, {1});
  w.rec(0x041B, {900, 900, 700, 700});
  std::vector<std::string> warnings;
  const auto s = read("wmf", w.finish(), &warnings);
  REQUIRE_OK(s);
  const auto list = items(*s);
  REQUIRE(list.size() == 5);
  REQUIRE(list[0].kind == Item::Kind::Picture);
  CHECK_APPROX(list[0].box.width(), 19.2, 1e-9);
  CHECK(list[0].picture->pixels->rgba[3] == 255);
  REQUIRE(list[1].kind == Item::Kind::Picture);
  const ir::Frame& anded = *list[1].picture->pixels;
  CHECK(anded.rgba[3] == 255);  // black stays
  CHECK(anded.rgba[7] == 0);    // white disappears
  const ir::Frame& ored = *list[2].picture->pixels;
  CHECK(ored.rgba[3] == 0);
  CHECK(ored.rgba[7] == 255);
  CHECK(near(list[3].fill.color, Color::rgb8(0, 0, 255)));
  CHECK(list[4].fill.type == Paint::Type::Pattern);
  CHECK(!warnings.empty());
}

TEST_CASE("vector/wmf: damaged and hostile files fail cleanly") {
  const Bytes good = test::fixture("vector/metafile/ink_shapes.wmf");
  CHECK_STATUS(read("wmf", ByteView(good).first(30)), Status::Decode);
  {
    Bytes bad = good;
    bad[22] = std::byte{7};  // header type
    CHECK_STATUS(read("wmf", bad), Status::Decode);
  }
  {
    Bytes bad = good;  // first record claims 2^31 words
    bad[40] = std::byte{0};
    bad[41] = std::byte{0};
    bad[42] = std::byte{0};
    bad[43] = std::byte{0x80};
    CHECK_STATUS(read("wmf", bad), Status::Decode);
  }
  SUBCASE("object table bomb") {
    Wmf w;
    w.objects = 0xFFFF;
    for (int i = 0; i < 70000; ++i) pen(w, 0, 1, 0, 0, 0);
    CHECK_STATUS(read("wmf", w.finish()), Status::Limit);
  }
  SUBCASE("deep SaveDC, invalid selections and absurd counts") {
    Wmf w;
    for (int i = 0; i < 5000; ++i) w.rec(0x001E, {});
    w.rec(0x012D, {30000});
    w.rec(0x0324, {32767, 0, 0, 10, 10});  // POLYGON claiming 32767 points
    w.rec(0x0538, {30000, 1, 2});          // POLYPOLYGON with more polygons than data
    w.rec(0x0127, {-30000});
    std::vector<std::string> warnings;
    const auto s = read("wmf", w.finish(), &warnings);
    REQUIRE_OK(s);
    CHECK(!warnings.empty());
  }
  SUBCASE("bitmap dimensions beyond the limits") {
    Wmf w;
    util::ByteWriter p;
    p.u32le(0x00CC0020).u16le(0).i16le(1).i16le(1).i16le(0).i16le(0).i16le(10).i16le(10).i16le(0).i16le(0);
    p.u32le(40).i32le(60000).i32le(60000).u16le(1).u16le(1).u32le(0).u32le(0).i32le(0).i32le(0).u32le(0).u32le(0);
    p.zeros(16);
    w.rec(0x0F43, p);
    Limits limits;
    limits.max_pixels = 1000000;
    CHECK_STATUS(read("wmf", w.finish(), nullptr, limits), Status::Limit);
  }

}

TEST_CASE("vector/wmf: fuzzing real files never crashes") {
  for (const char* name : {"ink_shapes.wmf", "lo_text.wmf"}) {
    INFO(name);
    CHECK(test::fuzz_reader<ir::Data>("wmf", test::fixture(std::string("vector/metafile/") + name), 192) > 0);
  }
  CHECK(test::fuzz_reader<ir::Data>("wmz", test::fixture("vector/metafile/ink_shapes.wmz"), 128) > 0);
}

TEST_CASE("vector/wmf: detection of placeable, standard and compressed files") {
  const Bytes wmf = test::fixture("vector/metafile/ink_shapes.wmf");
  auto d = detect_format(Registry::instance(), wmf, "");
  REQUIRE(d.has_value());
  CHECK(d->format->id == "wmf");
  d = detect_format(Registry::instance(), ByteView(wmf).subspan(22), "");
  REQUIRE(d.has_value());
  CHECK(d->format->id == "wmf");
  const Bytes wmz = test::fixture("vector/metafile/ink_shapes.wmz");
  d = detect_format(Registry::instance(), wmz, "");
  REQUIRE(d.has_value());
  CHECK(d->format->id == "wmz");
  d = detect_format(Registry::instance(), wmz, "drawing.wmz");
  REQUIRE(d.has_value());
  CHECK(d->format->id == "wmz");
  const Bytes emz = test::fixture("vector/metafile/ink_shapes.emz");
  d = detect_format(Registry::instance(), emz, "");
  REQUIRE(d.has_value());
  CHECK(d->format->id == "emz");
  // A gzip file that holds something else is not a metafile.
  const Bytes other = *util::gzip_compress(as_bytes("just some text, not a picture at all"));
  d = detect_format(Registry::instance(), other, "");
  CHECK((!d.has_value() || (d->format->id != "wmz" && d->format->id != "emz")));
  CHECK(test::convert(wmf, "", "wmf").ok());
  CHECK(test::convert(wmz, "", "wmz", "", "drawing.wmz").ok());
}
