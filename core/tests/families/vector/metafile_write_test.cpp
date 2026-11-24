// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "families/vector/font.hpp"
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
  FillRule rule = FillRule::NonZero;
  std::string text;
  FontSpec font;
  const Picture* picture = nullptr;
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
      it.rule = s->fill_rule;
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

bool near(const Rect& a, const Rect& b, double tol) {
  return std::abs(a.x0 - b.x0) <= tol && std::abs(a.y0 - b.y0) <= tol && std::abs(a.x1 - b.x1) <= tol &&
         std::abs(a.y1 - b.y1) <= tol;
}

const Item* find_fill(const std::vector<Item>& list, Color c, float tol = 0.01f) {
  for (const Item& it : list) {
    if (it.fill.type == Paint::Type::Solid && near(it.fill.color, c, tol)) return &it;
  }
  return nullptr;
}

Context make_ctx(const Limits& limits = {}) {
  Context ctx;
  ctx.registry = &Registry::instance();
  ctx.limits = limits;
  return ctx;
}

Result<Bytes> write(std::string_view id, const Scene& scene, std::string_view options = {},
                    std::vector<std::string>* warnings = nullptr, const Limits& limits = {}) {
  Context ctx = make_ctx(limits);
  auto opts = Options::parse(options);
  if (!opts) return std::unexpected(opts.error());
  auto out = write_scene(id, scene, ctx, *opts);
  if (warnings != nullptr) *warnings = ctx.warnings.list();
  return out;
}

Result<Scene> read(std::string_view id, ByteView bytes) {
  Context ctx = make_ctx();
  return read_scene(id, bytes, ctx);
}

Node shape(Path path, Paint fill, Stroke stroke = {}) {
  Shape s;
  s.path = std::move(path);
  s.fill = std::move(fill);
  s.stroke = std::move(stroke);
  return Node{std::move(s)};
}

Path rect(double x, double y, double w, double h) {
  Path p;
  add_rect(p, x, y, w, h);
  return p;
}

Scene one_page(double w = 300, double h = 200) {
  Scene s;
  Page p;
  p.width = w;
  p.height = h;
  s.pages.push_back(std::move(p));
  return s;
}

Group& root(Scene& s) { return s.pages[0].root; }

Paint gradient_paint(Gradient::Type type) {
  auto g = std::make_shared<Gradient>();
  g->type = type;
  if (type == Gradient::Type::Linear) {
    g->p1 = {0, 0};
    g->p2 = {100, 0};
  } else {
    g->center = {50, 50};
    g->radius = 50;
  }
  g->stops = {{0, {1, 1, 0, 1}}, {1, {0, 0, 1, 1}}};
  Paint p;
  p.type = Paint::Type::Gradient;
  p.gradient = std::move(g);
  return p;
}

std::shared_ptr<ir::Frame> checker(std::uint32_t w, std::uint32_t h, bool with_alpha) {
  auto f = std::make_shared<ir::Frame>(ir::Frame::filled(w, h, {0, 0, 0, 255}));
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t at = f->offset(x, y);
      f->rgba[at] = static_cast<std::uint8_t>(x * 40);
      f->rgba[at + 1] = static_cast<std::uint8_t>(y * 40);
      f->rgba[at + 2] = static_cast<std::uint8_t>((x + y) % 2 == 0 ? 255 : 0);
      f->rgba[at + 3] = with_alpha ? static_cast<std::uint8_t>(255 - x * 50) : 255;
    }
  }
  return f;
}

std::uint32_t u32_at(const Bytes& b, std::size_t at) {
  util::ByteReader r(b);
  r.seek(at);
  return r.u32le();
}

}  // namespace

TEST_CASE("vector/emf: writer round trip keeps geometry, colours and strokes") {
  Scene s = one_page();
  Stroke st;
  st.paint = Paint::solid({0, 0, 1, 1});
  st.width = 4;
  st.cap = LineCap::Square;
  st.join = LineJoin::Bevel;
  root(s).children.push_back(shape(rect(10, 10, 80, 50), Paint::solid({1, 0, 0, 1}), st));
  Path curve;
  curve.move_to({10, 150});
  curve.cubic_to({50, 100}, {90, 200}, {130, 150});
  curve.quad_to({150, 120}, {170, 150});
  Stroke thin;
  thin.paint = Paint::solid({0, 0.5f, 0, 1});
  thin.width = 1.5;
  root(s).children.push_back(shape(curve, Paint::none(), thin));
  Path holes = rect(200, 100, 80, 80);
  append(holes, rect(220, 120, 40, 40));
  Shape eo;
  eo.path = holes;
  eo.fill = Paint::solid({0.2f, 0.4f, 0.6f, 1});
  eo.fill_rule = FillRule::EvenOdd;
  root(s).children.push_back(Node{eo});
  Group moved;
  moved.transform = Matrix::translate(100, 20) * Matrix::scale(2, 2);
  moved.children.push_back(shape(rect(0, 0, 10, 10), Paint::solid({1, 0.5f, 0, 1})));
  root(s).children.push_back(Node{moved});
  Stroke hair;
  hair.paint = Paint::solid({0, 0, 0, 1});
  hair.hairline = true;
  root(s).children.push_back(shape(rect(5, 180, 50, 10), Paint::none(), hair));

  const auto emf = write("emf", s);
  REQUIRE_OK(emf);
  const auto back = read("emf", *emf);
  REQUIRE_OK(back);
  CHECK_APPROX(back->pages[0].width, 300.0, 0.03);  // the frame is stored in 0.01 mm
  CHECK_APPROX(back->pages[0].height, 200.0, 0.03);
  const auto list = items(*back);
  const Item* red = find_fill(list, Color{1, 0, 0, 1});
  REQUIRE(red != nullptr);
  CHECK(near(red->box, Rect{10, 10, 90, 60}, 0.05));
  CHECK(near(red->stroke.paint.color, Color{0, 0, 1, 1}));
  CHECK_APPROX(red->stroke.width, 4.0, 0.05);
  CHECK(red->stroke.cap == LineCap::Square);
  CHECK(red->stroke.join == LineJoin::Bevel);
  bool curve_found = false;
  for (const Item& it : list) {
    if (near(it.stroke.paint.color, Color{0, 0.5f, 0, 1}, 0.01f)) {
      curve_found = true;
      CHECK(near(it.box, bounds(curve), 0.1));
      CHECK_APPROX(it.stroke.width, 1.5, 0.05);
    }
    if (it.stroke.paint.visible() && near(it.stroke.paint.color, Color{0, 0, 0, 1}) && !it.fill.visible()) {
      CHECK(it.stroke.hairline);
    }
  }
  CHECK(curve_found);
  const Item* holed = find_fill(list, Color{0.2f, 0.4f, 0.6f, 1});
  REQUIRE(holed != nullptr);
  CHECK(holed->rule == FillRule::EvenOdd);
  const Item* orange = find_fill(list, Color{1, 0.5f, 0, 1});
  REQUIRE(orange != nullptr);
  CHECK(near(orange->box, Rect{100, 20, 120, 40}, 0.05));
}

TEST_CASE("vector/emf: writer header, records and determinism") {
  Scene s = one_page(400.4, 250.6);  // not whole pixels
  root(s).children.push_back(shape(rect(10, 10, 380, 50), Paint::solid({1, 0, 0, 1})));
  const auto a = write("emf", s);
  const auto b = write("emf", s);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  CHECK(*a == *b);
  CHECK(u32_at(*a, 0) == 1);
  CHECK(u32_at(*a, 40) == 0x464D4520u);
  CHECK(u32_at(*a, 48) == a->size());                                 // nBytes
  CHECK_APPROX(static_cast<double>(u32_at(*a, 32)), 400.4 * 2540.0 / 96, 1.0);  // frame right (0.01 mm)
  CHECK_APPROX(static_cast<double>(u32_at(*a, 36)), 250.6 * 2540.0 / 96, 1.0);
  const auto back = read("emf", *a);
  REQUIRE_OK(back);
  CHECK_APPROX(back->pages[0].width, 400.4, 0.03);
  REQUIRE(items(*back).size() == 1);
  CHECK(near(items(*back)[0].box, Rect{10, 10, 390, 60}, 0.04));
  CHECK(u32_at(*a, 72) == 9600);   // reference device: 96 dpi
  CHECK(u32_at(*a, 80) == 2540);
  // The record count matches the records present, and the last one is EOF.
  std::size_t pos = u32_at(*a, 4), count = 1;
  std::uint32_t last = 0;
  while (pos + 8 <= a->size()) {
    last = u32_at(*a, pos);
    pos += u32_at(*a, pos + 4);
    ++count;
  }
  CHECK(pos == a->size());
  CHECK(count == u32_at(*a, 52));
  CHECK(last == 14);
  // No description or other identifying data.
  CHECK(u32_at(*a, 60) == 0);
}

TEST_CASE("vector/emf: transparency, clipping and gradients survive as metafile records") {
  Scene s = one_page();
  root(s).children.push_back(shape(rect(10, 10, 50, 50), Paint::solid({0, 0.6f, 0, 1}, 0.5)));
  Group clipped;
  Path circle;
  add_ellipse(circle, 150, 50, 40, 40);
  auto clip = std::make_shared<ClipPath>();
  clip->items.push_back({circle, FillRule::NonZero});
  clipped.clip = clip;
  clipped.children.push_back(shape(rect(100, 0, 100, 100), Paint::solid({0, 0.8f, 0.8f, 1})));
  root(s).children.push_back(Node{clipped});
  Group grad;
  grad.transform = Matrix::translate(0, 100);
  grad.children.push_back(shape(rect(0, 0, 100, 50), gradient_paint(Gradient::Type::Linear)));
  root(s).children.push_back(Node{grad});
  const auto emf = write("emf", s, R"({"gradient_steps":16})");
  REQUIRE_OK(emf);
  const auto back = read("emf", *emf);
  REQUIRE_OK(back);
  const auto list = items(*back);
  // The translucent square: an AlphaBlend picture with half alpha, clipped to the square.
  const Item* blend = nullptr;
  for (const Item& it : list) {
    if (it.kind == Item::Kind::Picture && it.clipped) blend = &it;
  }
  REQUIRE(blend != nullptr);
  CHECK(near(blend->box, Rect{10, 10, 60, 60}, 0.1));
  const ir::Frame& px = *blend->picture->pixels;
  CHECK_APPROX(static_cast<double>(px.rgba[3]), 128.0, 1.0);
  CHECK_APPROX(static_cast<double>(px.rgba[1]), 153.0, 1.0);
  const Item* cyan = find_fill(list, Color{0, 0.8f, 0.8f, 1});
  REQUIRE(cyan != nullptr);
  CHECK(cyan->clipped);
  // 16 bands from yellow to blue, all clipped to the rectangle.
  std::vector<const Item*> bands;
  for (const Item& it : list) {
    if (it.clipped && it.fill.type == Paint::Type::Solid && it.box.y0 >= 99) bands.push_back(&it);
  }
  REQUIRE(bands.size() == 16);
  CHECK(near(bands.front()->fill.color, Color{1, 1, 0, 1}, 0.05f));
  CHECK(near(bands.back()->fill.color, Color{0, 0, 1, 1}, 0.05f));
  // More steps: a larger file.
  const auto fine = write("emf", s, R"({"gradient_steps":200})");
  REQUIRE_OK(fine);
  CHECK(fine->size() > emf->size());
}

TEST_CASE("vector/emf: text as outlines or as fonts") {
  Scene s = one_page();
  Group g;
  g.transform = Matrix::translate(20, 100) * Matrix::rotate(-std::numbers::pi / 6);
  g.children.push_back(Node{make_text("Text Ab", FontSpec{"sans-serif", 20, 700, false}, {0, 0}, Paint::solid({0, 0, 0.5f, 1}))});
  root(s).children.push_back(Node{g});
  const auto outlines = write("emf", s);
  REQUIRE_OK(outlines);
  const auto a = read("emf", *outlines);
  REQUIRE_OK(a);
  for (const Item& it : items(*a)) CHECK(it.kind == Item::Kind::Shape);
  CHECK(!items(*a).empty());
  const auto fonts = write("emf", s, R"({"metafile_text":"fonts"})");
  REQUIRE_OK(fonts);
  const auto b = read("emf", *fonts);
  REQUIRE_OK(b);
  const auto list = items(*b);
  REQUIRE(list.size() == 1);
  CHECK(list[0].text == "Text Ab");
  CHECK(list[0].font.family == "Arial");
  CHECK(list[0].font.weight == 700);
  CHECK_APPROX(list[0].font.size, 20.0, 0.1);
  CHECK_APPROX(std::atan2(list[0].m.b, list[0].m.a), -std::numbers::pi / 6, 0.002);
  CHECK(near(list[0].fill.color, Color{0, 0, 0.5f, 1}));
  CHECK(fonts->size() < outlines->size());
  CHECK_STATUS(write("emf", s, R"({"metafile_text":"bitmaps"})"), Status::InvalidArgument);
}

TEST_CASE("vector/emf: pictures keep their pixels; transparent and rotated ones are blended") {
  Scene s = one_page();
  Picture opaque;
  opaque.pixels = checker(5, 3, false);
  opaque.bounds = Rect::xywh(10, 10, 50, 30);
  root(s).children.push_back(Node{opaque});
  Picture alpha;
  alpha.pixels = checker(4, 4, true);
  alpha.bounds = Rect::xywh(100, 10, 40, 40);
  root(s).children.push_back(Node{alpha});
  Group turned;
  turned.transform = Matrix::translate(200, 100) * Matrix::rotate(0.5);
  Picture r = opaque;
  r.bounds = Rect::xywh(0, 0, 50, 30);
  turned.children.push_back(Node{r});
  root(s).children.push_back(Node{turned});
  const auto emf = write("emf", s);
  REQUIRE_OK(emf);
  const auto back = read("emf", *emf);
  REQUIRE_OK(back);
  const auto list = items(*back);
  REQUIRE(list.size() == 3);
  REQUIRE(list[0].kind == Item::Kind::Picture);
  CHECK(near(list[0].box, Rect{10, 10, 60, 40}, 0.07));
  CHECK(list[0].picture->pixels->rgba == opaque.pixels->rgba);  // 24-bit: exact
  REQUIRE(list[1].kind == Item::Kind::Picture);
  const ir::Frame& a = *list[1].picture->pixels;
  REQUIRE(a.width == 4);
  for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
    CHECK(a.rgba[i + 3] == alpha.pixels->rgba[i + 3]);
    if (alpha.pixels->rgba[i + 3] > 100) CHECK_APPROX(static_cast<double>(a.rgba[i]), alpha.pixels->rgba[i], 2.0);
  }
  REQUIRE(list[2].kind == Item::Kind::Picture);
  const Rect turned_box = transform_rect(Rect::xywh(0, 0, 50, 30), turned.transform);
  CHECK(near(list[2].box, turned_box, 0.2));
  const ir::Frame& t = *list[2].picture->pixels;
  CHECK(t.rgba[3] == 0);  // corners outside the rotated picture are transparent
}

TEST_CASE("vector/wmf: writer round trip with a placeable header") {
  Scene s = one_page(300, 200);
  Stroke st;
  st.paint = Paint::solid({0, 0, 1, 1});
  st.width = 4;
  root(s).children.push_back(shape(rect(10, 10, 80, 50), Paint::solid({1, 0, 0, 1}), st));
  Path curve;
  curve.move_to({10, 150});
  curve.cubic_to({50, 100}, {90, 200}, {130, 150});
  Stroke dashed;
  dashed.paint = Paint::solid({0.5f, 0, 0.5f, 1});
  dashed.width = 3;
  dashed.dash = {10, 5};
  root(s).children.push_back(shape(curve, Paint::none(), dashed));
  Group grad;
  grad.transform = Matrix::translate(200, 10);
  grad.children.push_back(shape(rect(0, 0, 100, 50), gradient_paint(Gradient::Type::Linear)));
  root(s).children.push_back(Node{grad});
  root(s).children.push_back(shape(rect(150, 100, 40, 40), Paint::solid({0, 0.6f, 0, 1}, 0.5)));
  Group clipped;
  Path circle;
  add_ellipse(circle, 250, 150, 30, 30);
  auto clip = std::make_shared<ClipPath>();
  clip->items.push_back({circle, FillRule::NonZero});
  clipped.clip = clip;
  clipped.children.push_back(shape(rect(200, 100, 100, 100), Paint::solid({0, 0.8f, 0.8f, 1})));
  root(s).children.push_back(Node{clipped});
  Picture pic;
  pic.pixels = checker(4, 4, true);
  pic.bounds = Rect::xywh(20, 160, 30, 30);
  root(s).children.push_back(Node{pic});
  std::vector<std::string> warnings;
  const auto wmf = write("wmf", s, {}, &warnings);
  REQUIRE_OK(wmf);
  CHECK(u32_at(*wmf, 0) == 0x9AC6CDD7u);
  CHECK(!warnings.empty());  // translucency, rectangle-only clipping, picture alpha
  const auto again = write("wmf", s);
  REQUIRE_OK(again);
  CHECK(*again == *wmf);
  const auto back = read("wmf", *wmf);
  REQUIRE_OK(back);
  CHECK_APPROX(back->pages[0].width, 300.0, 0.1);
  CHECK_APPROX(back->pages[0].height, 200.0, 0.1);
  const auto list = items(*back);
  const Item* red = find_fill(list, Color{1, 0, 0, 1});
  REQUIRE(red != nullptr);
  CHECK(near(red->box, Rect{10, 10, 90, 60}, 0.1));
  CHECK_APPROX(red->stroke.width, 4.0, 0.1);
  std::size_t dashes = 0, bands = 0, pictures = 0;
  for (const Item& it : list) {
    if (near(it.stroke.paint.color, Color{0.5f, 0, 0.5f, 1}, 0.01f)) ++dashes;
    if (it.fill.type == Paint::Type::Solid && it.box.x0 >= 199 && it.box.y1 <= 61) ++bands;
    if (it.kind == Item::Kind::Picture) ++pictures;
  }
  CHECK(dashes > 5);    // dashes are separate polylines in WMF
  CHECK(bands >= 32);   // gradient bands cut to the shape
  CHECK(pictures == 2);  // transparent picture: AND mask then OR colours
  CHECK(find_fill(list, Color{0.5f, 0.8f, 0.5f, 1}, 0.01f) != nullptr);  // 50% green on white
  const Item* cyan = find_fill(list, Color{0, 0.8f, 0.8f, 1});
  REQUIRE(cyan != nullptr);
  CHECK(cyan->clipped);
}

TEST_CASE("vector/wmf: writer text as fonts falls back to outlines outside Windows-1252") {
  Scene s = one_page();
  root(s).children.push_back(Node{make_text("Café", FontSpec{"serif", 24, 400, true}, {10, 50}, Paint::solid({1, 0, 0, 1}))});
  root(s).children.push_back(Node{make_text("Жук", FontSpec{"serif", 24, 400, false}, {10, 100}, Paint::solid({1, 0, 0, 1}))});
  const auto wmf = write("wmf", s, R"({"metafile_text":"fonts"})");
  REQUIRE_OK(wmf);
  const auto back = read("wmf", *wmf);
  REQUIRE_OK(back);
  std::size_t texts = 0, shapes = 0;
  for (const Item& it : items(*back)) {
    if (it.kind == Item::Kind::Text) {
      ++texts;
      CHECK(it.text == "Café");
      CHECK(it.font.family == "Times New Roman");
      CHECK(it.font.italic);
      CHECK_APPROX(it.font.size, 24.0, 0.1);
    } else {
      ++shapes;
    }
  }
  CHECK(texts == 1);
  CHECK(shapes >= 1);  // the Cyrillic word as one polygon set
}

TEST_CASE("vector/emz: gzip wrapping without name or time, compression levels") {
  Scene s = one_page();
  for (int i = 0; i < 40; ++i) root(s).children.push_back(shape(rect(i * 5, i * 3, 20, 20), Paint::solid({1, 0, 0, 1})));
  const auto emz = write("emz", s);
  REQUIRE_OK(emz);
  REQUIRE(emz->size() > 10);
  CHECK((*emz)[0] == std::byte{0x1F});
  CHECK((*emz)[3] == std::byte{0});  // no name, comment or extra field
  CHECK(u32_at(*emz, 4) == 0);       // no modification time
  const auto plain = util::gzip_decompress(*emz);
  REQUIRE_OK(plain);
  const auto emf = write("emf", s);
  REQUIRE_OK(emf);
  CHECK(*plain == *emf);
  const auto stored = write("emz", s, R"({"compression_level":0})");
  REQUIRE_OK(stored);
  CHECK(stored->size() > emz->size());
  const auto back = read("emz", *emz);
  REQUIRE_OK(back);
  CHECK(items(*back).size() == 40);
  const auto wmz = write("wmz", s);
  REQUIRE_OK(wmz);
  const auto wmf_back = read("wmz", *wmz);
  REQUIRE_OK(wmf_back);
  CHECK(items(*wmf_back).size() == 40);
}

TEST_CASE("vector/metafile: conversions through the engine") {
  const Bytes emf = test::fixture("vector/metafile/lo_text.emf");
  const auto same = test::convert(emf, "", "emf", "", "text.emf");
  REQUIRE(same.ok());
  CHECK(same.format_id == "emf");
  const auto to_wmf = test::convert(emf, "emf", "wmf");
  REQUIRE(to_wmf.ok());
  const auto back = read("wmf", to_wmf.data);
  REQUIRE_OK(back);
  CHECK(!items(*back).empty());
  const auto to_emz = test::convert(test::fixture("vector/metafile/ink_shapes.wmf"), "", "emz");
  REQUIRE(to_emz.ok());
  CHECK(test::convert(to_emz.data, "", "wmf", "", "x.emz").ok());
  CHECK(test::convert(test::fixture("vector/metafile/ink_shapes.wmz"), "", "emf", "", "x.wmz").ok());
}

TEST_CASE("vector/metafile: writer limits and empty drawings") {
  Scene empty;
  CHECK_STATUS(write("emf", empty), Status::Encode);
  Scene zero = one_page(0, 0);
  CHECK_STATUS(write("wmf", zero), Status::Encode);
  Scene s = one_page();
  Picture pic;
  pic.pixels = checker(64, 64, false);
  pic.bounds = Rect::xywh(0, 0, 100, 100);
  root(s).children.push_back(Node{pic});
  Limits tiny;
  tiny.max_output_bytes = 1000;
  CHECK_STATUS(write("emf", s, {}, nullptr, tiny), Status::Limit);
  // A huge page still writes (WMF scales its units down to fit 16 bits).
  Scene huge = one_page(100000, 50000);
  root(huge).children.push_back(shape(rect(0, 0, 100000, 50000), Paint::solid({0, 0, 0, 1})));
  const auto wmf = write("wmf", huge);
  REQUIRE_OK(wmf);
  const auto back = read("wmf", *wmf);
  REQUIRE_OK(back);
  CHECK_APPROX(back->pages[0].width, 100000.0, 100.0);
}
